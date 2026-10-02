#include "nitro/collector.hpp"
#include "nitro/metrics.hpp"
#include "nitro/options.hpp"
#include "nitro/trace.hpp"
#include "nitro/tui.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <thread>

namespace nitro {
volatile std::sig_atomic_t interrupted = 0;
namespace {
void handle_signal(int) {
    interrupted = 1;
}
void wait_until(Clock::time_point due) {
    while (!interrupted && Clock::now() < due)
        std::this_thread::sleep_until(std::min(due, Clock::now() + std::chrono::milliseconds{25}));
}
int headless(const Options& options) {
    auto collector = make_collector(options.demo, {}, !options.no_netlink);
    Metrics metrics;
    const auto read = [&] {
        const auto start = Clock::now();
        auto sample = collector->collect();
        filter_sample(sample, options.devices);
        auto snapshot = metrics.update(std::move(sample));
        snapshot.collection_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return snapshot;
    };
    const auto baseline = read();
    if (!options.devices.empty() && baseline.ports.empty())
        throw std::runtime_error("No interfaces match --device");
    const auto start = Clock::now();
    const auto duration = options.duration ? std::chrono::duration_cast<Clock::duration>(
                                                 std::chrono::duration<double>{*options.duration})
                                           : Clock::duration::max();
    const auto end = options.duration ? start + duration : Clock::time_point::max();
    std::unique_ptr<Recorder> trace, csv;
    if (!options.record.empty())
        trace = std::make_unique<Recorder>(options.record, ExportFormat::trace, start);
    if (!options.csv.empty())
        csv = std::make_unique<Recorder>(options.csv, ExportFormat::csv, start);
    std::uint64_t count = 0;
    auto due = start + std::chrono::milliseconds{options.interval_ms};
    while (!interrupted && (!options.count || count < *options.count)) {
        wait_until(std::min(due, end));
        if (interrupted || Clock::now() >= end)
            break;
        auto snapshot = read();
        if (snapshot.taken_at >= end || interrupted)
            break;
        if (trace)
            trace->append(snapshot);
        if (csv)
            csv->append(snapshot);
        std::cout << snapshot_json(snapshot) << '\n' << std::flush;
        if (!std::cout)
            throw std::runtime_error("Cannot write standard output");
        ++count;
        due += std::chrono::milliseconds{options.interval_ms};
        if (due < Clock::now())
            due = Clock::now() + std::chrono::milliseconds{options.interval_ms};
    }
    if (trace)
        trace->finish();
    if (csv)
        csv->finish();
    return 0;
}
} // namespace
} // namespace nitro
int main(int argc, char** argv) {
    try {
        std::vector<std::string_view> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        const auto options = nitro::parse_options(args);
        if (options.help) {
            std::cout << nitro::usage();
            return 0;
        }
        if (options.version) {
            std::cout << "nitro " NITRO_VERSION " (C++20, Linux)\n";
            return 0;
        }
        struct sigaction action {};
        action.sa_handler = nitro::handle_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
        std::signal(SIGPIPE, SIG_IGN);
        return options.headless ? nitro::headless(options) : nitro::run_tui(options);
    } catch (const std::exception& error) {
        std::cerr << "nitro: " << error.what() << '\n';
        return 1;
    }
}
