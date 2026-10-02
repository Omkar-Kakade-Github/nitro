#include "nitro/netlink.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <linux/netlink.h>
#include <poll.h>
#include <rdma/rdma_netlink.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace nitro::netlink {
namespace {
constexpr std::size_t align4(std::size_t size) {
    return (size + 3U) & ~std::size_t{3};
}
[[noreturn]] void system_failure(std::string_view what, int code = errno) {
    throw std::system_error(code, std::generic_category(), std::string{what});
}
template <typename T> T read_header(std::span<const std::byte> data) {
    if (data.size() < sizeof(T))
        throw std::runtime_error("Truncated netlink header");
    T result{};
    std::memcpy(&result, data.data(), sizeof(T));
    return result;
}
void advance_payload(std::span<const std::byte>& bytes, std::size_t length) {
    const auto padded = align4(length);
    if (length > bytes.size())
        throw std::runtime_error("Truncated netlink payload");
    bytes = bytes.subspan(std::min(padded, bytes.size()));
}
} // namespace
std::string Attribute::string() const {
    if (data.empty() || data.back() != std::byte{0})
        throw std::runtime_error("Unterminated netlink string");
    const auto* ptr = reinterpret_cast<const char*>(data.data());
    return {ptr, strnlen(ptr, data.size())};
}
std::vector<Attribute> attributes(std::span<const std::byte> bytes) {
    std::vector<Attribute> result;
    while (!bytes.empty()) {
        const auto header = read_header<nlattr>(bytes);
        if (header.nla_len < sizeof(nlattr) || header.nla_len > bytes.size())
            throw std::runtime_error("Invalid netlink attribute length");
        result.push_back({static_cast<std::uint16_t>(header.nla_type & 0x3fffU),
                          bytes.subspan(sizeof(nlattr), header.nla_len - sizeof(nlattr))});
        advance_payload(bytes, header.nla_len);
    }
    return result;
}
std::vector<Message> messages(std::span<const std::byte> bytes) {
    std::vector<Message> result;
    while (!bytes.empty()) {
        const auto header = read_header<nlmsghdr>(bytes);
        if (header.nlmsg_len < sizeof(nlmsghdr) || header.nlmsg_len > bytes.size())
            throw std::runtime_error("Invalid netlink message length");
        result.push_back({header.nlmsg_type, header.nlmsg_flags, header.nlmsg_seq,
                          bytes.subspan(sizeof(nlmsghdr), header.nlmsg_len - sizeof(nlmsghdr))});
        advance_payload(bytes, header.nlmsg_len);
    }
    return result;
}
void append_attribute(Bytes& bytes, std::uint16_t type, std::span<const std::byte> data) {
    const auto length = sizeof(nlattr) + data.size();
    if (length > std::numeric_limits<std::uint16_t>::max())
        throw std::length_error("Netlink attribute too large");
    const nlattr header{static_cast<std::uint16_t>(length), type};
    const auto offset = bytes.size();
    bytes.resize(offset + align4(length));
    std::memcpy(bytes.data() + offset, &header, sizeof(header));
    if (!data.empty())
        std::memcpy(bytes.data() + offset + sizeof(header), data.data(), data.size());
}
std::optional<Device> parse_device(std::span<const std::byte> payload) {
    Device device;
    bool has_index = false;
    for (const auto& attr : attributes(payload)) {
        switch (attr.type) {
        case RDMA_NLDEV_ATTR_DEV_INDEX:
            device.index = attr.integer<std::uint32_t>();
            has_index = true;
            break;
        case RDMA_NLDEV_ATTR_DEV_NAME:
            device.name = attr.string();
            break;
        case RDMA_NLDEV_ATTR_PORT_INDEX:
            device.ports = attr.integer<std::uint32_t>();
            break;
        default:
            break;
        }
    }
    if (!has_index || device.name.empty())
        return std::nullopt;
    if (device.name.find('/') != std::string::npos || device.name == "." || device.name == "..")
        throw std::runtime_error("Invalid RDMA device name");
    if (device.ports > 255)
        throw std::runtime_error("Invalid RDMA port count");
    return device;
}
Counters parse_counters(std::span<const std::byte> payload) {
    Counters result;
    for (const auto& table : attributes(payload)) {
        if (table.type != RDMA_NLDEV_ATTR_STAT_HWCOUNTERS)
            continue;
        for (const auto& entry : attributes(table.data)) {
            if (entry.type != RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY)
                continue;
            std::string name;
            std::optional<std::uint64_t> value;
            for (const auto& field : attributes(entry.data)) {
                if (field.type == RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_NAME)
                    name = field.string();
                if (field.type == RDMA_NLDEV_ATTR_STAT_HWCOUNTER_ENTRY_VALUE)
                    value = field.integer<std::uint64_t>();
            }
            if (!name.empty() && value)
                result.insert_or_assign(name, *value);
        }
    }
    return result;
}
std::vector<QueuePair> parse_queue_pairs(std::span<const std::byte> payload) {
    std::vector<QueuePair> result;
    for (const auto& table : attributes(payload)) {
        if (table.type != RDMA_NLDEV_ATTR_RES_QP)
            continue;
        for (const auto& entry : attributes(table.data)) {
            if (entry.type != RDMA_NLDEV_ATTR_RES_QP_ENTRY)
                continue;
            QueuePair qp;
            for (const auto& field : attributes(entry.data)) {
                switch (field.type) {
                case RDMA_NLDEV_ATTR_RES_PID:
                    qp.pid = field.integer<std::uint32_t>();
                    break;
                case RDMA_NLDEV_ATTR_RES_LQPN:
                    qp.number = field.integer<std::uint32_t>();
                    break;
                case RDMA_NLDEV_ATTR_PORT_INDEX:
                    qp.port = field.integer<std::uint32_t>();
                    break;
                case RDMA_NLDEV_ATTR_RES_KERN_NAME:
                    qp.kernel_name = field.string();
                    break;
                default:
                    break;
                }
            }
            result.push_back(std::move(qp));
        }
    }
    return result;
}
Socket::Socket() : fd_(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_RDMA)) {
    if (fd_ < 0)
        system_failure("RDMA netlink socket");
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
        const int error = errno;
        ::close(fd_);
        fd_ = -1;
        system_failure("Bind RDMA netlink socket", error);
    }
}
Socket::~Socket() {
    if (fd_ >= 0)
        ::close(fd_);
}
std::vector<Bytes>
Socket::request(std::uint16_t operation, bool dump,
                std::span<const std::pair<std::uint16_t, std::uint32_t>> values) {
    Bytes request(sizeof(nlmsghdr));
    for (const auto& [type, value] : values)
        append_attribute(request, type, std::as_bytes(std::span{&value, 1}));
    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(request.size());
    header.nlmsg_type = static_cast<std::uint16_t>(RDMA_NL_GET_TYPE(RDMA_NL_NLDEV, operation));
    header.nlmsg_flags =
        static_cast<std::uint16_t>(NLM_F_REQUEST | (dump ? NLM_F_DUMP : NLM_F_ACK));
    header.nlmsg_seq = ++sequence_;
    std::memcpy(request.data(), &header, sizeof(header));
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (::sendto(fd_, request.data(), request.size(), 0, reinterpret_cast<const sockaddr*>(&kernel),
                 sizeof(kernel)) < 0)
        system_failure("Send RDMA netlink request");
    std::vector<Bytes> result;
    const auto deadline = Clock::now() + std::chrono::milliseconds{300};
    std::size_t total_bytes{};
    bool got_data = false;
    for (;;) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0)
            system_failure("RDMA netlink response timeout", ETIMEDOUT);
        pollfd poll_fd{fd_, POLLIN, 0};
        const auto ready = ::poll(&poll_fd, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            system_failure("Poll RDMA netlink");
        if (ready == 0)
            system_failure("RDMA netlink response timeout", ETIMEDOUT);
        Bytes buffer(std::size_t{256} * 1024);
        sockaddr_nl sender{};
        iovec io{buffer.data(), buffer.size()};
        msghdr msg{};
        msg.msg_name = &sender;
        msg.msg_namelen = sizeof(sender);
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        const auto received = ::recvmsg(fd_, &msg, MSG_DONTWAIT);
        if (received < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (received < 0)
            system_failure("Receive RDMA netlink");
        if ((msg.msg_flags & MSG_TRUNC) != 0)
            throw std::runtime_error("Truncated RDMA netlink datagram");
        if (sender.nl_pid != 0)
            continue;
        buffer.resize(static_cast<std::size_t>(received));
        total_bytes += buffer.size();
        if (total_bytes > std::size_t{64} * 1024 * 1024)
            throw std::runtime_error("RDMA response exceeds 64 MiB");
        for (const auto& response : messages(buffer)) {
            if (response.sequence != sequence_)
                continue;
            if ((response.flags & NLM_F_DUMP_INTR) != 0)
                throw std::runtime_error("RDMA dump interrupted; retrying next sample");
            if (response.type == NLMSG_ERROR || response.type == NLMSG_DONE) {
                if (!response.payload.empty()) {
                    const auto error = read_header<std::int32_t>(response.payload);
                    if (error != 0) {
                        if (error > 0 || error == std::numeric_limits<std::int32_t>::min())
                            throw std::runtime_error("Invalid netlink error code");
                        system_failure("RDMA query", -error);
                    }
                } else if (response.type == NLMSG_ERROR)
                    throw std::runtime_error("Empty netlink error");
                if (response.type == NLMSG_DONE || (!dump && got_data))
                    return result;
                continue;
            }
            if (response.type == NLMSG_OVERRUN)
                throw std::runtime_error("RDMA netlink overrun");
            if (response.type != header.nlmsg_type)
                continue;
            result.emplace_back(response.payload.begin(), response.payload.end());
            got_data = true;
            if (!dump && (response.flags & NLM_F_MULTI) == 0)
                return result;
        }
    }
}
} // namespace nitro::netlink
