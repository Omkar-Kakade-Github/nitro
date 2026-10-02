#include "nitro/collector.hpp"
#include "nitro/metrics.hpp"
#include <iostream>

int main() {
    nitro::DemoCollector collector;
    nitro::Metrics metrics;
    const auto epoch = nitro::Clock::now();
    constexpr int iterations = 10000;
    std::size_t checksum = 0;
    const auto began = nitro::Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const auto sample = collector.at(epoch + std::chrono::milliseconds{i * 100});
        const auto snapshot = metrics.update(sample);
        checksum += snapshot.ports.size();
    }
    const auto elapsed =
        std::chrono::duration<double, std::milli>(nitro::Clock::now() - began).count();
    std::cout << "demo + metrics: " << elapsed / iterations << " ms/sample; " << checksum
              << " port samples; history capped at " << nitro::history_limit << '\n';
}
