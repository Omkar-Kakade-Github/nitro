#include "nitro/options.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
namespace nitro {
Options parse_options(std::span<const std::string_view> args) {
    Options result;
    bool json = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        const auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size())
                throw std::invalid_argument("Missing value for " + std::string{arg});
            return args[++i];
        };
        if (arg == "--help" || arg == "-h")
            result.help = true;
        else if (arg == "--version" || arg == "-V")
            result.version = true;
        else if (arg == "--demo")
            result.demo = true;
        else if (arg == "--ascii")
            result.ascii = true;
        else if (arg == "--no-netlink")
            result.no_netlink = true;
        else if (arg == "--json") {
            result.headless = true;
            json = true;
        } else if (arg == "--headless")
            result.headless = true;
        else if (arg == "--interval" || arg == "-i") {
            const auto number = parse_unsigned(value());
            if (!number || *number < 100 || *number > 60000)
                throw std::invalid_argument("Interval must be 100..60000 milliseconds");
            result.interval_ms = static_cast<int>(*number);
        } else if (arg == "--count") {
            result.count = parse_unsigned(value());
            if (!result.count || *result.count == 0)
                throw std::invalid_argument("Count must be a positive integer");
            result.headless = true;
        } else if (arg == "--duration") {
            const auto raw = value();
            double seconds{};
            const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), seconds);
            if (error != std::errc{} || end != raw.data() + raw.size() || !std::isfinite(seconds) ||
                seconds < 0.1 || seconds > 86400 * 365)
                throw std::invalid_argument("Duration must be 0.1..31536000 seconds");
            result.duration = seconds;
            result.headless = true;
        } else if (arg == "--record") {
            result.record = std::string{value()};
            if (result.record.empty())
                throw std::invalid_argument("Recording requires a file path");
        } else if (arg == "--csv") {
            result.csv = std::string{value()};
            result.headless = true;
            if (result.csv.empty() || result.csv == "-")
                throw std::invalid_argument("CSV requires a file path");
        } else if (arg == "--device" || arg == "-d") {
            auto list = value();
            while (true) {
                const auto comma = list.find(',');
                auto name = trim(list.substr(0, comma));
                if (name.empty())
                    throw std::invalid_argument("Device names must not be empty");
                result.devices.push_back(std::move(name));
                if (comma == std::string_view::npos)
                    break;
                list.remove_prefix(comma + 1);
            }
        } else if (arg == "--theme") {
            const auto name = value();
            if (name == "midnight")
                result.theme = 0;
            else if (name == "forest")
                result.theme = 1;
            else if (name == "mono")
                result.theme = 2;
            else
                throw std::invalid_argument("Theme must be midnight, forest, or mono");
        } else
            throw std::invalid_argument("Unknown option: " + std::string{arg});
    }
    if (json && !result.count && !result.duration)
        result.count = 1;
    return result;
}
void filter_sample(Sample& sample, const std::vector<std::string>& devices) {
    if (devices.empty())
        return;
    const auto match = [&](std::string_view device, std::uint32_t port) {
        return std::any_of(devices.begin(), devices.end(), [&](const auto& filter) {
            return filter == device || filter == std::string{device} + "/" + std::to_string(port);
        });
    };
    std::erase_if(sample.ports, [&](const auto& port) { return !match(port.device, port.number); });
    std::erase_if(sample.processes, [&](const auto& process) {
        return std::none_of(sample.ports.begin(), sample.ports.end(),
                            [&](const auto& port) { return process.device == port.device; });
    });
    if (sample.ports.empty())
        sample.warnings.emplace_back("No interfaces match the requested device filter");
}
std::string_view usage() {
    return R"(nitro — a fast, interactive RDMA monitor

Usage: nitro [options]
  --demo                 Explore a deterministic simulated RDMA fabric
  -i, --interval MS      Sampling interval, 100..60000 (default 1000)
  -d, --device NAMES     Comma-separated devices or device/port pairs
  --theme NAME          midnight (default), forest, mono
  --ascii               Use ASCII borders and graphs
  --no-netlink          Read sysfs counters without RDMA netlink
  --json                Print NDJSON; one measured sample by default
  --headless            Stream NDJSON until interrupted
  --count N             Stop after N measured samples (headless)
  --duration SECONDS    Stop after this duration (headless)
  --record PATH         Stream a Perfetto / Chrome JSON trace
  --csv PATH            Stream all raw counters and rates to CSV (headless)
  -h, --help            Show help
  -V, --version         Show version

Keys: 1/2 RDMA/network, arrows select, Enter details, Tab focus,
      / filter, s sort, S reverse, Space pause, a average, g scale,
      +/- interval, t theme, r record, h help, q quit.
Existing recording files are never overwritten. Linux and ncursesw required.
)";
}
} // namespace nitro
