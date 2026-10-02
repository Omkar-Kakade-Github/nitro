#pragma once
#include "nitro/model.hpp"
namespace nitro {
class Metrics {
  public:
    [[nodiscard]] Snapshot update(Sample sample);

  private:
    struct Baseline {
        std::uint64_t value{};
        Clock::time_point time{};
    };
    struct State {
        std::map<std::string, Baseline, std::less<>> baseline;
        std::deque<TrafficPoint> history;
        std::string source;
    };
    std::map<std::string, State> states_;
    std::uint64_t sequence_{};
};
[[nodiscard]] std::optional<double> average_rate(const std::deque<TrafficPoint>& history, bool tx,
                                                 std::chrono::seconds window);
} // namespace nitro
