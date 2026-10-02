#include "nitro/metrics.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace nitro {
Snapshot Metrics::update(Sample sample) {
    Snapshot result;
    result.sequence = ++sequence_;
    result.taken_at = sample.taken_at;
    result.host = std::move(sample.host);
    result.processes = std::move(sample.processes);
    result.warnings = std::move(sample.warnings);
    result.demo = sample.demo;
    std::set<std::string> present;
    for (auto& port : sample.ports) {
        const auto key = port.id();
        present.insert(key);
        auto& state = states_[key];
        if (state.source != port.source) {
            state.baseline.clear();
            state.source = port.source;
        }
        PortView view;
        for (const auto& [name, value] : port.counters) {
            CounterRate rate{value, std::nullopt, false};
            const auto previous = state.baseline.find(name);
            if (previous != state.baseline.end()) {
                const double elapsed =
                    std::chrono::duration<double>(port.taken_at - previous->second.time).count();
                if (elapsed > 0 && std::isfinite(elapsed)) {
                    if (value >= previous->second.value)
                        rate.per_second =
                            static_cast<double>(value - previous->second.value) / elapsed;
                    else
                        rate.reset = true;
                }
            }
            if (previous == state.baseline.end() || port.taken_at > previous->second.time)
                state.baseline.insert_or_assign(name, Baseline{value, port.taken_at});
            view.rates.emplace(name, rate);
        }
        // Retain baselines across a transient missing read. The next valid delta spans the gap.
        if (state.history.empty() || port.taken_at > state.history.back().time) {
            state.history.push_back({port.taken_at, view.tx_gbps(), view.rx_gbps()});
            if (state.history.size() > history_limit)
                state.history.pop_front();
        }
        view.history = state.history;
        view.port = std::move(port);
        result.ports.push_back(std::move(view));
    }
    std::erase_if(states_, [&](const auto& entry) { return !present.contains(entry.first); });
    return result;
}
std::optional<double> average_rate(const std::deque<TrafficPoint>& history, bool tx,
                                   std::chrono::seconds window) {
    if (history.size() < 2 || window.count() <= 0)
        return std::nullopt;
    const auto cutoff = history.back().time - window;
    double weighted = 0;
    double duration = 0;
    for (std::size_t i = 1; i < history.size(); ++i) {
        const auto& point = history[i];
        const auto value = tx ? point.tx : point.rx;
        const double elapsed =
            std::chrono::duration<double>(point.time - std::max(cutoff, history[i - 1].time))
                .count();
        if (value && elapsed > 0) {
            weighted += *value * elapsed;
            duration += elapsed;
        }
    }
    return duration > 0 ? std::optional{weighted / duration} : std::nullopt;
}
} // namespace nitro
