#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nitro {
using Clock = std::chrono::steady_clock;
using Counters = std::map<std::string, std::uint64_t, std::less<>>;
inline constexpr std::size_t history_limit = 240;

enum class PortKind : std::uint8_t { rdma, network };
struct Port {
    std::string device;
    std::uint32_t number{1};
    PortKind kind{PortKind::rdma};
    std::string state{"UNKNOWN"};
    std::string link_layer;
    std::string source;
    std::optional<double> link_gbps;
    Counters counters;
    Clock::time_point taken_at{};
    [[nodiscard]] std::string id() const;
};
struct Process {
    std::uint32_t pid{};
    std::string device;
    std::string user{"?"};
    std::string command;
    std::string state{"?"};
    std::uint64_t rss_bytes{};
    std::uint64_t virtual_bytes{};
    std::uint32_t threads{};
    int nice{};
    std::size_t qp_count{};
};
struct Host {
    std::string name;
    std::string kernel;
    double uptime_seconds{};
    double load{};
    std::uint64_t memory_total{};
    std::uint64_t memory_available{};
};
struct Sample {
    Clock::time_point taken_at{Clock::now()};
    std::vector<Port> ports;
    std::vector<Process> processes;
    Host host;
    std::vector<std::string> warnings;
    bool demo{};
};
struct CounterRate {
    std::uint64_t total{};
    std::optional<double> per_second;
    bool reset{};
};
struct TrafficPoint {
    Clock::time_point time{};
    std::optional<double> tx;
    std::optional<double> rx;
};
struct PortView {
    Port port;
    std::map<std::string, CounterRate, std::less<>> rates;
    std::deque<TrafficPoint> history;
    [[nodiscard]] std::optional<double> rate(std::string_view name) const;
    [[nodiscard]] std::optional<double> tx_gbps() const;
    [[nodiscard]] std::optional<double> rx_gbps() const;
};
struct Snapshot {
    std::uint64_t sequence{};
    Clock::time_point taken_at{};
    std::vector<PortView> ports;
    std::vector<Process> processes;
    Host host;
    std::vector<std::string> warnings;
    bool demo{};
    bool recording{};
    std::string recording_path;
    std::string recording_error;
    double collection_ms{};
};
[[nodiscard]] std::string trim(std::string_view text);
[[nodiscard]] std::string safe_text(std::string_view text);
[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text);
[[nodiscard]] std::string format_number(double value, int precision = 1);
[[nodiscard]] std::string format_rate(std::optional<double> value, bool bytes = false);
[[nodiscard]] std::string json_escape(std::string_view text);
void normalize_counters(Counters& counters);
} // namespace nitro
