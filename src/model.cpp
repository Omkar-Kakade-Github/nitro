#include "nitro/model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace nitro {
std::string Port::id() const {
    return (kind == PortKind::rdma ? "rdma:" : "net:") + device + "/" + std::to_string(number);
}
std::optional<double> PortView::rate(std::string_view name) const {
    const auto it = rates.find(name);
    return it == rates.end() ? std::nullopt : it->second.per_second;
}
std::optional<double> PortView::tx_gbps() const {
    const auto value = rate("tx_bytes");
    return value ? std::optional{*value * 8.0 / 1e9} : std::nullopt;
}
std::optional<double> PortView::rx_gbps() const {
    const auto value = rate("rx_bytes");
    return value ? std::optional{*value * 8.0 / 1e9} : std::nullopt;
}
std::string trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
        return {};
    return std::string{text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1)};
}
std::string safe_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        // Kernel and process strings must never inject terminal control sequences.
        result += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return result;
}
std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    const auto clean = trim(text);
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(clean.data(), clean.data() + clean.size(), value);
    if (error != std::errc{} || end != clean.data() + clean.size())
        return std::nullopt;
    return value;
}
std::string format_number(double value, int precision) {
    if (!std::isfinite(value))
        return "--";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
std::string format_rate(std::optional<double> value, bool bytes) {
    if (!value)
        return "--";
    constexpr std::array suffixes{"", "k", "M", "G", "T", "P", "E"};
    std::size_t unit = 0;
    double scaled = *value;
    while (std::abs(scaled) >= 1000 && unit + 1 < suffixes.size()) {
        scaled /= 1000;
        ++unit;
    }
    return format_number(scaled, unit == 0 ? 0 : 1) + suffixes[unit] + (bytes ? "B/s" : "/s");
}
std::string json_escape(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else
            result += static_cast<char>(c);
    }
    return result + '"';
}
void normalize_counters(Counters& counters) {
    // Keep the raw IB word counters unchanged; scale only their canonical byte aliases.
    const auto alias = [&](std::string_view dst, std::string_view src, std::uint64_t scale = 1) {
        const auto found = counters.find(src);
        if (counters.contains(dst) || found == counters.end())
            return;
        const auto max = std::numeric_limits<std::uint64_t>::max();
        // An unrepresentable total is unavailable, not silently wrapped or saturated.
        if (found->second <= max / scale)
            counters.emplace(dst, found->second * scale);
    };
    for (const auto prefix : {"hw_", ""}) {
        alias("tx_bytes", std::string{prefix} + "tx_bytes_cnt");
        alias("rx_bytes", std::string{prefix} + "rx_bytes_cnt");
        alias("tx_pkts", std::string{prefix} + "tx_packets_cnt");
        alias("rx_pkts", std::string{prefix} + "rx_packets_cnt");
        alias("rx_drops", std::string{prefix} + "rx_disable_drop_cnt");
    }
    alias("tx_bytes", "port_xmit_data", 4);
    alias("rx_bytes", "port_rcv_data", 4);
    alias("tx_pkts", "port_xmit_packets");
    alias("rx_pkts", "port_rcv_packets");
}
} // namespace nitro
