#include "nitro/collector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <pwd.h>
#include <rdma/rdma_netlink.h>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace nitro {
namespace fs = std::filesystem;
namespace {
std::vector<fs::path> directories(const fs::path& path) {
    std::vector<fs::path> result;
    std::error_code error;
    for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error))
            result.push_back(it->path());
    }
    std::sort(result.begin(), result.end());
    return result;
}
void read_counters(const fs::path& path, Counters& counters) {
    std::error_code error;
    for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (counters.contains(name) || name == "lifespan")
            continue;
        const auto raw = read_text(it->path());
        if (raw) {
            if (const auto value = parse_unsigned(*raw))
                counters.emplace(name, *value);
        }
    }
}
std::string field(const fs::path& path, std::string fallback = {}) {
    const auto data = read_text(path);
    return data ? trim(*data) : std::move(fallback);
}
std::uint64_t safe_multiply(std::uint64_t value, std::uint64_t factor) {
    return value <= std::numeric_limits<std::uint64_t>::max() / factor ? value * factor : 0;
}
} // namespace
std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string result;
    std::array<char, 4096> buffer{};
    while (in && result.size() < std::size_t{1024} * 1024) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        result.append(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad())
        return std::nullopt;
    return result;
}
std::optional<double> parse_link_speed(std::string_view text) {
    std::istringstream in{std::string{text}};
    in.imbue(std::locale::classic());
    double result{};
    if (in >> result && std::isfinite(result) && result > 0)
        return result;
    return std::nullopt;
}
Process read_process(const Paths& paths, std::uint32_t pid) {
    Process result;
    result.pid = pid;
    if (pid == 0) {
        result.command = "[kernel]";
        result.user = "kernel";
        return result;
    }
    const auto base = paths.proc / std::to_string(pid);
    result.command = field(base / "cmdline");
    std::replace(result.command.begin(), result.command.end(), '\0', ' ');
    result.command = trim(result.command);
    if (result.command.empty())
        result.command = field(base / "comm", "[exited or restricted]");
    if (const auto raw = read_text(base / "stat")) {
        const auto close = raw->rfind(')');
        if (close != std::string::npos) {
            std::istringstream in(raw->substr(close + 1));
            std::vector<std::string> parts;
            for (std::string part; in >> part;)
                parts.push_back(part);
            if (parts.size() >= 22) {
                result.state = parts[0];
                std::istringstream(parts[16]) >> result.nice;
                result.threads = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(parse_unsigned(parts[17]).value_or(0), UINT32_MAX));
                result.virtual_bytes = parse_unsigned(parts[20]).value_or(0);
                const auto page_size = ::sysconf(_SC_PAGESIZE);
                if (page_size > 0)
                    result.rss_bytes = safe_multiply(parse_unsigned(parts[21]).value_or(0),
                                                     static_cast<std::uint64_t>(page_size));
            }
        }
    }
    if (const auto raw = read_text(base / "status")) {
        std::istringstream in(*raw);
        for (std::string line; std::getline(in, line);) {
            if (!line.starts_with("Uid:"))
                continue;
            std::istringstream fields(line.substr(4));
            std::uint32_t uid{};
            if (fields >> uid) {
                result.user = std::to_string(uid);
                std::array<char, 16384> buffer{};
                passwd entry{};
                passwd* found = nullptr;
                if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found)
                    result.user = entry.pw_name;
            }
            break;
        }
    }
    return result;
}
LinuxCollector::LinuxCollector(Paths paths, bool use_netlink)
    : paths_(std::move(paths)), use_netlink_(use_netlink) {}
Host LinuxCollector::read_host() const {
    Host host;
    utsname info{};
    if (::uname(&info) == 0) {
        host.name = info.nodename;
        host.kernel = info.release;
    }
    std::istringstream(field(paths_.proc / "uptime")) >> host.uptime_seconds;
    std::istringstream(field(paths_.proc / "loadavg")) >> host.load;
    std::istringstream mem(field(paths_.proc / "meminfo"));
    for (std::string line; std::getline(mem, line);) {
        std::string name;
        std::uint64_t kb{};
        std::istringstream fields(line);
        if (!(fields >> name >> kb))
            continue;
        if (name == "MemTotal:")
            host.memory_total = safe_multiply(kb, 1024);
        if (name == "MemAvailable:")
            host.memory_available = safe_multiply(kb, 1024);
    }
    return host;
}
std::vector<Port> LinuxCollector::read_network(std::vector<std::string>& warnings) const {
    std::vector<Port> result;
    const auto raw = read_text(paths_.proc / "net/dev");
    if (!raw) {
        warnings.emplace_back("Network counters unavailable: cannot read proc/net/dev");
        return result;
    }
    std::istringstream in(*raw);
    for (std::string line; std::getline(in, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        Port port;
        port.device = trim(line.substr(0, colon));
        if (port.device == "lo" || port.device.empty() ||
            port.device.find('/') != std::string::npos)
            continue;
        port.kind = PortKind::network;
        port.number = 0;
        port.source = "procfs";
        port.link_layer = "Ethernet";
        std::array<std::uint64_t, 16> values{};
        std::istringstream fields(line.substr(colon + 1));
        bool valid = true;
        for (auto& value : values)
            if (!(fields >> value)) {
                valid = false;
                break;
            }
        if (!valid)
            continue;
        port.counters = {{"rx_bytes", values[0]},   {"rx_pkts", values[1]},
                         {"rx_errors", values[2]},  {"rx_drops", values[3]},
                         {"tx_bytes", values[8]},   {"tx_pkts", values[9]},
                         {"tx_errors", values[10]}, {"tx_drops", values[11]}};
        const auto path = paths_.sys / "class/net" / port.device;
        port.state = field(path / "operstate", "unknown");
        if (const auto speed = parse_link_speed(field(path / "speed")))
            port.link_gbps = *speed / 1000;
        port.taken_at = Clock::now();
        result.push_back(std::move(port));
    }
    return result;
}
Sample LinuxCollector::collect(std::stop_token stop) {
    Sample sample;
    sample.host = read_host();
    std::map<std::string, netlink::Device> devices;
    std::unique_ptr<netlink::Socket> socket;
    if (use_netlink_) {
        try {
            socket = std::make_unique<netlink::Socket>();
            for (const auto& response : socket->request(RDMA_NLDEV_CMD_GET, true)) {
                if (auto device = netlink::parse_device(response))
                    devices.emplace(device->name, *device);
            }
        } catch (const std::exception& e) {
            sample.warnings.emplace_back(std::string{"RDMA netlink: "} + e.what() +
                                         "; using sysfs");
            socket.reset();
        }
    }
    const auto base = paths_.sys / "class/infiniband";
    std::map<std::string, std::vector<std::uint32_t>> ports;
    for (const auto& [name, device] : devices)
        for (std::uint32_t number = 1; number <= device.ports; ++number)
            ports[name].push_back(number);
    for (const auto& device : directories(base)) {
        const auto name = device.filename().string();
        for (const auto& dir : directories(device / "ports")) {
            if (const auto number = parse_unsigned(dir.filename().string());
                number && *number <= 255) {
                auto& list = ports[name];
                if (std::find(list.begin(), list.end(), *number) == list.end())
                    list.push_back(static_cast<std::uint32_t>(*number));
            }
        }
    }
    for (const auto& [name, numbers] : ports) {
        for (const auto number : numbers) {
            if (stop.stop_requested())
                return sample;
            Port port;
            port.device = name;
            port.number = number;
            port.source = "sysfs";
            const auto path = base / name / "ports" / std::to_string(number);
            port.state = field(path / "state", "UNKNOWN");
            if (const auto colon = port.state.find(':'); colon != std::string::npos)
                port.state = trim(port.state.substr(colon + 1));
            port.link_layer = field(path / "link_layer", "RDMA");
            port.link_gbps = parse_link_speed(field(path / "rate"));
            if (socket && devices.contains(name)) {
                try {
                    const std::array values{std::pair<std::uint16_t, std::uint32_t>{
                                                RDMA_NLDEV_ATTR_DEV_INDEX, devices.at(name).index},
                                            std::pair<std::uint16_t, std::uint32_t>{
                                                RDMA_NLDEV_ATTR_PORT_INDEX, number}};
                    for (const auto& response :
                         socket->request(RDMA_NLDEV_CMD_STAT_GET, false, values))
                        port.counters.merge(netlink::parse_counters(response));
                    if (!port.counters.empty())
                        port.source = "netlink + sysfs";
                } catch (const std::system_error& e) {
                    // Unsupported per-port statistics are normal for several drivers.
                    if (e.code().value() != EOPNOTSUPP && e.code().value() != EINVAL)
                        sample.warnings.emplace_back(name + ": " + e.what() + "; using sysfs");
                } catch (const std::exception& e) {
                    sample.warnings.emplace_back(name + ": " + e.what());
                }
            }
            read_counters(path / "hw_counters", port.counters);
            read_counters(path / "counters", port.counters);
            normalize_counters(port.counters);
            if (port.counters.empty())
                sample.warnings.emplace_back(name + "/" + std::to_string(number) +
                                             ": counters unavailable (driver or permissions)");
            port.taken_at = Clock::now();
            sample.ports.push_back(std::move(port));
        }
    }
    if (Clock::now() - processes_at_ >= std::chrono::seconds{3}) {
        processes_.clear();
        process_warnings_.clear();
        std::map<std::uint32_t, Process> cache;
        for (const auto& [name, device] : devices) {
            if (!socket || stop.stop_requested())
                break;
            try {
                const std::array values{std::pair<std::uint16_t, std::uint32_t>{
                    RDMA_NLDEV_ATTR_DEV_INDEX, device.index}};
                std::map<std::uint32_t, std::size_t> counts;
                for (const auto& response :
                     socket->request(RDMA_NLDEV_CMD_RES_QP_GET, true, values))
                    for (const auto& qp : netlink::parse_queue_pairs(response))
                        ++counts[qp.pid];
                for (const auto& [pid, count] : counts) {
                    if (!cache.contains(pid))
                        cache.emplace(pid, read_process(paths_, pid));
                    auto process = cache.at(pid);
                    process.device = name;
                    process.qp_count = count;
                    processes_.push_back(std::move(process));
                }
            } catch (const std::exception& e) {
                process_warnings_.emplace_back(name +
                                               ": process ownership unavailable: " + e.what());
            }
        }
        if (!socket && !ports.empty())
            process_warnings_.emplace_back(
                "Process ownership requires RDMA netlink; sysfs counters remain available");
        processes_at_ = Clock::now();
    }
    sample.processes = processes_;
    sample.warnings.insert(sample.warnings.end(), process_warnings_.begin(),
                           process_warnings_.end());
    auto network = read_network(sample.warnings);
    sample.ports.insert(sample.ports.end(), std::make_move_iterator(network.begin()),
                        std::make_move_iterator(network.end()));
    sample.taken_at = Clock::now();
    return sample;
}
DemoCollector::DemoCollector() : epoch_(Clock::now() - std::chrono::seconds{600}) {}
Sample DemoCollector::collect(std::stop_token) {
    return at(Clock::now());
}
Sample DemoCollector::at(Clock::time_point time) const {
    Sample sample;
    sample.demo = true;
    sample.taken_at = time;
    const double seconds = std::max(0.0, std::chrono::duration<double>(time - epoch_).count());
    sample.host = {"fabric-lab", "6.8.0-nitro", seconds + 86400 * 3,
                   4.27,         512ULL << 30,  337ULL << 30};
    for (std::size_t i = 0; i < 7; ++i) {
        const double offset = static_cast<double>(i) * 1.8;
        // Integral of a positive, varying rate: deterministic monotonic counters.
        const auto total = [&](double base, double phase) {
            const double integral =
                base * (seconds + 0.24 * 9 * (std::sin(seconds / 9 + phase) - std::sin(phase)) +
                        0.13 * 2.4 * (std::sin(seconds / 2.4 + phase) - std::sin(phase)));
            return static_cast<std::uint64_t>(std::max(0.0, integral));
        };
        Port port;
        port.device = i < 4   ? "mlx5_" + std::to_string(i)
                      : i < 6 ? "efa_" + std::to_string(i - 4)
                              : "enp65s0";
        port.number = i < 6 ? 1 : 0;
        port.kind = i < 6 ? PortKind::rdma : PortKind::network;
        port.state = i == 3 ? "DOWN" : "ACTIVE";
        port.link_layer = i < 4 ? "InfiniBand" : "Ethernet";
        port.link_gbps = i < 4 ? 400 : 100;
        port.source = "DEMO";
        port.taken_at = time;
        const double base = i == 3 ? 0 : i < 4 ? 18e9 - static_cast<double>(i) * 2e9 : 4e9;
        port.counters = {{"tx_bytes", total(base, offset)},
                         {"rx_bytes", total(base * 0.72, offset + 2)},
                         {"tx_pkts", total(base / 4096, offset)},
                         {"rx_pkts", total(base * 0.72 / 4096, offset + 2)},
                         {"rx_drops", 0},
                         {"rdma_read_bytes", total(base * 0.32, offset)},
                         {"rdma_write_bytes", total(base * 0.6, offset)},
                         {"rdma_read_wrs", total(base / 65536, offset)},
                         {"rdma_write_wrs", total(base / 32768, offset)},
                         {"retrans_pkts", total(i == 2 ? 12 : 0, offset)},
                         {"port_rcv_errors", 0},
                         {"link_downed", i == 3 ? 1U : 0U},
                         {"port_xmit_wait", total(i == 2 ? 23 : 0, offset)}};
        sample.ports.push_back(std::move(port));
    }
    constexpr std::array commands{
        "python train.py --model llama-3 --world-size 8", "all_reduce_perf -b 8M -e 4G -g 8",
        "python -m vllm.entrypoints.openai.api_server", "ib_write_bw -d mlx5_0 --report_gbits"};
    for (std::size_t i = 0; i < 5; ++i) {
        Process p;
        p.pid = 18240 + static_cast<std::uint32_t>(i) * 21;
        p.device = sample.ports[i == 3 ? 4 : i].device;
        p.user = "research";
        p.command = commands[i % commands.size()];
        p.state = "S";
        p.rss_bytes = (4 + i * 2) * (1ULL << 30);
        p.virtual_bytes = p.rss_bytes * 2;
        p.threads = 32;
        p.qp_count = 32 + i * 16;
        sample.processes.push_back(std::move(p));
    }
    return sample;
}
std::unique_ptr<Collector> make_collector(bool demo, Paths paths, bool netlink) {
    if (demo)
        return std::make_unique<DemoCollector>();
    return std::make_unique<LinuxCollector>(std::move(paths), netlink);
}
} // namespace nitro
