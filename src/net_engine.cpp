#include "net_engine.h"

#include "exec_util.h"
#include "logger.h"
#include "net_presets.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <set>
#include <sstream>

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

constexpr std::string_view kSysNet = "/proc/sys/net/";

[[nodiscard]]
std::string slurp(const std::string& path)
{
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

[[nodiscard]]
std::string trim_ws(std::string s)
{
    const auto ns = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return ns(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return ns(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

[[nodiscard]]
bool run_ok(std::initializer_list<std::string_view> argv, int timeout = 5)
{
    return exec_cmd(argv, timeout).ok();
}

} // namespace

NetEngine::NetEngine(KnobStore& knobs, Stash& stash, std::string root)
    : knobs_(knobs)
    , stash_(stash)
    , root_(std::move(root))
{
}

// ── Settings.Global helpers ─────────────────────────────────────────────────

std::optional<std::string> NetEngine::settings_get(const char* key) const
{
    const auto r = exec_cmd({"cmd", "settings", "get", "global", key}, 5);
    if (!r.ok()) {
        return std::nullopt;
    }
    std::string s = trim_ws(r.stdout_str);
    if (s.empty()) {
        return std::string("null");
    }
    // exec_cmd merges stderr into stdout; a real value is a single token.
    if (s.find_first_of(" \t\n") != std::string::npos) {
        return std::nullopt;
    }
    return s;
}

void NetEngine::settings_put(const char* key, const std::string& value) const
{
    static_cast<void>(run_ok({"cmd", "settings", "put", "global", key, value}));
}

void NetEngine::settings_del(const char* key) const
{
    static_cast<void>(run_ok({"cmd", "settings", "delete", "global", key}));
}

// ── profile / tcp ───────────────────────────────────────────────────────────

void NetEngine::apply_profile(const NetConfig& cfg, std::string_view profile)
{
    std::size_t applied = 0, unchanged = 0, missing = 0, failed = 0;
    std::string failed_keys;

    if (profile == "off") {
        const std::size_t n = knobs_.restore("profile");
        if (profile_ != "off") {
            Logger::info("yay_net: profile off — restored " + std::to_string(n) + " knob(s) to device values");
        }
        last_changed_ = 0; last_failed_ = 0; last_missing_ = 0;
    } else if (const Preset* p = find_preset(profile)) {
        for (std::size_t i = 0; i < p->count; ++i) {
            const std::string path = std::string(kSysNet) + std::string(p->knobs[i].key);
            switch (knobs_.set(path, p->knobs[i].value, "profile")) {
                case KnobResult::Applied:   ++applied; break;
                case KnobResult::Unchanged: ++unchanged; break;
                case KnobResult::Missing:   ++missing; break;
                case KnobResult::Failed:
                    ++failed;
                    if (failed_keys.size() < 200U) {
                        failed_keys += (failed_keys.empty() ? "" : ", ") + std::string(p->knobs[i].key);
                    }
                    break;
            }
        }
        knobs_.flush();
        last_changed_ = applied; last_failed_ = failed; last_missing_ = missing;
        if (profile_ != profile || applied > 0 || failed > 0) {
            Logger::info("yay_net: profile " + std::string(profile) + ": " + std::to_string(applied) +
                         " written, " + std::to_string(unchanged) + " already set, " +
                         std::to_string(missing) + " not on this kernel, " + std::to_string(failed) + " failed" +
                         (failed_keys.empty() ? std::string() : " (" + failed_keys + ")"));
        }
    } else {
        Logger::warn("yay_net: unknown profile \"" + std::string(profile) + "\"");
        return;
    }

    // Wi-Fi low-latency: part of the game preset, or forced by the toggle.
    const Preset* p = find_preset(profile);
    set_low_latency(cfg.toggles.low_latency || (p != nullptr && p->low_latency));
    profile_ = std::string(profile);
}

void NetEngine::apply_tcp(const NetConfig& cfg, std::string_view profile)
{
    static constexpr const char* kAlgoPath = "/proc/sys/net/ipv4/tcp_congestion_control";
    static constexpr const char* kAvailPath = "/proc/sys/net/ipv4/tcp_available_congestion_control";

    if (cfg.tcp_algo.empty()) {
        knobs_.restore("tcp");
        return;
    }

    std::string algo;
    if (cfg.tcp_algo == "auto") {
        const auto avail = knobs_.read(kAvailPath);
        if (!avail) return;
        const auto pick = pick_available(tcp_preference(profile), *avail);
        if (!pick) return;
        algo = *pick;
    } else {
        algo = cfg.tcp_algo;
    }

    const KnobResult r = knobs_.set(kAlgoPath, algo, "tcp");
    knobs_.flush();
    if (r == KnobResult::Applied) {
        Logger::info("yay_net: TCP congestion control -> " + algo);
    } else if (r == KnobResult::Failed) {
        Logger::warn("yay_net: kernel refused TCP congestion control \"" + algo + "\"");
    }
}

// ── toggles ─────────────────────────────────────────────────────────────────

void NetEngine::set_ipv6(bool on)
{
    if (!on) {
        if (knobs_.changed_count("ipv6") > 0) {
            knobs_.restore("ipv6");
            knobs_.flush();
            Logger::info("yay_net: IPv6 restored");
        }
        return;
    }
    std::size_t n = 0;
    for (const char* scope : {"all", "default", "lo"}) {
        const std::string path = std::string(kSysNet) + "ipv6/conf/" + scope + "/disable_ipv6";
        if (knobs_.set(path, "1", "ipv6") == KnobResult::Applied) ++n;
    }
    knobs_.flush();
    if (n > 0) Logger::info("yay_net: IPv6 disabled");
}

void NetEngine::set_conntrack(bool on)
{
    if (!on) {
        if (knobs_.changed_count("conntrack") > 0) {
            knobs_.restore("conntrack");
            knobs_.flush();
            Logger::info("yay_net: conntrack restored");
        }
        return;
    }
    static constexpr std::pair<const char*, const char*> kCt[] = {
        {"netfilter/nf_conntrack_max", "32768"},
        {"netfilter/nf_conntrack_tcp_timeout_established", "600"},
        {"netfilter/nf_conntrack_tcp_timeout_time_wait", "30"},
        {"netfilter/nf_conntrack_udp_timeout", "15"},
        {"netfilter/nf_conntrack_udp_timeout_stream", "60"},
    };
    std::size_t n = 0;
    for (const auto& [k, v] : kCt) {
        if (knobs_.set(std::string(kSysNet) + k, v, "conntrack") == KnobResult::Applied) ++n;
    }
    knobs_.flush();
    if (n > 0) Logger::info("yay_net: conntrack tuned");
}

void NetEngine::set_irq(bool on)
{
    if (!on) {
        if (knobs_.changed_count("irq") > 0) {
            knobs_.restore("irq");   // each IRQ goes back to ITS OWN original mask
            knobs_.flush();
            Logger::info("yay_net: IRQ affinity restored");
        }
        return;
    }

    long max_cpu = 0;
    {
        const std::string present = trim_ws(slurp(root_ + "/sys/devices/system/cpu/present"));
        const auto dash = present.find('-');
        max_cpu = std::strtol(present.c_str() + (dash == std::string::npos ? 0 : dash + 1), nullptr, 10);
    }
    const std::string mask = irq_mask_for_cpus(max_cpu);

    std::istringstream in(slurp(root_ + "/proc/interrupts"));
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        std::string low = line;
        for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("wlan") == std::string::npos && low.find("rmnet") == std::string::npos &&
            low.find("ccmni") == std::string::npos) {
            continue;
        }
        const auto first = line.find_first_not_of(" \t");
        const auto colon = line.find(':');
        if (first == std::string::npos || colon == std::string::npos || colon <= first) continue;
        const std::string irq = line.substr(first, colon - first);
        if (!std::all_of(irq.begin(), irq.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) continue;
        if (knobs_.set("/proc/irq/" + irq + "/smp_affinity", mask, "irq") == KnobResult::Applied) ++n;
    }
    knobs_.flush();
    if (n > 0) Logger::info("yay_net: " + std::to_string(n) + " NIC IRQ(s) pinned to mask " + mask);
}

void NetEngine::set_ksoft(bool on)
{
    if (on == ksoft_applied_) {
        return;
    }
    DIR* d = ::opendir((root_ + "/proc").c_str());
    if (!d) return;

    std::size_t n = 0;
    while (const dirent* e = ::readdir(d)) {
        if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        const int pid = std::atoi(e->d_name);
        std::string comm = trim_ws(slurp(root_ + "/proc/" + e->d_name + "/comm"));
        if (comm.rfind("ksoftirqd", 0) != 0) continue;

        if (on) {
            errno = 0;
            const int cur = ::getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
            if (errno != 0) continue;
            ksoft_orig_.emplace(pid, cur);   // first sighting only
            if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), -20) == 0) ++n;
        } else if (const auto it = ksoft_orig_.find(pid); it != ksoft_orig_.end()) {
            if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), it->second) == 0) ++n;
        }
    }
    ::closedir(d);
    if (!on) ksoft_orig_.clear();
    ksoft_applied_ = on;
    Logger::info(std::string("yay_net: ksoftirqd priority ") + (on ? "raised" : "restored") + " (" + std::to_string(n) + ")");
}

// The Wi-Fi station interface, or "" when there is none right now (radio off,
// or a device that has not brought it up yet) — in which case there is nothing
// for `iw` to do and no process should be spawned.
std::string NetEngine::wlan_iface() const
{
    if (::access((root_ + "/sys/class/net/wlan0").c_str(), F_OK) == 0) {
        return "wlan0";
    }
    DIR* d = ::opendir((root_ + "/sys/class/net").c_str());
    if (!d) return {};
    std::string found;
    while (const dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind("wlan", 0) == 0 && n.find("p2p") == std::string::npos) {
            found = n;
            break;
        }
    }
    ::closedir(d);
    return found;
}

void NetEngine::set_wifi_ps(bool off)
{
    if (off) {
        const std::string ifc = wlan_iface();
        if (ifc.empty() || iw_missing_) return;
        if (wps_orig_.empty()) {
            const auto r = exec_cmd({"iw", "dev", ifc, "get", "power_save"}, 5);
            if (r.exit_code == -1 || r.exit_code == 127) {
                iw_missing_ = true;
                Logger::warn("yay_net: `iw` not available on this ROM — Wi-Fi power-save toggle unsupported");
                return;
            }
            wps_orig_ = (r.stdout_str.find("off") != std::string::npos) ? "off" : "on";
        }
        if (run_ok({"iw", "dev", ifc, "set", "power_save", "off"})) {
            if (!wps_applied_) Logger::info("yay_net: Wi-Fi power-save off on " + ifc);
            wps_applied_ = true;
            wps_iface_ = ifc;
        }
    } else if (wps_applied_) {
        if (!wps_iface_.empty() && ::access((root_ + "/sys/class/net/" + wps_iface_).c_str(), F_OK) == 0) {
            static_cast<void>(run_ok({"iw", "dev", wps_iface_, "set", "power_save", wps_orig_.empty() ? "on" : wps_orig_}));
        }
        wps_applied_ = false;
        wps_orig_.clear();
        Logger::info("yay_net: Wi-Fi power-save restored");
    }
}

void NetEngine::set_low_latency(bool on)
{
    if (on == ll_applied_) {
        return;
    }
    static_cast<void>(run_ok({"cmd", "wifi", "force-low-latency-mode", on ? "enabled" : "disabled"}));
    ll_applied_ = on;
}

void NetEngine::set_netstats(bool off)
{
    if (off) {
        if (netstats_applied_) return;
        if (!stash_.has("netstats_enabled")) {
            stash_.put("netstats_enabled", settings_get("netstats_enabled").value_or("null"));
        }
        settings_put("netstats_enabled", "0");
        netstats_applied_ = true;
        Logger::info("yay_net: network-stats collection off");
    } else if (stash_.has("netstats_enabled")) {
        const std::string orig = stash_.get("netstats_enabled").value_or("null");
        if (orig == "null") settings_del("netstats_enabled");
        else settings_put("netstats_enabled", orig);
        stash_.erase("netstats_enabled");
        netstats_applied_ = false;
        Logger::info("yay_net: network-stats collection restored");
    }
}

void NetEngine::set_data_saver(bool on)
{
    if (on) {
        if (saver_applied_) return;
        if (!stash_.has("saver_ingress")) {
            stash_.put("saver_ingress", settings_get("ingress_rate_limit_bytes_per_second").value_or("null"));
        }
        if (!stash_.has("saver_restrict")) {
            const auto r = exec_cmd({"cmd", "netpolicy", "get", "restrict-background"}, 5);
            stash_.put("saver_restrict", r.stdout_str.find("enabled") != std::string::npos &&
                                         r.stdout_str.find("disabled") == std::string::npos ? "true" : "false");
        }
        static_cast<void>(run_ok({"cmd", "netpolicy", "set", "restrict-background", "true"}));
        settings_put("ingress_rate_limit_bytes_per_second", "1048576");
        saver_applied_ = true;
        Logger::info("yay_net: data saver on (background restricted, 1 MB/s ingress cap)");
    } else if (stash_.has("saver_restrict") || stash_.has("saver_ingress")) {
        static_cast<void>(run_ok({"cmd", "netpolicy", "set", "restrict-background",
                                  stash_.get("saver_restrict").value_or("false")}));
        const std::string ing = stash_.get("saver_ingress").value_or("null");
        if (ing == "null") settings_del("ingress_rate_limit_bytes_per_second");
        else settings_put("ingress_rate_limit_bytes_per_second", ing);
        stash_.erase("saver_restrict");
        stash_.erase("saver_ingress");
        saver_applied_ = false;
        Logger::info("yay_net: data saver restored");
    }
}

void NetEngine::apply_toggles(const NetConfig& cfg, std::string_view)
{
    const NetToggles& t = cfg.toggles;
    set_ipv6(t.ipv6_off);
    set_conntrack(t.conntrack);
    set_irq(t.irq_affinity);
    set_ksoft(t.ksoft_boost);
    set_wifi_ps(t.wifi_ps_off);
    set_netstats(t.netstats_off);
    set_data_saver(t.data_saver);
}

// ── per-app rules ───────────────────────────────────────────────────────────

void NetEngine::apply_rules(const NetConfig& cfg)
{
    std::set<std::string> wanted(cfg.boost_apps.begin(), cfg.boost_apps.end());
    wanted.insert(cfg.jail_apps.begin(), cfg.jail_apps.end());

    // One file read for ALL packages. PingPimp spawned `pm list packages -U`
    // (a full package dump through the framework) once per app.
    const auto uids = parse_packages_list(slurp(root_ + "/data/system/packages.list"), wanted);

    std::vector<unsigned> boost, jail;
    std::size_t missing = 0;
    for (const auto& p : cfg.boost_apps) {
        if (const auto it = uids.find(p); it != uids.end()) boost.push_back(it->second); else ++missing;
    }
    for (const auto& p : cfg.jail_apps) {
        if (const auto it = uids.find(p); it != uids.end()) jail.push_back(it->second); else ++missing;
    }
    if (missing != missing_packages_) {
        missing_packages_ = missing;
        if (missing > 0) {
            Logger::warn("yay_net: " + std::to_string(missing) + " configured app(s) are not installed — skipped");
        }
    }

    const bool had = rules_.state().active;
    const RulesState s = rules_.apply(boost, jail);
    if (s.active != had || (s.active && (s.boost != 0 || s.jail != 0))) {
        const std::string msg = "yay_net: rules — " + std::to_string(s.boost) + " boosted (" +
                                (s.dscp ? "DSCP" : "MARK") + "), " + std::to_string(s.jail) + " jailed";
        if (msg != last_rules_msg_) {
            last_rules_msg_ = msg;
            Logger::info(msg);
        }
    }
}

// ── public ──────────────────────────────────────────────────────────────────

void NetEngine::apply(const NetConfig& cfg, std::string_view profile)
{
    apply_profile(cfg, profile);
    apply_tcp(cfg, profile);
    apply_toggles(cfg, profile);
    apply_rules(cfg);
}

void NetEngine::reassert(const NetConfig& cfg)
{
    rules_.reassert();
    // Drivers re-enable Wi-Fi power-save on every (re)association.
    if (cfg.toggles.wifi_ps_off) {
        wps_applied_ = false;
        set_wifi_ps(true);
    }
}

void NetEngine::restore_all()
{
    knobs_.restore_all();
    knobs_.flush();

    set_ksoft(false);
    set_wifi_ps(false);
    set_low_latency(false);
    set_netstats(false);
    set_data_saver(false);
    rules_.teardown();
    profile_.clear();
}

Json NetEngine::status(const NetConfig& cfg) const
{
    Json j;
    j["profile"] = profile_;
    j["knobs"] = {
        {"changed", knobs_.changed_count()},
        {"last_written", last_changed_},
        {"last_failed", last_failed_},
        {"not_on_kernel", last_missing_},
    };
    const RulesState& r = rules_.state();
    j["rules"] = {{"active", r.active}, {"boost", r.boost}, {"jail", r.jail}, {"mode", r.dscp ? "dscp" : "mark"}};

    if (const auto cur = knobs_.read("/proc/sys/net/ipv4/tcp_congestion_control")) j["tcp"]["current"] = *cur;
    if (const auto av = knobs_.read("/proc/sys/net/ipv4/tcp_available_congestion_control")) {
        Json arr = Json::array();
        std::istringstream in(*av);
        for (std::string a; in >> a;) arr.push_back(a);
        j["tcp"]["available"] = arr;
    }
    j["toggles_unsupported"] = Json::array();
    if (iw_missing_) j["toggles_unsupported"].push_back("wifi_ps_off");
    static_cast<void>(cfg);
    return j;
}
