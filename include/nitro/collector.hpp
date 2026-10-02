#pragma once
#include "nitro/model.hpp"
#include "nitro/netlink.hpp"
#include <filesystem>
#include <memory>
#include <stop_token>

namespace nitro {
struct Paths {
    std::filesystem::path sys{"/sys"};
    std::filesystem::path proc{"/proc"};
};
class Collector {
  public:
    virtual ~Collector() = default;
    [[nodiscard]] virtual Sample collect(std::stop_token stop = {}) = 0;
};
[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path);
[[nodiscard]] std::optional<double> parse_link_speed(std::string_view text);
[[nodiscard]] Process read_process(const Paths& paths, std::uint32_t pid);
class LinuxCollector final : public Collector {
  public:
    explicit LinuxCollector(Paths paths = {}, bool use_netlink = true);
    [[nodiscard]] Sample collect(std::stop_token stop = {}) override;

  private:
    Paths paths_;
    bool use_netlink_;
    std::vector<Process> processes_;
    std::vector<std::string> process_warnings_;
    Clock::time_point processes_at_{};
    [[nodiscard]] Host read_host() const;
    [[nodiscard]] std::vector<Port> read_network(std::vector<std::string>& warnings) const;
};
class DemoCollector final : public Collector {
  public:
    DemoCollector();
    [[nodiscard]] Sample collect(std::stop_token stop = {}) override;
    [[nodiscard]] Sample at(Clock::time_point time) const;

  private:
    Clock::time_point epoch_;
};
[[nodiscard]] std::unique_ptr<Collector> make_collector(bool demo, Paths paths = {},
                                                        bool netlink = true);
} // namespace nitro
