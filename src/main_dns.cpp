// yay_dns — Auto Private DNS.
//
// Switches Android's Private DNS (DNS-over-TLS) endpoint by the type of the
// active connection:
//
//   Wi-Fi        -> wifi_host   (personal NextDNS endpoint)
//   Mobile data  -> mobile_host (default: Cloudflare — passes carrier SNI/DPI
//                                filters that selectively block NextDNS)
//
// Design constraints (see README "Auto Private DNS"):
//
//  * Zero polling. The daemon blocks in poll() with an infinite timeout on a
//    NETLINK_ROUTE socket subscribed to link + address changes. When the
//    network is stable it is asleep — no timers, no wakeups. The only timed
//    poll() is the short debounce window after a burst of events, and the
//    one-shot "wait for Settings Provider" loop right after boot.
//  * No VPN/TUN, no packet interception. The only thing it ever does is
//    write Settings.Global private_dns_mode / private_dns_specifier via
//    `cmd settings`; the kernel/netd keep handling all traffic natively.
//  * Manual changes win. See decide() in dns_policy.cpp for the exact rule.
//  * Single instance via a POSIX record lock (released by the kernel on any
//    kind of process death, so there is never a stale lockfile to clean up).

#include "dns_policy.h"
#include "exec_util.h"
#include "json_config.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>

#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* kConfigPath = "/data/adb/yay/config/dns_config.json";
constexpr const char* kLogFile    = "/data/adb/yay/run.log";
constexpr const char* kLockPath   = "/data/adb/yay/cache/yay_dns.lock";

constexpr const char* kKeyMode = "private_dns_mode";
constexpr const char* kKeyHost = "private_dns_specifier";

// `cmd settings` is not up the instant boot_completed fires on every ROM.
constexpr int kSettingsReadyTries   = 60;
constexpr int kSettingsReadyStepMs  = 2000;

// A write that did not stick is retried a few times with a short backoff
// before giving up until the next network event.
constexpr int kApplyAttempts = 3;

// Continuous flapping must still converge: never delay evaluation past
// this many debounce windows after the first event of a burst.
constexpr int kMaxDebounceStretch = 5;

// ─── small RAII fd ───────────────────────────────────────────────────────────

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    ~Fd() noexcept { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    void reset() noexcept
    {
        if (fd_ >= 0) {
            while (::close(fd_) < 0 && errno == EINTR) {
            }
            fd_ = -1;
        }
    }

private:
    int fd_;
};

// ─── signals: self-pipe so poll() can never miss a wakeup ───────────────────

volatile sig_atomic_t g_stop   = 0;
volatile sig_atomic_t g_reload = 0;
int g_sig_wr = -1;

void on_signal(int sig)
{
    const int saved = errno;
    if (sig == SIGHUP) {
        g_reload = 1;
    } else {
        g_stop = 1;
    }
    if (g_sig_wr >= 0) {
        const char c = 0;
        static_cast<void>(::write(g_sig_wr, &c, 1));
    }
    errno = saved;
}

void drain_fd(int fd) noexcept
{
    char buf[64];
    while (::read(fd, buf, sizeof(buf)) > 0) {
    }
}

// Sleep up to `ms`, but return early (true) if asked to stop.
[[nodiscard]]
bool wait_ms(int sig_rd, int ms) noexcept
{
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::milliseconds(ms);

    while (!g_stop) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - clock::now()).count();
        if (left <= 0) {
            break;
        }
        pollfd pfd{sig_rd, POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(left));
        if (rc > 0) {
            drain_fd(sig_rd);
        } else if (rc < 0 && errno != EINTR) {
            break;
        }
        if (g_reload) {
            break; // let the main loop pick the reload up promptly
        }
    }
    return g_stop != 0;
}

// ─── lock / single instance ──────────────────────────────────────────────────

[[nodiscard]]
Fd acquire_lock()
{
    Fd fd(::open(kLockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!fd.valid()) {
        return Fd();
    }

    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd.get(), F_SETLK, &fl) != 0) {
        return Fd(); // held by another instance
    }

    // Informational only (humans doing `cat`) — the lock is the truth.
    if (::ftruncate(fd.get(), 0) == 0) {
        char line[24];
        const int n = std::snprintf(line, sizeof(line), "%d\n", static_cast<int>(::getpid()));
        if (n > 0) {
            static_cast<void>(::write(fd.get(), line, static_cast<size_t>(n)));
        }
    }
    return fd;
}

// pid of the running daemon, or 0.
[[nodiscard]]
pid_t lock_holder() noexcept
{
    Fd fd(::open(kLockPath, O_RDWR | O_CLOEXEC));
    if (!fd.valid()) {
        return 0;
    }
    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd.get(), F_GETLK, &fl) != 0 || fl.l_type == F_UNLCK) {
        return 0;
    }
    return fl.l_pid;
}

// ─── config ──────────────────────────────────────────────────────────────────

enum class CfgState { Ok, Disabled, Invalid };

[[nodiscard]]
CfgState load_config(AutoDnsConfig& out, std::string* err)
{
    const auto j = load_json(kConfigPath);
    if (!j) {
        if (err) *err = std::string("cannot load ") + kConfigPath;
        return CfgState::Invalid;
    }

    std::string why;
    if (!validate_config_shape(*j, ConfigKind::DnsConfig, &why)) {
        if (err) *err = "shape validation failed: " + why;
        return CfgState::Invalid;
    }

    auto parsed = parse_dns_config(*j, &why);
    if (!parsed) {
        if (err) *err = why;
        return CfgState::Invalid;
    }

    out = std::move(*parsed);
    return out.enabled ? CfgState::Ok : CfgState::Disabled;
}

// ─── Settings.Global access (via `cmd settings`, no shell involved) ─────────

[[nodiscard]]
std::string trim_ws(std::string s)
{
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [&](char c) { return not_space(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [&](char c) { return not_space(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

// nullopt = could not read (service not up yet, error). "" = key unset.
[[nodiscard]]
std::optional<std::string> settings_get(const char* key)
{
    const auto r = exec_cmd({"cmd", "settings", "get", "global", key}, 5);
    if (!r.ok()) {
        return std::nullopt;
    }
    std::string s = trim_ws(r.stdout_str);
    if (s == "null") {
        s.clear();
    }
    // exec_cmd merges stderr into stdout. A genuine value is one token; if
    // there is whitespace inside, something else got printed — don't trust it.
    if (std::any_of(s.begin(), s.end(),
            [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; })) {
        return std::nullopt;
    }
    return s;
}

[[nodiscard]]
bool settings_put(const char* key, const std::string& value)
{
    return exec_cmd({"cmd", "settings", "put", "global", key, value}, 5).ok();
}

[[nodiscard]]
std::optional<DnsSetting> read_current()
{
    auto mode = settings_get(kKeyMode);
    auto host = settings_get(kKeyHost);
    if (!mode || !host) {
        return std::nullopt;
    }
    return DnsSetting{std::move(*mode), std::move(*host)};
}

// Specifier first, mode second: when the mode flips to "hostname" the
// specifier is already the right one, so the system never briefly tries to
// validate a stale host. If mode is already "hostname" only the specifier
// write happens — that alone makes ConnectivityService re-resolve.
[[nodiscard]]
bool write_target(const DnsSetting& cur, const DnsSetting& target)
{
    if (cur.host != target.host && !settings_put(kKeyHost, target.host)) {
        return false;
    }
    if (cur.mode != target.mode && !settings_put(kKeyMode, target.mode)) {
        return false;
    }
    return true;
}

// Write + read back. Only a verified write counts as applied.
[[nodiscard]]
bool apply_verified(int sig_rd, DnsSetting cur, const DnsSetting& target)
{
    for (int attempt = 1; attempt <= kApplyAttempts && !g_stop; ++attempt) {
        if (write_target(cur, target)) {
            const auto now = read_current();
            if (now && same_setting(*now, target)) {
                return true;
            }
            if (now) {
                cur = *now;
            }
        }
        Logger::warn("yay_dns: write attempt " + std::to_string(attempt) + "/" +
                     std::to_string(kApplyAttempts) + " did not stick");
        if (attempt < kApplyAttempts && wait_ms(sig_rd, 500 * attempt)) {
            break;
        }
    }
    return false;
}

// ─── network detection ───────────────────────────────────────────────────────

// Which kind of network currently has a usable address? When both are up,
// cfg.prefer_cellular decides (default: the SIM wins, so the carrier-safe
// profile is in effect whenever mobile data is actually available).
[[nodiscard]]
NetKind detect_network(const AutoDnsConfig& cfg)
{
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return NetKind::None;
    }

    bool wifi = false;
    bool cell = false;

    for (const ifaddrs* p = list; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr || p->ifa_name == nullptr) {
            continue;
        }
        if ((p->ifa_flags & IFF_UP) == 0U) {
            continue;
        }

        const IfaceClass cls = classify_iface(p->ifa_name, cfg);
        if (cls == IfaceClass::Other) {
            continue;
        }
        // Carrier state is reliable on the station iface; mobile ifaces
        // (rmnet/ccmni/seth) are not consistent about IFF_RUNNING across
        // vendors, so for those the usable address is the signal.
        if (cls == IfaceClass::Wifi && (p->ifa_flags & IFF_RUNNING) == 0U) {
            continue;
        }

        bool usable = false;
        if (p->ifa_addr->sa_family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
            usable = usable_ipv4(sin->sin_addr.s_addr);
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(p->ifa_addr);
            usable = usable_ipv6(sin6->sin6_addr);
        }

        if (usable) {
            (cls == IfaceClass::Wifi ? wifi : cell) = true;
        }
    }

    ::freeifaddrs(list);

    // Only when BOTH are up does the mobile-data toggle matter. A phone with
    // mobile data switched off still keeps an IMS/VoLTE cellular interface
    // with an address; without this check "cellular wins" would then pin the
    // DNS to the mobile profile forever, even while on Wi-Fi.
    bool data_off = false;
    if (wifi && cell && cfg.prefer_cellular) {
        const auto md = settings_get("mobile_data");
        data_off = md.has_value() && *md == "0";
    }

    return pick_network(wifi, cell, cfg.prefer_cellular, data_off);
}

[[nodiscard]]
const char* net_name(NetKind k) noexcept
{
    switch (k) {
        case NetKind::Wifi:     return "wifi";
        case NetKind::Cellular: return "cellular";
        case NetKind::None:     return "none";
    }
    return "none";
}

[[nodiscard]]
std::string fmt(const DnsSetting& s)
{
    return (s.mode.empty() ? std::string("unset") : s.mode) +
           (s.host.empty() ? std::string() : (" " + s.host));
}

// ─── netlink ─────────────────────────────────────────────────────────────────

[[nodiscard]]
Fd open_netlink()
{
    Fd fd(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE));
    if (!fd.valid()) {
        return Fd();
    }

    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    // Link up/down + address add/remove only. No routes, no neighbours, no
    // rules: those are far chattier and carry nothing we need.
    sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        return Fd();
    }
    return fd;
}

[[nodiscard]]
constexpr size_t nl_align(size_t n) noexcept
{
    return (n + 3U) & ~static_cast<size_t>(3U);
}

// Is an event on interface `index` worth waking up for? Events for tun*,
// dummy*, lo, v4-wlan0, p2p-*, ... are noise. If the index can no longer be
// resolved the interface just vanished (rmnet_data* are torn down on
// disconnect) — treat that as relevant rather than guess.
[[nodiscard]]
bool iface_relevant(unsigned index, const AutoDnsConfig& cfg) noexcept
{
    char name[IF_NAMESIZE] = {};
    if (::if_indextoname(index, name) == nullptr) {
        return true;
    }
    return classify_iface(name, cfg) != IfaceClass::Other;
}

// Drain everything queued; true if at least one event mattered.
[[nodiscard]]
bool drain_netlink(int nl, const AutoDnsConfig& cfg)
{
    bool relevant = false;
    alignas(8) char buf[8192];

    for (;;) {
        const ssize_t n = ::recv(nl, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOBUFS) {
                relevant = true; // kernel dropped events — re-evaluate from scratch
                continue;
            }
            break; // EAGAIN or hard error
        }
        if (n == 0) {
            break;
        }

        const auto len = static_cast<size_t>(n);
        size_t off = 0;
        while (off + sizeof(nlmsghdr) <= len) {
            nlmsghdr h{};
            std::memcpy(&h, buf + off, sizeof(h));
            if (h.nlmsg_len < sizeof(h) || off + h.nlmsg_len > len) {
                break;
            }

            const char* payload = buf + off + NLMSG_HDRLEN;
            const size_t plen = h.nlmsg_len - NLMSG_HDRLEN;

            switch (h.nlmsg_type) {
                case RTM_NEWLINK:
                case RTM_DELLINK:
                    if (plen >= sizeof(ifinfomsg)) {
                        ifinfomsg m{};
                        std::memcpy(&m, payload, sizeof(m));
                        if (m.ifi_index > 0 &&
                            iface_relevant(static_cast<unsigned>(m.ifi_index), cfg)) {
                            relevant = true;
                        }
                    }
                    break;
                case RTM_NEWADDR:
                case RTM_DELADDR:
                    if (plen >= sizeof(ifaddrmsg)) {
                        ifaddrmsg m{};
                        std::memcpy(&m, payload, sizeof(m));
                        // Link-local v6 churn accompanies every link event we
                        // already see; skipping it just trims wakeups.
                        const bool link_scope =
                            m.ifa_family == AF_INET6 && m.ifa_scope == RT_SCOPE_LINK;
                        if (!link_scope && iface_relevant(m.ifa_index, cfg)) {
                            relevant = true;
                        }
                    }
                    break;
                default:
                    break;
            }

            off += nl_align(h.nlmsg_len);
        }
    }
    return relevant;
}

// ─── one evaluation ──────────────────────────────────────────────────────────

// What the last evaluation concluded, so an event that did not change the
// kind of network (v6 address/lifetime churn, a second address on the same
// iface, ...) costs nothing — no `cmd settings` processes at all. Re-reading
// Settings in that case could only ever (a) find nothing to do or (b) notice
// a manual change, which decide() would respect anyway.
struct EvalMemo {
    NetKind net = NetKind::None;
    bool settled = false; // last pass ended InSync / Applied / HoldManual
};

// Look at the network, compare with Settings, act per decide().
// Returns a one-line summary (also logged when something changed).
std::string evaluate(int sig_rd, Tracker& tr, EvalMemo& memo, const AutoDnsConfig& cfg)
{
    const NetKind net = detect_network(cfg);
    if (net == NetKind::None) {
        memo.settled = false; // coming back from no-network must re-check
        return "no usable network — Private DNS left untouched";
    }
    if (memo.settled && net == memo.net) {
        return std::string("net=") + net_name(net) + " unchanged — nothing to do";
    }
    memo.net = net;
    memo.settled = false;

    const auto cur = read_current();
    if (!cur) {
        Logger::warn("yay_dns: Settings not readable, will retry on next network event");
        return "settings not readable";
    }

    const Decision d = decide(tr, net, *cur, cfg);
    const std::string prefix = std::string("net=") + net_name(net) + " ";

    switch (d.verdict) {
        case Verdict::NoNetwork:
            return "no usable network";

        case Verdict::InSync:
            memo.settled = true;
            if (d.resumed) {
                const std::string msg = prefix + "settings back on module profile, "
                                        "automation resumed (" + fmt(d.target) + ")";
                Logger::info("yay_dns: " + msg);
                return msg;
            }
            return prefix + "already " + fmt(d.target);

        case Verdict::HoldManual: {
            memo.settled = true;
            const std::string msg = prefix + "manual Private DNS (" + fmt(*cur) +
                                    ") — automation paused";
            if (d.entered_hold) {
                Logger::info("yay_dns: " + msg + " until it returns to a module profile or reboot");
            }
            return msg;
        }

        case Verdict::Apply: {
            if (apply_verified(sig_rd, *cur, d.target)) {
                note_applied(tr, d.target);
                memo.settled = true;
                const std::string msg = prefix + fmt(*cur) + " -> " + fmt(d.target) +
                                        (d.resumed ? " (resumed)" : "");
                Logger::info("yay_dns: " + msg);
                return msg;
            }
            const std::string msg = prefix + "failed to apply " + fmt(d.target);
            Logger::err("yay_dns: " + msg);
            return msg;
        }
    }
    return "unknown";
}

// ─── daemon ──────────────────────────────────────────────────────────────────

[[nodiscard]]
int run_daemon()
{
    Logger::init("yay_dns", kLogFile);

    Fd lock = acquire_lock();
    if (!lock.valid()) {
        Logger::info("yay_dns: another instance is already running, exiting");
        return 0;
    }

    AutoDnsConfig cfg;
    {
        std::string err;
        switch (load_config(cfg, &err)) {
            case CfgState::Ok:
                break;
            case CfgState::Disabled:
                Logger::info("yay_dns: disabled in dns_config.json, exiting");
                return 0;
            case CfgState::Invalid:
                Logger::err("yay_dns: " + err + " — exiting");
                return 1;
        }
    }

    // Signals: self-pipe, handlers installed before anything blocks.
    int pipefd[2] = {-1, -1};
    if (::pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) {
        Logger::err("yay_dns: pipe2 failed");
        return 1;
    }
    Fd sig_rd(pipefd[0]);
    Fd sig_wr(pipefd[1]);
    g_sig_wr = sig_wr.get();

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);

    // Subscribe BEFORE the first evaluation so a change that lands while
    // we are still waiting on Settings is not lost.
    Fd nl = open_netlink();
    if (!nl.valid()) {
        Logger::err("yay_dns: cannot open NETLINK_ROUTE socket");
        return 1;
    }

    Logger::info("yay_dns: started (wifi=" + cfg.wifi_host + ", mobile=" + cfg.mobile_host +
                 ", debounce=" + std::to_string(cfg.debounce_ms) + "ms)");

    // Boot: `cmd settings` may not answer yet even after boot_completed.
    // Bounded one-shot wait — this is the only startup polling there is.
    bool ready = false;
    for (int i = 0; i < kSettingsReadyTries && !g_stop; ++i) {
        if (settings_get(kKeyMode)) {
            ready = true;
            break;
        }
        if (wait_ms(sig_rd.get(), kSettingsReadyStepMs)) {
            break;
        }
    }
    if (!ready && !g_stop) {
        Logger::warn("yay_dns: Settings Provider not ready after waiting, "
                     "continuing — will retry on the next network event");
    }

    using clock = std::chrono::steady_clock;
    const auto debounce = [&cfg] { return std::chrono::milliseconds(cfg.debounce_ms); };

    Tracker tr;
    EvalMemo memo;
    bool pending = true;               // initial evaluation, immediately
    bool quit = false;
    auto first = clock::now();         // first event of the current burst
    auto deadline = first;

    while (!g_stop && !quit) {
        if (g_reload) {
            g_reload = 0;
            AutoDnsConfig fresh;
            std::string err;
            switch (load_config(fresh, &err)) {
                case CfgState::Ok:
                    cfg = std::move(fresh);
                    memo.settled = false; // new hosts/prefer: must re-check
                    Logger::info("yay_dns: config reloaded (wifi=" + cfg.wifi_host +
                                 ", mobile=" + cfg.mobile_host + ")");
                    pending = true;
                    first = clock::now();
                    deadline = first;
                    break;
                case CfgState::Disabled:
                    Logger::info("yay_dns: disabled in dns_config.json, exiting");
                    quit = true;
                    break;
                case CfgState::Invalid:
                    Logger::err("yay_dns: reload rejected: " + err + " — keeping previous config");
                    break;
            }
            if (quit) {
                break;
            }
        }

        // Idle: timeout -1 == sleep until the kernel has something to say.
        int timeout_ms = -1;
        if (pending) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - clock::now()).count();
            timeout_ms = left > 0 ? static_cast<int>(std::min<long long>(left, INT_MAX)) : 0;
        }

        pollfd fds[2] = {
            {nl.get(), POLLIN, 0},
            {sig_rd.get(), POLLIN, 0},
        };
        const int rc = ::poll(fds, 2, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            Logger::err("yay_dns: poll failed");
            break;
        }

        if ((fds[1].revents & POLLIN) != 0) {
            drain_fd(sig_rd.get());
        }

        if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            if (drain_netlink(nl.get(), cfg)) {
                const auto now = clock::now();
                if (!pending) {
                    pending = true;
                    first = now;
                }
                // Trailing-edge debounce, capped so endless flapping still
                // converges on a decision instead of deferring forever.
                deadline = std::min(now + debounce(),
                                    first + debounce() * kMaxDebounceStretch);
            }
        }

        if (pending && clock::now() >= deadline) {
            pending = false;
            static_cast<void>(evaluate(sig_rd.get(), tr, memo, cfg));
        }
    }

    Logger::info("yay_dns: stopping");
    g_sig_wr = -1;
    return 0;
}

// ─── CLI commands ────────────────────────────────────────────────────────────

// Detach a daemon, or — if one is already running — make it reload its
// config. Idempotent, so the WebUI can call it on every "Save".
int cmd_start()
{
    const pid_t running = lock_holder();
    if (running > 0) {
        ::kill(running, SIGHUP);
        std::puts("yay_dns: already running, config reloaded");
        return 0;
    }

    AutoDnsConfig cfg;
    std::string err;
    switch (load_config(cfg, &err)) {
        case CfgState::Ok:
            break;
        case CfgState::Disabled:
            std::fputs("yay_dns: disabled in dns_config.json\n", stderr);
            return 3;
        case CfgState::Invalid:
            std::fprintf(stderr, "yay_dns: %s\n", err.c_str());
            return 2;
    }

    const pid_t p1 = ::fork();
    if (p1 < 0) {
        std::fputs("yay_dns: fork failed\n", stderr);
        return 1;
    }
    if (p1 > 0) {
        int status = 0;
        static_cast<void>(::waitpid(p1, &status, 0));
        std::puts("yay_dns: started");
        return 0;
    }

    // Intermediate child: new session, then fork again so the daemon is
    // never a session leader and can't reacquire a controlling terminal.
    ::setsid();
    const pid_t p2 = ::fork();
    if (p2 != 0) {
        _exit(p2 < 0 ? 1 : 0);
    }

    // Detach stdio fully — a caller capturing our output (KernelSU WebUI
    // exec) would otherwise wait for an EOF that never comes.
    ::umask(077);
    static_cast<void>(::chdir("/"));
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            ::close(devnull);
        }
    }
    _exit(run_daemon());
}

int cmd_stop()
{
    const pid_t pid = lock_holder();
    if (pid <= 0) {
        std::puts("yay_dns: not running");
        return 0;
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20 && lock_holder() > 0; ++i) {
        ::poll(nullptr, 0, 100);
    }
    std::puts("yay_dns: stopped");
    return 0;
}

// Evaluate and apply once, without a daemon (debugging / "apply now").
// Uses a fresh Tracker, so it adopts the current setting like a fresh boot.
int cmd_once()
{
    Logger::init("yay_dns", kLogFile);

    AutoDnsConfig cfg;
    std::string err;
    switch (load_config(cfg, &err)) {
        case CfgState::Ok:
            break;
        case CfgState::Disabled:
            std::fputs("yay_dns: disabled in dns_config.json\n", stderr);
            return 3;
        case CfgState::Invalid:
            std::fprintf(stderr, "yay_dns: %s\n", err.c_str());
            return 2;
    }

    Tracker tr;
    EvalMemo memo;
    std::puts(evaluate(-1, tr, memo, cfg).c_str()); // fd -1: poll() just sleeps
    return 0;
}

int cmd_status()
{
    AutoDnsConfig cfg;
    std::string err;
    const CfgState st = load_config(cfg, &err);

    Json j;
    j["running"] = lock_holder() > 0;
    j["enabled"] = (st == CfgState::Ok);
    if (st == CfgState::Invalid) {
        j["config_error"] = err;
    } else {
        j["wifi_host"] = cfg.wifi_host;
        j["mobile_host"] = cfg.mobile_host;
        j["network"] = net_name(detect_network(cfg));
    }

    if (const auto cur = read_current()) {
        j["current"] = {{"mode", cur->mode}, {"host", cur->host}};
    } else {
        j["current"] = nullptr;
    }

    std::puts(j.dump().c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const char* arg = (argc > 1) ? argv[1] : "--run";

    if (std::strcmp(arg, "--run") == 0 || std::strcmp(arg, "--daemon") == 0) {
        return run_daemon();
    }
    if (std::strcmp(arg, "--start") == 0)  return cmd_start();
    if (std::strcmp(arg, "--stop") == 0)   return cmd_stop();
    if (std::strcmp(arg, "--once") == 0)   return cmd_once();
    if (std::strcmp(arg, "--status") == 0) return cmd_status();

    std::fputs(
        "usage: yay_dns [--run | --start | --stop | --once | --status]\n"
        "  --run     run in the foreground (default)\n"
        "  --start   start detached, or reload the running one\n"
        "  --stop    stop the running daemon\n"
        "  --once    evaluate + apply once, no daemon\n"
        "  --status  print state as JSON\n",
        stderr);
    return 64;
}
