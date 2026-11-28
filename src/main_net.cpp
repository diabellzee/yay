// yay_net — network engine (profiles, per-app boost/jail, Smart Auto).
//
// Ported from PingPimp (https://github.com/fuckyoustan/PingPimp, Apache-2.0,
// (c) fuckyoustan) and re-architected around yay's rules; see NOTICE.
//
// What changed in the port, and why:
//
//  * No polling. PingPimp's daemon woke every 10 s to run `dumpsys power`,
//    every 10 s more (screen on) to run `cmd activity stack info` and re-exec
//    itself, and every 30 s to `ping` 1.1.1.1 four times. yay_net sleeps in
//    poll() with no timeout and wakes only for: a netlink link/address
//    change, a line on `logcat -b events` (foreground activity / screen
//    on-off — both are logged by system_server as they happen), or a signal.
//    The only timers are one-shot: a debounce after a network burst, and the
//    "app must stay foreground N seconds" confirmation.
//  * Everything that was a `sh -c` is a direct write or one process:
//    sysctl/sysfs via open()+write(), packages via ONE read of
//    /data/system/packages.list, per-app rules via ONE iptables-restore per
//    family into dedicated chains.
//  * Every change is snapshotted first and can be undone to the device's real
//    values (PingPimp wrote back its own constants).
//  * No active probing (ping/MTU discovery), no airplane-mode toggling, no
//    APN rewriting, no Wi-Fi firmware config overlay: see README.

#include "daemon_util.h"
#include "exec_util.h"
#include "json_config.h"
#include "logger.h"
#include "net_engine.h"
#include "net_knobs.h"
#include "net_policy.h"
#include "netlink_util.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using daemon_util::Fd;
using daemon_util::Signals;

namespace {

constexpr const char* kConfigPath   = "/data/adb/yay/config/net_config.json";
constexpr const char* kGamePath     = "/data/adb/yay/config/game_config.json";
constexpr const char* kLogFile      = "/data/adb/yay/run.log";
constexpr const char* kLockPath     = "/data/adb/yay/cache/yay_net.lock";
constexpr const char* kSnapshot     = "/data/adb/yay/cache/net_knobs.json";
constexpr const char* kStashPath    = "/data/adb/yay/cache/net_stash.json";
constexpr const char* kStatusPath   = "/data/adb/yay/cache/net_status.json";
constexpr const char* kGameCache    = "/data/adb/yay/cache/net_games.json";
constexpr const char* kPackagesList = "/data/system/packages.list";

constexpr int kMaxStretch = 5;          // cap on a debounce burst, in windows
constexpr int kMaxRespawns = 3;         // logcat restarts before giving up

using Clock = std::chrono::steady_clock;

enum class CfgState { Ok, Disabled, Invalid };

[[nodiscard]]
CfgState load_config(NetConfig& out, std::string* err)
{
    const auto j = load_json(kConfigPath);
    if (!j) {
        if (err) *err = std::string("cannot load ") + kConfigPath;
        return CfgState::Invalid;
    }
    std::string why;
    if (!validate_config_shape(*j, ConfigKind::NetConfig, &why)) {
        if (err) *err = "shape validation failed: " + why;
        return CfgState::Invalid;
    }
    auto parsed = parse_net_config(*j, &why);
    if (!parsed) {
        if (err) *err = why;
        return CfgState::Invalid;
    }
    out = std::move(*parsed);
    return out.enabled ? CfgState::Ok : CfgState::Disabled;
}

// ── games: user's game_config.json ∪ app-declared GAME category ─────────────

[[nodiscard]]
std::set<std::string> user_games()
{
    std::set<std::string> out;
    const auto j = load_json(kGamePath);
    if (!j || !j->contains("games") || !(*j)["games"].is_array()) {
        return out;
    }
    for (const auto& g : (*j)["games"]) {
        if (!g.is_object() || g.value("enabled", true) == false) continue;
        const std::string pkg = g.value("package", std::string{});
        if (valid_package(pkg)) out.insert(pkg);
    }
    return out;
}

[[nodiscard]]
long mtime_of(const char* path) noexcept
{
    struct stat st{};
    return ::stat(path, &st) == 0 ? static_cast<long>(st.st_mtime) : -1L;
}

// The scan is one `cmd package query-activities` (framework dump, ~seconds).
// Cache it keyed on packages.list's mtime: it only changes when an app is
// installed/removed, so a daemon restart does not repeat the work.
[[nodiscard]]
std::set<std::string> declared_games()
{
    const long stamp = mtime_of(kPackagesList);
    if (const auto cached = load_json(kGameCache);
        cached && stamp > 0 && cached->value("stamp", -1L) == stamp && cached->contains("games")) {
        std::set<std::string> out;
        for (const auto& g : (*cached)["games"]) {
            if (g.is_string()) out.insert(g.get<std::string>());
        }
        return out;
    }

    const auto r = exec_cmd({"cmd", "package", "query-activities", "-a", "android.intent.action.MAIN",
                             "-c", "android.intent.category.LAUNCHER"}, 25);
    if (!r.ok()) {
        return {};
    }
    auto games = parse_declared_games(r.stdout_str);

    Json j;
    j["stamp"] = stamp;
    j["games"] = games;
    static_cast<void>(save_json(kGameCache, j));
    return games;
}

// ── logcat events ───────────────────────────────────────────────────────────

class LogcatStream {
public:
    ~LogcatStream() { stop(); }

    [[nodiscard]] bool start()
    {
        stop();
        int p[2];
        if (::pipe2(p, O_CLOEXEC) != 0) return false;

        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(p[0]); ::close(p[1]);
            return false;
        }
        if (pid == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL); // never outlive the daemon
            const int dn = ::open("/dev/null", O_RDWR);
            if (dn >= 0) {
                ::dup2(dn, STDIN_FILENO);
                ::dup2(dn, STDERR_FILENO);
            }
            ::dup2(p[1], STDOUT_FILENO);
            // -T N replays the last N lines first, so the current foreground
            // app and screen state are known right away, with no dumpsys.
            ::execlp("logcat", "logcat", "-b", "events", "-T", "500", "-v", "brief", "-s",
                     "wm_set_resumed_activity:I", "am_focused_activity:I", "screen_toggled:I",
                     static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(p[1]);
        pid_ = pid;
        fd_ = Fd(p[0]);
        const int fl = ::fcntl(fd_.get(), F_GETFL);
        ::fcntl(fd_.get(), F_SETFL, fl | O_NONBLOCK);
        buf_.clear();
        return true;
    }

    void stop() noexcept
    {
        fd_.reset();
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int st = 0;
            while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
        buf_.clear();
    }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] bool running() const noexcept { return pid_ > 0; }

    // Feed every complete line to `on_line`. False once the stream ended.
    template <class F>
    [[nodiscard]] bool pump(F&& on_line)
    {
        char chunk[4096];
        for (;;) {
            const ssize_t n = ::read(fd_.get(), chunk, sizeof(chunk));
            if (n > 0) {
                buf_.append(chunk, static_cast<std::size_t>(n));
                std::size_t nl;
                while ((nl = buf_.find('\n')) != std::string::npos) {
                    on_line(std::string_view(buf_).substr(0, nl));
                    buf_.erase(0, nl + 1U);
                }
                if (buf_.size() > 65536U) buf_.clear(); // malformed stream guard
                continue;
            }
            if (n == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
    }

private:
    pid_t pid_ = -1;
    Fd fd_;
    std::string buf_;
};

// ── runtime status file (disposable: no fsync) ──────────────────────────────

void write_status(const Json& j) noexcept
{
    try {
        const std::string tmp = std::string(kStatusPath) + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << j.dump();
        }
        ::rename(tmp.c_str(), kStatusPath);
    } catch (...) {
    }
}

// ── the daemon ──────────────────────────────────────────────────────────────

class Daemon {
public:
    explicit Daemon(const Signals& sig)
        : sig_(sig)
        , knobs_("", kSnapshot, read_boot_id())
        , stash_(kStashPath)
        , engine_(knobs_, stash_, "")
    {
    }

    int run(NetConfig cfg)
    {
        cfg_ = std::move(cfg);

        nl_ = Fd(netlink_util::open_route_socket());
        if (!nl_.valid()) {
            Logger::warn("yay_net: no NETLINK_ROUTE socket — network-change re-assert disabled");
        }

        Logger::info("yay_net: started (profile=" + cfg_.profile + ", auto=" +
                     (cfg_.auto_mode ? "on" : "off") + ")");

        reconfigure(true);

        auto net_due = Clock::time_point::max();
        auto net_first = Clock::now();

        while (!Signals::stop_requested() && !quit_) {
            if (Signals::take_reload()) {
                handle_reload();
                if (quit_) break;
            }

            // Next one-shot deadline; none -> sleep until the kernel speaks.
            auto next = Clock::time_point::max();
            for (const auto d : {net_due, confirm_due_, respawn_due_}) next = std::min(next, d);
            int timeout_ms = -1;
            if (next != Clock::time_point::max()) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
                timeout_ms = left > 0 ? static_cast<int>(std::min<long long>(left, INT_MAX)) : 0;
            }

            pollfd fds[3] = {
                {sig_.fd(), POLLIN, 0},
                {nl_.get(), POLLIN, 0},
                {logcat_.running() ? logcat_.fd() : -1, POLLIN, 0},
            };
            const int rc = ::poll(fds, 3, timeout_ms);
            if (rc < 0) {
                if (errno == EINTR) continue;
                Logger::err("yay_net: poll failed");
                break;
            }

            if ((fds[0].revents & POLLIN) != 0) sig_.drain();

            if ((fds[1].revents & (POLLIN | POLLERR)) != 0 && nl_.valid()) {
                if (netlink_util::drain_route_events(nl_.get(), [](std::string_view n) { return is_wan_iface(n); })) {
                    const auto now = Clock::now();
                    if (net_due == Clock::time_point::max()) net_first = now;
                    const auto w = std::chrono::milliseconds(cfg_.debounce_ms);
                    net_due = std::min(now + w, net_first + w * kMaxStretch);
                }
            }

            if ((fds[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && logcat_.running()) {
                const bool alive = logcat_.pump([this](std::string_view line) { on_event_line(line); });
                if (!alive) on_logcat_gone();
            }

            const auto now = Clock::now();
            if (net_due != Clock::time_point::max() && now >= net_due) {
                net_due = Clock::time_point::max();
                engine_.reassert(cfg_);
            }
            if (confirm_due_ != Clock::time_point::max() && now >= confirm_due_) {
                confirm_due_ = Clock::time_point::max();
                commit_foreground();
            }
            if (respawn_due_ != Clock::time_point::max() && now >= respawn_due_) {
                respawn_due_ = Clock::time_point::max();
                start_events();
            }
        }

        logcat_.stop();
        Logger::info("yay_net: stopping");
        return 0;
    }

private:
    // (Re)apply the whole config. `force` re-applies even if the profile name
    // is unchanged (new toggles/lists/algo).
    void reconfigure(bool first)
    {
        state_.committed_pkg.clear();
        pending_.clear();
        confirm_due_ = Clock::time_point::max();

        if (cfg_.auto_mode) {
            games_ = cfg_.auto_detect_games ? user_games() : std::set<std::string>{};
            // Start with the base profile at once; the (slow) game scan and the
            // event replay refine it afterwards.
            engine_.apply(cfg_, cfg_.profile);
            publish();
            if (cfg_.auto_detect_games) {
                const auto declared = declared_games();
                games_.insert(declared.begin(), declared.end());
            }
            respawns_ = 0;
            start_events();
        } else {
            logcat_.stop();
            events_ok_ = false;
            respawn_due_ = Clock::time_point::max();
            games_.clear();
            engine_.apply(cfg_, cfg_.profile);
            publish();
        }
        static_cast<void>(first);
    }

    void handle_reload()
    {
        NetConfig fresh;
        std::string err;
        switch (load_config(fresh, &err)) {
            case CfgState::Ok:
                cfg_ = std::move(fresh);
                Logger::info("yay_net: config reloaded");
                reconfigure(false);
                break;
            case CfgState::Disabled:
                Logger::info("yay_net: disabled in net_config.json — restoring device values, exiting");
                logcat_.stop();
                engine_.restore_all();
                quit_ = true;
                break;
            case CfgState::Invalid:
                Logger::err("yay_net: reload rejected: " + err + " — keeping previous config");
                break;
        }
    }

    void start_events()
    {
        if (!cfg_.auto_mode) return;
        events_ok_ = logcat_.start();
        if (!events_ok_) {
            Logger::warn("yay_net: cannot start logcat — Smart Auto inactive");
        }
        publish();
    }

    void on_logcat_gone()
    {
        logcat_.stop();
        events_ok_ = false;
        if (cfg_.auto_mode && respawns_ < kMaxRespawns) {
            const int backoff = 2000 << respawns_; // 2 s, 4 s, 8 s
            ++respawns_;
            respawn_due_ = Clock::now() + std::chrono::milliseconds(backoff);
            Logger::warn("yay_net: logcat ended — restarting in " + std::to_string(backoff / 1000) + " s");
        } else if (cfg_.auto_mode) {
            Logger::err("yay_net: logcat keeps dying — Smart Auto inactive until config reload");
        }
        publish();
    }

    void on_event_line(std::string_view line)
    {
        const NetEvent ev = parse_event_line(line);
        switch (ev.kind) {
            case EventKind::Foreground:
                if (ev.pkg == state_.committed_pkg) {
                    pending_.clear();                       // came back: nothing to confirm
                    confirm_due_ = Clock::time_point::max();
                } else if (ev.pkg != pending_) {
                    pending_ = ev.pkg;
                    confirm_due_ = Clock::now() + std::chrono::milliseconds(cfg_.confirm_ms);
                }
                break;
            case EventKind::Screen:
                if (ev.screen_on != state_.screen_on) {
                    state_.screen_on = ev.screen_on;
                    if (!ev.screen_on) {                    // screen off: drop to base at once
                        pending_.clear();
                        confirm_due_ = Clock::time_point::max();
                    }
                    evaluate();
                }
                break;
            case EventKind::None:
                break;
        }
        events_ok_ = true;
    }

    void commit_foreground()
    {
        if (pending_.empty() || pending_ == state_.committed_pkg) return;
        state_.committed_pkg = pending_;
        pending_.clear();
        evaluate();
    }

    void evaluate()
    {
        const std::string prof = effective_profile(cfg_, state_, games_);
        if (prof != engine_.applied_profile()) {
            Logger::info("yay_net: auto: " + (state_.screen_on ? state_.committed_pkg : std::string("(screen off)")) +
                         " -> " + prof);
            engine_.apply(cfg_, prof);
        }
        publish();
    }

    void publish()
    {
        Json j = engine_.status(cfg_);
        j["pid"] = static_cast<long>(::getpid());
        j["base"] = cfg_.profile;
        j["auto"] = cfg_.auto_mode;
        j["foreground"] = state_.committed_pkg;
        j["screen_on"] = state_.screen_on;
        j["events_ok"] = events_ok_;
        j["games"] = games_.size();
        write_status(j);
    }

    const Signals& sig_;
    KnobStore knobs_;
    Stash stash_;
    NetEngine engine_;
    NetConfig cfg_;
    AutoState state_;
    std::set<std::string> games_;
    std::string pending_;
    LogcatStream logcat_;
    Fd nl_;
    bool events_ok_ = false;
    bool quit_ = false;
    int respawns_ = 0;
    Clock::time_point confirm_due_ = Clock::time_point::max();
    Clock::time_point respawn_due_ = Clock::time_point::max();
};

[[nodiscard]]
int run_daemon()
{
    Logger::init("yay_net", kLogFile);

    Fd lock = daemon_util::acquire_lock(kLockPath);
    if (!lock.valid()) {
        Logger::info("yay_net: another instance is already running, exiting");
        return 0;
    }

    NetConfig cfg;
    std::string err;
    switch (load_config(cfg, &err)) {
        case CfgState::Ok: break;
        case CfgState::Disabled:
            Logger::info("yay_net: disabled in net_config.json, exiting");
            return 0;
        case CfgState::Invalid:
            Logger::err("yay_net: " + err + " — exiting");
            return 1;
    }

    Signals sig;
    if (!sig.install()) {
        Logger::err("yay_net: cannot install signal handlers");
        return 1;
    }

    Daemon d(sig);
    return d.run(std::move(cfg));
}

// ── CLI ─────────────────────────────────────────────────────────────────────

int cmd_start()
{
    if (const pid_t running = daemon_util::lock_holder(kLockPath); running > 0) {
        ::kill(running, SIGHUP);
        std::puts("yay_net: already running, config reloaded");
        return 0;
    }

    NetConfig cfg;
    std::string err;
    switch (load_config(cfg, &err)) {
        case CfgState::Ok: break;
        case CfgState::Disabled:
            std::fputs("yay_net: disabled in net_config.json\n", stderr);
            return 3;
        case CfgState::Invalid:
            std::fprintf(stderr, "yay_net: %s\n", err.c_str());
            return 2;
    }
    if (!daemon_util::detach_and_run(run_daemon)) {
        std::fputs("yay_net: fork failed\n", stderr);
        return 1;
    }
    std::puts("yay_net: started");
    return 0;
}

void stop_running()
{
    const pid_t pid = daemon_util::lock_holder(kLockPath);
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 30 && daemon_util::lock_holder(kLockPath) > 0; ++i) {
        ::poll(nullptr, 0, 100);
    }
}

int cmd_stop()
{
    if (daemon_util::lock_holder(kLockPath) <= 0) {
        std::puts("yay_net: not running");
        return 0;
    }
    stop_running();
    std::puts("yay_net: stopped");
    return 0;
}

// Stop the daemon and put EVERYTHING back to what the device had — works from
// the persisted snapshot even if the daemon was killed hard.
int cmd_restore()
{
    Logger::init("yay_net", kLogFile);
    stop_running();
    KnobStore knobs("", kSnapshot, read_boot_id());
    Stash stash(kStashPath);
    NetEngine engine(knobs, stash, "");
    engine.restore_all();
    ::unlink(kStatusPath);
    Logger::info("yay_net: restored device values");
    std::puts("yay_net: restored");
    return 0;
}

int cmd_once()
{
    Logger::init("yay_net", kLogFile);
    NetConfig cfg;
    std::string err;
    switch (load_config(cfg, &err)) {
        case CfgState::Ok: break;
        case CfgState::Disabled:
            std::fputs("yay_net: disabled in net_config.json\n", stderr);
            return 3;
        case CfgState::Invalid:
            std::fprintf(stderr, "yay_net: %s\n", err.c_str());
            return 2;
    }
    KnobStore knobs("", kSnapshot, read_boot_id());
    Stash stash(kStashPath);
    NetEngine engine(knobs, stash, "");
    engine.apply(cfg, cfg.profile);
    std::printf("yay_net: applied %s\n", cfg.profile.c_str());
    return 0;
}

int cmd_status()
{
    NetConfig cfg;
    std::string err;
    const CfgState st = load_config(cfg, &err);

    KnobStore knobs("", kSnapshot, read_boot_id());
    Stash stash(kStashPath);
    NetEngine engine(knobs, stash, "");
    Json j = engine.status(cfg);

    const pid_t pid = daemon_util::lock_holder(kLockPath);
    j["running"] = pid > 0;
    j["enabled"] = (st == CfgState::Ok);
    if (st == CfgState::Invalid) j["config_error"] = err;
    if (pid > 0) {
        if (const auto live = load_json(kStatusPath); live && live->is_object()) {
            for (const auto& [k, v] : live->items()) j[k] = v;   // daemon's view wins
        }
    }
    std::puts(j.dump().c_str());
    return 0;
}

// Does THIS ROM emit the events Smart Auto depends on? Dumps the buffer and
// reports what parsed — run it once on a new device.
int cmd_selftest()
{
    const auto r = exec_cmd({"logcat", "-b", "events", "-d", "-t", "2000", "-v", "brief", "-s",
                             "wm_set_resumed_activity:I", "am_focused_activity:I", "screen_toggled:I"}, 15);
    Json j;
    j["logcat_ok"] = r.ok();
    int fg = 0, screen = 0;
    std::string last_pkg;
    std::size_t pos = 0;
    while (pos < r.stdout_str.size()) {
        auto nl = r.stdout_str.find('\n', pos);
        if (nl == std::string::npos) nl = r.stdout_str.size();
        const NetEvent ev = parse_event_line(std::string_view(r.stdout_str).substr(pos, nl - pos));
        pos = nl + 1U;
        if (ev.kind == EventKind::Foreground) { ++fg; last_pkg = ev.pkg; }
        if (ev.kind == EventKind::Screen) ++screen;
    }
    j["foreground_events"] = fg;
    j["screen_events"] = screen;
    j["last_foreground"] = last_pkg;
    j["smart_auto_supported"] = fg > 0;
    std::puts(j.dump().c_str());
    return fg > 0 ? 0 : 4;
}

} // namespace

int main(int argc, char* argv[])
{
    const char* arg = (argc > 1) ? argv[1] : "--run";
    if (!std::strcmp(arg, "--run") || !std::strcmp(arg, "--daemon")) return run_daemon();
    if (!std::strcmp(arg, "--start"))    return cmd_start();
    if (!std::strcmp(arg, "--stop"))     return cmd_stop();
    if (!std::strcmp(arg, "--restore"))  return cmd_restore();
    if (!std::strcmp(arg, "--once"))     return cmd_once();
    if (!std::strcmp(arg, "--status"))   return cmd_status();
    if (!std::strcmp(arg, "--selftest")) return cmd_selftest();

    std::fputs(
        "usage: yay_net [--run | --start | --stop | --restore | --once | --status | --selftest]\n"
        "  --run       run in the foreground (default)\n"
        "  --start     start detached, or make the running one reload its config\n"
        "  --stop      stop the daemon (leaves applied values in place)\n"
        "  --restore   stop the daemon and put every value back to the device's own\n"
        "  --once      apply the base profile once, no daemon\n"
        "  --status    print state as JSON\n"
        "  --selftest  check this ROM emits the logcat events Smart Auto needs\n",
        stderr);
    return 64;
}
