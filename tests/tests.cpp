#include "nitro/collector.hpp"
#include "nitro/metrics.hpp"
#include "nitro/netlink.hpp"
#include "nitro/options.hpp"
#include "nitro/sampler.hpp"
#include "nitro/trace.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <linux/netlink.h>
#include <random>
#include <rdma/rdma_netlink.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace nitro;
using namespace std::chrono_literals;
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression))                                                                         \
            throw std::runtime_error(std::string{__FILE__} + ":" + std::to_string(__LINE__) +      \
                                     ": " #expression);                                            \
    } while (false)
void near(double actual, double expected) {
    CHECK(std::abs(actual - expected) < 1e-7 * std::max(1.0, std::abs(expected)));
}
template <typename F> void throws(F fn) {
    bool caught = false;
    try {
        fn();
    } catch (const std::exception&) {
        caught = true;
    }
    CHECK(caught);
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        std::array<char, 40> pattern{};
        std::string base = "/tmp/nitro-test-XXXXXX";
        std::copy(base.begin(), base.end(), pattern.begin());
        const auto* dir = ::mkdtemp(pattern.data());
        if (!dir)
            throw std::runtime_error("mkdtemp failed");
        path = dir;
    }
    ~Temp() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    void write(const std::string& relative, const std::string& content) const {
        const auto file = path / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << content;
    }
};
Sample sample(double seconds, Counters counters, std::string device = "mlx5_0") {
    Sample result;
    result.taken_at = Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(
                                                std::chrono::duration<double>{seconds});
    Port port;
    port.device = std::move(device);
    port.counters = std::move(counters);
    port.taken_at = result.taken_at;
    port.source = "test";
    result.ports.push_back(std::move(port));
    return result;
}
template <typename T> void integer(nitro::netlink::Bytes& bytes, std::uint16_t type, T value) {
    netlink::append_attribute(bytes, type, std::as_bytes(std::span{&value, 1}));
}
void string_attr(netlink::Bytes& bytes, std::uint16_t type, const std::string& value) {
    netlink::append_attribute(bytes, type,
                              std::as_bytes(std::span{value.c_str(), value.size() + 1}));
}
void metrics_rates() {
    Metrics metrics;
    auto baseline =
        metrics.update(sample(1, {{"tx_bytes", 1000}, {"rx_bytes", 100}, {"tx_pkts", 20}}));
    CHECK(!baseline.ports[0].tx_gbps());
    const auto next = metrics.update(
        sample(3, {{"tx_bytes", 250001000}, {"rx_bytes", 125000100}, {"tx_pkts", 120}}));
    near(*next.ports[0].tx_gbps(), 1);
    near(*next.ports[0].rx_gbps(), .5);
    near(*next.ports[0].rate("tx_pkts"), 50);
    CHECK(!next.ports[0].rate("rx_drops"));
}
void resets_and_gaps() {
    Metrics metrics;
    (void)metrics.update(sample(1, {{"tx_bytes", 100}}));
    auto missing = metrics.update(sample(2, {}));
    CHECK(!missing.ports[0].tx_gbps());
    auto recovered = metrics.update(sample(3, {{"tx_bytes", 300}, {"rx_bytes", 900000000}}));
    near(*recovered.ports[0].rate("tx_bytes"), 100);
    CHECK(!recovered.ports[0].rx_gbps());
    auto reset = metrics.update(sample(4, {{"tx_bytes", 2}}));
    CHECK(!reset.ports[0].rate("tx_bytes"));
    CHECK(reset.ports[0].rates.at("tx_bytes").reset);
    auto next = metrics.update(sample(5, {{"tx_bytes", 12}}));
    near(*next.ports[0].rate("tx_bytes"), 10);
}
void timestamps_and_hotplug() {
    Metrics metrics;
    (void)metrics.update(sample(2, {{"tx_bytes", 100}}));
    auto duplicate = metrics.update(sample(2, {{"tx_bytes", 1000}}));
    CHECK(!duplicate.ports[0].tx_gbps());
    auto reversed = metrics.update(sample(1, {{"tx_bytes", 4000}}));
    CHECK(!reversed.ports[0].tx_gbps());
    auto next = metrics.update(sample(3, {{"tx_bytes", 200}}));
    near(*next.ports[0].rate("tx_bytes"), 100);
    CHECK(next.ports[0].history.size() == 2);
    auto empty = sample(4, {});
    empty.ports.clear();
    (void)metrics.update(std::move(empty));
    auto replug = metrics.update(sample(5, {{"tx_bytes", 9000000}}));
    CHECK(!replug.ports[0].tx_gbps());
}
void source_and_history() {
    Metrics metrics;
    for (int i = 0; i < 1000; ++i)
        (void)metrics.update(sample(i, {{"tx_bytes", static_cast<std::uint64_t>(i)}}));
    auto next = metrics.update(sample(1000, {{"tx_bytes", 1000}}));
    CHECK(next.ports[0].history.size() == history_limit);
    auto changed = sample(1001, {{"tx_bytes", 99999999}});
    changed.ports[0].source = "other";
    CHECK(!metrics.update(std::move(changed)).ports[0].tx_gbps());
}
void multi_port_identity() {
    Metrics metrics;
    auto a = sample(1, {{"tx_bytes", 100}});
    a.ports.push_back(a.ports[0]);
    a.ports[1].number = 2;
    a.ports[1].counters["tx_bytes"] = 900;
    (void)metrics.update(a);
    a.taken_at += 1s;
    for (auto& port : a.ports) {
        port.taken_at += 1s;
        port.counters["tx_bytes"] += port.number * 10;
    }
    std::reverse(a.ports.begin(), a.ports.end());
    const auto b = metrics.update(a);
    near(*b.ports[0].rate("tx_bytes"), 20);
    near(*b.ports[1].rate("tx_bytes"), 10);
}
void weighted_average() {
    const auto time = Clock::time_point{};
    const std::deque<TrafficPoint> history{
        {time, std::nullopt, std::nullopt}, {time + 1s, 10, 20}, {time + 4s, 30, std::nullopt}};
    near(*average_rate(history, true, 10s), 25);
    near(*average_rate(history, true, 2s), 30);
    near(*average_rate(history, false, 10s), 20);
    CHECK(!average_rate(history, false, 2s));
}
void aliases() {
    Counters ib{{"port_xmit_data", 50},
                {"port_rcv_data", 80},
                {"port_xmit_packets", 9},
                {"port_rcv_packets", 8},
                {"port_rcv_errors", 99}};
    normalize_counters(ib);
    CHECK(ib.at("tx_bytes") == 200);
    CHECK(ib.at("rx_bytes") == 320);
    CHECK(ib.at("port_xmit_data") == 50);
    CHECK(!ib.contains("rx_drops"));
    normalize_counters(ib);
    CHECK(ib.at("tx_bytes") == 200);
    Counters provider{
        {"tx_bytes", 500}, {"hw_tx_bytes_cnt", 10}, {"port_xmit_data", 1000}, {"rx_bytes_cnt", 42}};
    normalize_counters(provider);
    CHECK(provider.at("tx_bytes") == 500);
    CHECK(provider.at("rx_bytes") == 42);
    Counters overflow{{"port_xmit_data", UINT64_MAX}};
    normalize_counters(overflow);
    CHECK(!overflow.contains("tx_bytes"));
}
void parse_values() {
    CHECK(parse_unsigned(" 18446744073709551615\n") == UINT64_MAX);
    for (const auto bad : {"-1", "", "1x", "18446744073709551616", "+2"})
        CHECK(!parse_unsigned(bad));
    near(*parse_link_speed("400 Gb/sec (4X NDR)"), 400);
    CHECK(!parse_link_speed("-1"));
    CHECK(!parse_link_speed("NaN"));
    CHECK(!parse_link_speed("bad"));
    CHECK(safe_text("a\x1b[2J\t") == "a?[2J?");
    CHECK(json_escape("a\n\"\\") == "\"a\\u000a\\\"\\\\\"");
    CHECK(format_rate(std::nullopt) == "--");
    CHECK(format_rate(125000000., true) == "125.0MB/s");
}
void netlink_nested() {
    netlink::Bytes entry, nested, payload;
    string_attr(entry, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_NAME, "tx_bytes");
    integer(entry, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_VALUE, std::uint64_t{9876543210123});
    netlink::append_attribute(nested, RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY | NLA_F_NESTED, entry);
    netlink::append_attribute(payload, RDMA_NLDEV_ATTR_STAT_HWCOUNTERS | NLA_F_NESTED, nested);
    CHECK(netlink::parse_counters(payload).at("tx_bytes") == 9876543210123);
    netlink::Bytes device;
    integer(device, RDMA_NLDEV_ATTR_DEV_INDEX, std::uint32_t{7});
    string_attr(device, RDMA_NLDEV_ATTR_DEV_NAME, "mlx5_0");
    integer(device, RDMA_NLDEV_ATTR_PORT_INDEX, std::uint32_t{2});
    const auto parsed = netlink::parse_device(device);
    CHECK(parsed);
    CHECK(parsed->index == 7);
    CHECK(parsed->ports == 2);
    netlink::Bytes invalid;
    integer(invalid, RDMA_NLDEV_ATTR_DEV_INDEX, std::uint32_t{7});
    string_attr(invalid, RDMA_NLDEV_ATTR_DEV_NAME, "../bad");
    throws([&] { (void)netlink::parse_device(invalid); });
}
void netlink_qps() {
    netlink::Bytes qp, table, payload;
    integer(qp, RDMA_NLDEV_ATTR_RES_PID, std::uint32_t{1001});
    integer(qp, RDMA_NLDEV_ATTR_RES_LQPN, std::uint32_t{73});
    integer(qp, RDMA_NLDEV_ATTR_PORT_INDEX, std::uint32_t{2});
    netlink::append_attribute(table, RDMA_NLDEV_ATTR_RES_QP_ENTRY | NLA_F_NESTED, qp);
    netlink::append_attribute(payload, RDMA_NLDEV_ATTR_RES_QP | NLA_F_NESTED, table);
    const auto rows = netlink::parse_queue_pairs(payload);
    CHECK(rows.size() == 1);
    CHECK(rows[0].pid == 1001);
    CHECK(rows[0].number == 73);
    CHECK(rows[0].port == 2);
}
void malformed_netlink() {
    netlink::Bytes good;
    integer(good, 1, std::uint64_t{99});
    for (std::size_t size = 1; size < good.size(); ++size)
        throws([&] { (void)netlink::attributes(std::span{good}.first(size)); });
    netlink::Bytes bad(4, std::byte{0});
    throws([&] { (void)netlink::attributes(bad); });
    const netlink::Attribute short_int{1, std::span{good}.first(3)};
    throws([&] { (void)short_int.integer<std::uint64_t>(); });
    const std::array text{std::byte{'x'}};
    const netlink::Attribute no_null{1, text};
    throws([&] { (void)no_null.string(); });
    std::mt19937 random(42);
    for (std::size_t trial = 0; trial < 10000; ++trial) {
        netlink::Bytes data(random() % 128);
        for (auto& b : data)
            b = static_cast<std::byte>(random() & 255U);
        try {
            (void)netlink::attributes(data);
        } catch (const std::runtime_error&) {
        }
        try {
            (void)netlink::messages(data);
        } catch (const std::runtime_error&) {
        }
    }
}
void netlink_messages() {
    nlmsghdr header{};
    header.nlmsg_len = sizeof(header) + 4;
    header.nlmsg_type = NLMSG_DONE;
    header.nlmsg_seq = 10;
    netlink::Bytes bytes(header.nlmsg_len);
    std::memcpy(bytes.data(), &header, sizeof(header));
    const auto parsed = netlink::messages(bytes);
    CHECK(parsed.size() == 1);
    CHECK(parsed[0].sequence == 10);
    CHECK(parsed[0].payload.size() == 4);
    for (std::size_t i = 1; i < bytes.size(); ++i)
        throws([&] { (void)netlink::messages(std::span{bytes}.first(i)); });
}
void sysfs_fixture() {
    Temp dir;
    const std::string port = "sys/class/infiniband/mlx5_9/ports/1/";
    dir.write(port + "state", "4: ACTIVE\n");
    dir.write(port + "rate", "200 Gb/sec (4X HDR)\n");
    dir.write(port + "link_layer", "InfiniBand\n");
    dir.write(port + "counters/port_xmit_data", "100\n");
    dir.write(port + "counters/port_rcv_packets", "42\n");
    dir.write(port + "hw_counters/rx_bytes", "987\n");
    dir.write(port + "hw_counters/bad", "N/A\n");
    dir.write("proc/net/dev", "header\n eth0: 1000 20 3 4 0 0 0 0 2000 30 5 6 0 0 0 0\n lo: 10 0 0 "
                              "0 0 0 0 0 10 0 0 0 0 0 0 0\n");
    dir.write("sys/class/net/eth0/operstate", "up\n");
    dir.write("sys/class/net/eth0/speed", "10000\n");
    dir.write("proc/meminfo", "MemTotal: 1024 kB\nMemFree: 12 kB\nMemAvailable: 512 kB\n");
    LinuxCollector collector({dir.path / "sys", dir.path / "proc"}, false);
    const auto result = collector.collect();
    CHECK(result.ports.size() == 2);
    CHECK(result.ports[0].state == "ACTIVE");
    near(*result.ports[0].link_gbps, 200);
    CHECK(result.ports[0].counters.at("tx_bytes") == 400);
    CHECK(result.ports[0].counters.at("rx_bytes") == 987);
    CHECK(!result.ports[0].counters.contains("bad"));
    CHECK(result.ports[1].kind == PortKind::network);
    CHECK(result.ports[1].counters.at("tx_bytes") == 2000);
    CHECK(result.ports[1].counters.at("rx_drops") == 4);
    near(*result.ports[1].link_gbps, 10);
    CHECK(result.host.memory_total == 1024 * 1024);
    CHECK(result.host.memory_available == 512 * 1024);
    std::filesystem::remove_all(dir.path / "sys/class/infiniband/mlx5_9");
    CHECK(collector.collect().ports.size() == 1);
}
void proc_fixture() {
    Temp dir;
    std::vector<std::string> fields(22, "0");
    fields[0] = "S";
    fields[16] = "-5";
    fields[17] = "8";
    fields[20] = "9999";
    fields[21] = "7";
    std::string stat = "123 (worker (with) spaces))";
    for (const auto& field : fields)
        stat += " " + field;
    dir.write("proc/123/stat", stat);
    dir.write("proc/123/status", "Uid:\t424242\t424242\t424242\t424242\n");
    dir.write("proc/123/cmdline", std::string{"train\0--rdma\0", 13});
    const auto result = read_process({dir.path / "sys", dir.path / "proc"}, 123);
    CHECK(result.state == "S");
    CHECK(result.nice == -5);
    CHECK(result.threads == 8);
    CHECK(result.virtual_bytes == 9999);
    CHECK(result.rss_bytes == 7 * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)));
    CHECK(result.command.starts_with("train --rdma"));
    CHECK(read_process({dir.path / "sys", dir.path / "proc"}, 999).user == "?");
}
void recorder_files() {
    Temp dir;
    Metrics metrics;
    (void)metrics.update(sample(1, {{"tx_bytes", 0}}));
    auto next = metrics.update(sample(2, {{"tx_bytes", 125000000}}));
    const auto path = dir.path / "trace.json";
    Recorder recorder(path, ExportFormat::trace, Clock::time_point{} + 1s);
    recorder.append(next);
    recorder.finish();
    recorder.finish();
    const auto data = read_text(path);
    CHECK(data);
    CHECK(data->find("\"value\":1") != std::string::npos);
    CHECK(data->find("\"ts\":1000000") != std::string::npos);
    CHECK(data->ends_with("]}\n"));
    throws([&] { Recorder another(path, ExportFormat::trace, Clock::now()); });
    CHECK(read_text(path) == data);
    throws([&] { recorder.append(next); });
    Recorder csv(dir.path / "trace.csv", ExportFormat::csv, Clock::time_point{});
    csv.append(next);
    csv.finish();
    CHECK(read_text(dir.path / "trace.csv")->find("125000000,125000000,0") != std::string::npos);
    std::filesystem::create_symlink(path, dir.path / "link.json");
    throws([&] { Recorder link(dir.path / "link.json", ExportFormat::trace, Clock::now()); });
}
void recording_destructor() {
    Temp dir;
    { Recorder recorder(dir.path / "empty.json", ExportFormat::trace, Clock::now()); }
    CHECK(read_text(dir.path / "empty.json")->ends_with("]}\n"));
}
void options_and_filter() {
    const std::array<std::string_view, 7> args{"--demo",   "--json",         "--interval", "100",
                                               "--device", "mlx5_0/1,efa_0", "--ascii"};
    const auto options = parse_options(args);
    CHECK(options.demo && options.headless && options.ascii);
    CHECK(options.count == 1);
    CHECK(options.interval_ms == 100);
    for (const auto bad : {"0", "99", "60001", "abc", "-1"}) {
        const std::array<std::string_view, 2> invalid{"--interval", bad};
        throws([&] { (void)parse_options(invalid); });
    }
    const std::array<std::string_view, 2> nan{"--duration", "nan"};
    throws([&] { (void)parse_options(nan); });
    const std::array<std::string_view, 1> missing{"--device"};
    throws([&] { (void)parse_options(missing); });
    auto current = sample(1, {});
    current.ports.push_back(current.ports.front());
    current.ports[1].number = 2;
    filter_sample(current, options.devices);
    CHECK(current.ports.size() == 1);
    CHECK(current.ports[0].number == 1);
}
void demo_monotonic() {
    DemoCollector demo;
    const auto now = Clock::now();
    const auto a = demo.at(now), b = demo.at(now + 1s);
    CHECK(a.demo && a.ports.size() == 7 && a.processes.size() == 5);
    for (std::size_t i = 0; i < a.ports.size(); ++i)
        for (const auto& [name, value] : a.ports[i].counters)
            CHECK(b.ports[i].counters.at(name) >= value);
    Metrics metrics;
    (void)metrics.update(a);
    const auto view = metrics.update(b);
    CHECK(*view.ports[0].tx_gbps() > 0);
    CHECK(view.ports[3].tx_gbps() == 0);
}
void sampler_mailbox_and_recording() {
    Temp dir;
    Options options;
    options.demo = true;
    options.interval_ms = 100;
    const auto start = Clock::now();
    {
        Sampler sampler(options);
        auto wait_for = [&](const auto& predicate) {
            const auto deadline = Clock::now() + 3s;
            while (Clock::now() < deadline) {
                auto latest = sampler.latest();
                if (latest && predicate(*latest))
                    return;
                std::this_thread::sleep_for(5ms);
            }
            throw std::runtime_error("Sampler timed out");
        };
        wait_for([](const auto& s) { return s.sequence > 0; });
        CHECK(sampler.latest()->ports[0].history.size() == history_limit);
        const auto old = sampler.latest();
        sampler.record((dir.path / "trace.json").string());
        wait_for([](const auto& s) { return s.recording; });
        sampler.interval(100);
        wait_for([&](const auto& s) { return s.sequence > old->sequence + 2; });
        CHECK(old->ports[0].history.size() == history_limit); // Published frames are immutable.
        sampler.record("");
        wait_for([](const auto& s) { return !s.recording; });
        CHECK(read_text(dir.path / "trace.json")->ends_with("]}\n"));
    }
    CHECK(Clock::now() - start < 4s);
}
} // namespace
int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"measured rates and warmup", metrics_rates},
        {"counter resets and read gaps", resets_and_gaps},
        {"timestamps and hotplug", timestamps_and_hotplug},
        {"source changes and bounded history", source_and_history},
        {"device and port identity", multi_port_identity},
        {"time-weighted average", weighted_average},
        {"driver aliases and IB units", aliases},
        {"numeric and terminal input", parse_values},
        {"nested netlink counters", netlink_nested},
        {"netlink QP ownership", netlink_qps},
        {"malformed netlink and deterministic fuzz", malformed_netlink},
        {"netlink message framing", netlink_messages},
        {"sysfs and procfs collector fixture", sysfs_fixture},
        {"process names and native page sizes", proc_fixture},
        {"streaming trace and CSV files", recorder_files},
        {"RAII recording finalization", recording_destructor},
        {"CLI parsing and filters", options_and_filter},
        {"deterministic demo", demo_monotonic},
        {"background sampling and recording", sampler_mailbox_and_recording}};
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    std::cout << tests.size() - static_cast<std::size_t>(failures) << '/' << tests.size()
              << " passed\n";
    return failures == 0 ? 0 : 1;
}
