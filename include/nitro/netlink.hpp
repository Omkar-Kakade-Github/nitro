#pragma once

#include "nitro/model.hpp"
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

namespace nitro::netlink {
using Bytes = std::vector<std::byte>;
struct Attribute {
    std::uint16_t type{};
    std::span<const std::byte> data;
    template <typename T> [[nodiscard]] T integer() const {
        if (data.size() != sizeof(T))
            throw std::runtime_error("Invalid netlink integer size");
        T value{};
        std::memcpy(&value, data.data(), sizeof(T));
        return value;
    }
    [[nodiscard]] std::string string() const;
};
[[nodiscard]] std::vector<Attribute> attributes(std::span<const std::byte> bytes);
struct Message {
    std::uint16_t type{};
    std::uint16_t flags{};
    std::uint32_t sequence{};
    std::span<const std::byte> payload;
};
[[nodiscard]] std::vector<Message> messages(std::span<const std::byte> bytes);
void append_attribute(Bytes& bytes, std::uint16_t type, std::span<const std::byte> data);
[[nodiscard]] Counters parse_counters(std::span<const std::byte> payload);
struct Device {
    std::uint32_t index{};
    std::string name;
    std::uint32_t ports{1};
};
[[nodiscard]] std::optional<Device> parse_device(std::span<const std::byte> payload);
struct QueuePair {
    std::uint32_t pid{};
    std::uint32_t number{};
    std::uint32_t port{};
    std::string kernel_name;
};
[[nodiscard]] std::vector<QueuePair> parse_queue_pairs(std::span<const std::byte> payload);

class Socket {
  public:
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] std::vector<Bytes>
    request(std::uint16_t operation, bool dump,
            std::span<const std::pair<std::uint16_t, std::uint32_t>> values = {});

  private:
    int fd_{-1};
    std::uint32_t sequence_{};
};
} // namespace nitro::netlink
