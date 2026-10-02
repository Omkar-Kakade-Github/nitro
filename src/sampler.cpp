#include "nitro/sampler.hpp"
#include "nitro/metrics.hpp"
#include "nitro/trace.hpp"
#include <algorithm>
namespace nitro {
Sampler::Sampler(const Options& options) : options_(options), interval_ms_(options.interval_ms) {
    if (!options.record.empty())
        record_command_ = options.record;
    worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}
Sampler::~Sampler() {
    worker_.request_stop();
    wake_.notify_all();
}
std::shared_ptr<const Snapshot> Sampler::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}
void Sampler::interval(int milliseconds) {
    {
        std::lock_guard lock(mutex_);
        interval_ms_ = std::clamp(milliseconds, 100, 60000);
        ++revision_;
    }
    wake_.notify_all();
}
void Sampler::record(std::string path) {
    {
        std::lock_guard lock(mutex_);
        record_command_ = std::move(path);
        ++revision_;
    }
    wake_.notify_all();
}
void Sampler::run(const std::stop_token& stop) {
    std::unique_ptr<Recorder> recorder;
    std::string path;
    std::string recording_error;
    try {
        auto collector = make_collector(options_.demo, {}, !options_.no_netlink);
        Metrics metrics;
        if (auto* demo = dynamic_cast<DemoCollector*>(collector.get())) {
            const auto now = Clock::now();
            for (int i = static_cast<int>(history_limit); i > 0; --i) {
                auto sample = demo->at(now - std::chrono::seconds{i});
                filter_sample(sample, options_.devices);
                (void)metrics.update(std::move(sample));
            }
        }
        while (!stop.stop_requested()) {
            std::optional<std::string> command;
            std::uint64_t revision{};
            int interval{};
            {
                std::lock_guard lock(mutex_);
                command = std::move(record_command_);
                record_command_.reset();
                revision = revision_;
                interval = interval_ms_;
            }
            if (command) {
                try {
                    if (recorder)
                        recorder->finish();
                    recorder.reset();
                    path.clear();
                    recording_error.clear();
                    if (!command->empty()) {
                        recorder =
                            std::make_unique<Recorder>(*command, ExportFormat::trace, Clock::now());
                        path = *command;
                    }
                } catch (const std::exception& e) {
                    recorder.reset();
                    path.clear();
                    recording_error = e.what();
                }
            }
            const auto began = Clock::now();
            Snapshot snapshot;
            try {
                auto sample = collector->collect(stop);
                if (stop.stop_requested())
                    break;
                filter_sample(sample, options_.devices);
                snapshot = metrics.update(std::move(sample));
            } catch (const std::exception& e) {
                snapshot.taken_at = Clock::now();
                snapshot.warnings.emplace_back(std::string{"Sampling failed: "} + e.what());
            }
            snapshot.collection_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - began).count();
            if (recorder) {
                try {
                    recorder->append(snapshot);
                } catch (const std::exception& e) {
                    recording_error = e.what();
                    recorder.reset();
                    path.clear();
                }
            }
            snapshot.recording = recorder != nullptr;
            snapshot.recording_path = path;
            snapshot.recording_error = recording_error;
            auto published = std::make_shared<const Snapshot>(std::move(snapshot));
            std::unique_lock lock(mutex_);
            latest_ = std::move(published);
            auto due = began + std::chrono::milliseconds{interval};
            if (due <= Clock::now())
                due = Clock::now() + std::chrono::milliseconds{interval};
            wake_.wait_until(lock, stop, due, [&] { return revision_ != revision; });
        }
        if (recorder)
            recorder->finish();
    } catch (const std::exception& e) {
        auto failure = std::make_shared<Snapshot>();
        failure->taken_at = Clock::now();
        failure->warnings.emplace_back(std::string{"Sampler stopped: "} + e.what());
        std::lock_guard lock(mutex_);
        latest_ = std::move(failure);
    }
}
} // namespace nitro
