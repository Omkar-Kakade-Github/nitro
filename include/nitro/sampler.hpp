#pragma once
#include "nitro/collector.hpp"
#include "nitro/options.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace nitro {
class Sampler {
  public:
    explicit Sampler(const Options& options);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    [[nodiscard]] std::shared_ptr<const Snapshot> latest() const;
    void interval(int milliseconds);
    void record(std::string path); // Empty path stops recording.
  private:
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::shared_ptr<const Snapshot> latest_;
    int interval_ms_;
    std::optional<std::string> record_command_;
    std::uint64_t revision_{};
    std::jthread worker_;
    void run(const std::stop_token& stop);
};
} // namespace nitro
