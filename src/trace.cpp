#include "nitro/trace.hpp"
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>
#include <unistd.h>
namespace nitro {
namespace {
std::string number(std::optional<double> value) {
    if (!value || !std::isfinite(*value))
        return "null";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << *value;
    return out.str();
}
std::string csv_quote(std::string_view value) {
    std::string result{"\""};
    for (char c : value) {
        if (c == '"')
            result += '"';
        result += c;
    }
    return result + '"';
}
} // namespace
Recorder::Recorder(const std::filesystem::path& path, ExportFormat format, Clock::time_point start)
    : format_(format), start_(start) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd_ < 0)
        throw std::system_error(errno, std::generic_category(),
                                "Create recording " + path.string());
    try {
        write(format_ == ExportFormat::trace
                  ? "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n"
                  : "timestamp_us,kind,device,port,counter,total,per_second,reset\n");
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
Recorder::~Recorder() {
    try {
        finish();
    } catch (...) { // NOLINT(bugprone-empty-catch): destructors cannot report errors.
        // Explicit finish() reports I/O errors; destruction is a best-effort fallback.
    }
}
void Recorder::write(std::string_view text) {
    while (!text.empty()) {
        const auto count = ::write(fd_, text.data(), text.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                    "Write recording");
        text.remove_prefix(static_cast<std::size_t>(count));
    }
}
void Recorder::append(const Snapshot& snapshot) {
    if (fd_ < 0)
        throw std::logic_error("Recording is closed");
    if (snapshot.taken_at < start_)
        return;
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::microseconds>(snapshot.taken_at - start_).count();
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto event = [&](const std::string& json) {
        if (!first_)
            out << ",\n";
        first_ = false;
        out << json;
    };
    for (const auto& port : snapshot.ports) {
        if (format_ == ExportFormat::csv) {
            for (const auto& [name, counter] : port.rates) {
                out << timestamp << ',' << (port.port.kind == PortKind::rdma ? "rdma" : "network")
                    << ',' << csv_quote(port.port.device) << ',' << port.port.number << ','
                    << csv_quote(name) << ',' << counter.total << ','
                    << (counter.per_second ? number(counter.per_second) : "") << ','
                    << (counter.reset ? 1 : 0) << '\n';
            }
            continue;
        }
        const auto [track, inserted] = tracks_.try_emplace(port.port.id(), tracks_.size() + 1);
        const auto pid = std::to_string(track->second);
        if (inserted)
            event("{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":" + pid +
                  ",\"args\":{\"name\":" +
                  json_escape(port.port.device + "/" + std::to_string(port.port.number)) + "}}");
        const std::array<std::pair<std::string_view, std::optional<double>>, 5> metrics{
            {{"tx_gbps", port.tx_gbps()},
             {"rx_gbps", port.rx_gbps()},
             {"tx_pps", port.rate("tx_pkts")},
             {"rx_pps", port.rate("rx_pkts")},
             {"rx_drops_per_sec", port.rate("rx_drops")}}};
        for (const auto& [name, value] : metrics) {
            if (!value || !std::isfinite(*value))
                continue;
            event("{\"name\":" + json_escape(name) + ",\"cat\":\"nitro\",\"ph\":\"C\",\"pid\":" +
                  pid + ",\"tid\":0,\"ts\":" + std::to_string(timestamp) +
                  ",\"args\":{\"value\":" + number(value) + "}}");
        }
    }
    write(out.str());
}
void Recorder::finish() {
    if (fd_ < 0)
        return;
    try {
        if (format_ == ExportFormat::trace)
            write("\n]}\n");
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) < 0)
        throw std::system_error(errno, std::generic_category(), "Close recording");
}
std::string snapshot_json(const Snapshot& snapshot) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"sequence\":" << snapshot.sequence
        << ",\"demo\":" << (snapshot.demo ? "true" : "false") << ",\"ports\":[";
    bool first = true;
    for (const auto& port : snapshot.ports) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"device\":" << json_escape(port.port.device) << ",\"port\":" << port.port.number
            << ",\"kind\":" << json_escape(port.port.kind == PortKind::rdma ? "rdma" : "network")
            << ",\"state\":" << json_escape(port.port.state)
            << ",\"source\":" << json_escape(port.port.source)
            << ",\"link_gbps\":" << number(port.port.link_gbps)
            << ",\"tx_gbps\":" << number(port.tx_gbps())
            << ",\"rx_gbps\":" << number(port.rx_gbps())
            << ",\"tx_pps\":" << number(port.rate("tx_pkts"))
            << ",\"rx_pps\":" << number(port.rate("rx_pkts")) << ",\"counters\":{";
        bool first_counter = true;
        for (const auto& [name, counter] : port.rates) {
            if (!first_counter)
                out << ',';
            first_counter = false;
            out << json_escape(name) << ":{\"total\":" << counter.total
                << ",\"per_second\":" << number(counter.per_second)
                << ",\"reset\":" << (counter.reset ? "true" : "false") << '}';
        }
        out << "}}";
    }
    out << "],\"processes\":[";
    first = true;
    for (const auto& process : snapshot.processes) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"pid\":" << process.pid << ",\"device\":" << json_escape(process.device)
            << ",\"user\":" << json_escape(process.user)
            << ",\"command\":" << json_escape(process.command)
            << ",\"qp_count\":" << process.qp_count << ",\"rss_bytes\":" << process.rss_bytes
            << '}';
    }
    out << "],\"warnings\":[";
    for (std::size_t i = 0; i < snapshot.warnings.size(); ++i) {
        if (i)
            out << ',';
        out << json_escape(snapshot.warnings[i]);
    }
    out << "],\"collection_ms\":" << number(snapshot.collection_ms) << '}';
    return out.str();
}
std::string recording_filename() {
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return "nitro-" + std::to_string(now) + ".json";
}
} // namespace nitro
