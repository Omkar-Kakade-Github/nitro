#pragma once
#include "nitro/model.hpp"
#include <span>
namespace nitro {
struct Options {
    bool demo{};
    bool headless{};
    bool help{};
    bool version{};
    bool ascii{};
    bool no_netlink{};
    int interval_ms{1000};
    int theme{};
    std::optional<std::uint64_t> count;
    std::optional<double> duration;
    std::string record;
    std::string csv;
    std::vector<std::string> devices;
};
[[nodiscard]] Options parse_options(std::span<const std::string_view> args);
void filter_sample(Sample& sample, const std::vector<std::string>& devices);
[[nodiscard]] std::string_view usage();
} // namespace nitro
