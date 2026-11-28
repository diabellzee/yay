#include "netlink_util.h"

#include <cerrno>
#include <cstring>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

namespace netlink_util {

namespace {

[[nodiscard]]
constexpr std::size_t nl_align(std::size_t n) noexcept
{
    return (n + 3U) & ~static_cast<std::size_t>(3U);
}

[[nodiscard]]
bool iface_matters(unsigned index, const std::function<bool(std::string_view)>& relevant)
{
    char name[IF_NAMESIZE] = {};
    if (::if_indextoname(index, name) == nullptr) {
        return true;
    }
    return relevant(name);
}

} // namespace

int open_route_socket() noexcept
{
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) {
        return -1;
    }
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool drain_route_events(int fd, const std::function<bool(std::string_view)>& relevant)
{
    bool matters = false;
    alignas(8) char buf[8192];

    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) { matters = true; continue; }
            break;
        }
        if (n == 0) break;

        const auto len = static_cast<std::size_t>(n);
        std::size_t off = 0;
        while (off + sizeof(nlmsghdr) <= len) {
            nlmsghdr h{};
            std::memcpy(&h, buf + off, sizeof(h));
            if (h.nlmsg_len < sizeof(h) || off + h.nlmsg_len > len) break;

            const char* payload = buf + off + NLMSG_HDRLEN;
            const std::size_t plen = h.nlmsg_len - NLMSG_HDRLEN;

            switch (h.nlmsg_type) {
                case RTM_NEWLINK:
                case RTM_DELLINK:
                    if (plen >= sizeof(ifinfomsg)) {
                        ifinfomsg m{};
                        std::memcpy(&m, payload, sizeof(m));
                        if (m.ifi_index > 0 && iface_matters(static_cast<unsigned>(m.ifi_index), relevant)) {
                            matters = true;
                        }
                    }
                    break;
                case RTM_NEWADDR:
                case RTM_DELADDR:
                    if (plen >= sizeof(ifaddrmsg)) {
                        ifaddrmsg m{};
                        std::memcpy(&m, payload, sizeof(m));
                        const bool link_scope = m.ifa_family == AF_INET6 && m.ifa_scope == RT_SCOPE_LINK;
                        if (!link_scope && iface_matters(m.ifa_index, relevant)) {
                            matters = true;
                        }
                    }
                    break;
                default:
                    break;
            }
            off += nl_align(h.nlmsg_len);
        }
    }
    return matters;
}

} // namespace netlink_util
