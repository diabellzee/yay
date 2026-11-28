#pragma once

// NETLINK_ROUTE link + address change listener shared by event-driven daemons.

#include <functional>
#include <string_view>

namespace netlink_util {

// Subscribe to link up/down and IPv4/IPv6 address add/remove. -1 on failure.
// Non-blocking, close-on-exec. No routes/neighbours/rules: far chattier and
// nothing we need.
[[nodiscard]] int open_route_socket() noexcept;

// Drain everything queued. `relevant(ifname)` decides whether an event on an
// interface matters; an interface that has already vanished (rmnet_data* are
// torn down on disconnect) counts as relevant. Link-local IPv6 churn is
// skipped (it accompanies every link event we already see). A kernel buffer
// overflow counts as relevant (re-evaluate from scratch). True if at least one
// event mattered.
[[nodiscard]] bool drain_route_events(
    int fd, const std::function<bool(std::string_view)>& relevant);

} // namespace netlink_util
