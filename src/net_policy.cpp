#include "net_policy.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace {

[[nodiscard]]
std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]]
std::string trim(std::string_view s)
{
    const auto sp = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && sp(s.front())) s.remove_prefix(1U);
    while (!s.empty() && sp(s.back())) s.remove_suffix(1U);
    return std::string(s);
}

[[nodiscard]]
bool valid_algo(std::string_view a) noexcept
{
    if (a.empty() || a.size() > 16U) {
        return false;
    }
    return std::all_of(a.begin(), a.end(), [](char c) {
        return std::islower(static_cast<unsigned char>(c)) != 0 ||
               std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
    });
}

// Package list -> deduped, validated, order-preserving.
[[nodiscard]]
bool read_packages(const Json& cfg, const char* key, std::vector<std::string>& out, std::string* err)
{
    if (!cfg.contains(key)) {
        return true;
    }
    std::vector<std::string> parsed;
    std::set<std::string> seen;
    for (const auto& item : cfg[key]) {
        const std::string pkg = trim(item.get<std::string>());
        if (!valid_package(pkg)) {
            if (err) *err = std::string(key) + ": invalid package name \"" + pkg + "\"";
            return false;
        }
        if (seen.insert(pkg).second) {
            parsed.push_back(pkg);
        }
    }
    out = std::move(parsed);
    return true;
}

[[nodiscard]]
bool contains(std::string_view hay, std::string_view needle) noexcept
{
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

bool valid_package(std::string_view pkg) noexcept
{
    if (pkg.empty() || pkg.size() > 255U || pkg.front() == '.') {
        return false;
    }
    return std::all_of(pkg.begin(), pkg.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_';
    });
}

bool valid_profile(std::string_view name) noexcept
{
    static constexpr std::string_view kNames[] = {
        "off", "balanced", "game", "download", "streaming", "social", "browsing", "outdoor",
    };
    return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

std::optional<NetConfig> parse_net_config(const Json& cfg, std::string* error_out)
{
    const auto fail = [&](std::string msg) -> std::optional<NetConfig> {
        if (error_out) *error_out = std::move(msg);
        return std::nullopt;
    };

    try {
        NetConfig out;
        out.enabled = cfg.value("enabled", false);
        out.auto_mode = cfg.value("auto", false);
        out.auto_detect_games = cfg.value("auto_detect_games", true);

        out.profile = lower(trim(cfg.value("profile", std::string("balanced"))));
        if (!valid_profile(out.profile)) {
            return fail("profile \"" + out.profile + "\" is not a known profile");
        }

        out.tcp_algo = lower(trim(cfg.value("tcp_algo", std::string{})));
        if (!out.tcp_algo.empty() && out.tcp_algo != "auto" && !valid_algo(out.tcp_algo)) {
            return fail("tcp_algo \"" + out.tcp_algo + "\" is not a valid algorithm name");
        }

        out.confirm_ms = std::clamp(cfg.value("confirm_ms", out.confirm_ms), kMinConfirmMs, kMaxConfirmMs);
        out.debounce_ms = std::clamp(cfg.value("debounce_ms", out.debounce_ms), kMinNetDebounceMs, kMaxNetDebounceMs);

        std::string err;
        if (!read_packages(cfg, "boost_apps", out.boost_apps, &err) ||
            !read_packages(cfg, "jail_apps", out.jail_apps, &err)) {
            return fail(err);
        }

        if (cfg.contains("app_profiles")) {
            for (const auto& [pkg, prof] : cfg["app_profiles"].items()) {
                const std::string p = lower(trim(prof.get<std::string>()));
                if (!valid_package(pkg)) {
                    return fail("app_profiles: invalid package name \"" + pkg + "\"");
                }
                if (!valid_profile(p)) {
                    return fail("app_profiles: \"" + p + "\" is not a known profile");
                }
                out.app_profiles[pkg] = p;
            }
        }

        if (cfg.contains("toggles")) {
            const Json& t = cfg["toggles"];
            out.toggles.ipv6_off = t.value("ipv6_off", false);
            out.toggles.wifi_ps_off = t.value("wifi_ps_off", false);
            out.toggles.low_latency = t.value("low_latency", false);
            out.toggles.conntrack = t.value("conntrack", false);
            out.toggles.netstats_off = t.value("netstats_off", false);
            out.toggles.data_saver = t.value("data_saver", false);
            out.toggles.irq_affinity = t.value("irq_affinity", false);
            out.toggles.ksoft_boost = t.value("ksoft_boost", false);
        }
        return out;
    } catch (const Json::exception& e) {
        return fail(std::string("net_config: ") + e.what());
    } catch (...) {
        return fail("net_config: unknown error");
    }
}

bool is_wan_iface(std::string_view name) noexcept
{
    static constexpr std::string_view kPrefixes[] = {
        "wlan", "rmnet", "v4-", "ccmni", "ccemni", "seth", "sipa", "pdp", "ppp", "wwan",
    };
    if (name.rfind("p2p", 0) == 0) {
        return false; // Wi-Fi Direct
    }
    return std::any_of(std::begin(kPrefixes), std::end(kPrefixes),
                       [&](std::string_view p) { return name.substr(0, p.size()) == p; });
}

std::string classify_app(std::string_view pkg)
{
    const std::string p = lower(pkg);
    static constexpr std::string_view kStreaming[] = {
        "tube", "netflix", "twitch", "disney", "hulu", "spotify", "bilibili", "viu", "iqiyi", "music",
    };
    static constexpr std::string_view kBrowsing[] = {
        "chrome", "firefox", "opera", "edge", "brave", "ucbrowser",
    };
    static constexpr std::string_view kSocial[] = {
        "whatsapp", "insta", "facebook", "twitter", "tiktok", "gram", "snap", "reddit",
    };
    for (const auto k : kStreaming) if (contains(p, k)) return "streaming";
    for (const auto k : kBrowsing)  if (contains(p, k)) return "browsing";
    for (const auto k : kSocial)    if (contains(p, k)) return "social";
    return {};
}

std::string profile_for_app(const NetConfig& cfg, std::string_view pkg, bool is_game)
{
    if (const auto it = cfg.app_profiles.find(std::string(pkg)); it != cfg.app_profiles.end()) {
        return it->second;
    }
    if (is_game) {
        return "game";
    }
    if (const std::string cat = classify_app(pkg); !cat.empty()) {
        return cat;
    }
    return cfg.profile;
}

std::string effective_profile(
    const NetConfig& cfg, const AutoState& st, const std::set<std::string>& games)
{
    if (!cfg.auto_mode || !st.screen_on || st.committed_pkg.empty()) {
        return cfg.profile;
    }
    const bool is_game = cfg.auto_detect_games && games.count(st.committed_pkg) > 0;
    return profile_for_app(cfg, st.committed_pkg, is_game);
}

std::vector<std::string_view> tcp_preference(std::string_view profile)
{
    if (profile == "game") {
        return {"westwood", "bbr", "cubic"};
    }
    if (profile == "download" || profile == "streaming") {
        return {"bbr", "cubic", "reno"};
    }
    return {"cubic", "bbr", "reno"};
}

std::optional<std::string> pick_available(
    const std::vector<std::string_view>& prefs, std::string_view available)
{
    for (const auto want : prefs) {
        std::size_t pos = 0;
        while (pos < available.size()) {
            while (pos < available.size() && std::isspace(static_cast<unsigned char>(available[pos])) != 0) ++pos;
            const std::size_t start = pos;
            while (pos < available.size() && std::isspace(static_cast<unsigned char>(available[pos])) == 0) ++pos;
            if (available.substr(start, pos - start) == want) {
                return std::string(want);
            }
        }
    }
    return std::nullopt;
}

std::string irq_mask_for_cpus(long max_cpu_index)
{
    if (max_cpu_index >= 7) return "c0";
    if (max_cpu_index >= 3) return "c";
    return "3";
}

NetEvent parse_event_line(std::string_view line)
{
    NetEvent ev;

    const auto payload_after = [&](std::string_view tag) -> std::optional<std::string_view> {
        const auto at = line.find(tag);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        // `tag( 1234): payload` (brief) or `tag: payload`
        const auto colon = line.find(':', at + tag.size());
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        return line.substr(colon + 1U);
    };

    for (const std::string_view tag : {"wm_set_resumed_activity", "am_focused_activity"}) {
        if (const auto p = payload_after(tag)) {
            // [userId,package/Activity,reason]
            const auto open = p->find('[');
            if (open == std::string_view::npos) continue;
            const auto c1 = p->find(',', open);
            if (c1 == std::string_view::npos) continue;
            const auto c2 = p->find(',', c1 + 1U);
            const auto comp = p->substr(c1 + 1U, (c2 == std::string_view::npos ? p->size() : c2) - c1 - 1U);
            const auto slash = comp.find('/');
            const std::string pkg = trim(comp.substr(0, slash));
            if (valid_package(pkg)) {
                ev.kind = EventKind::Foreground;
                ev.pkg = pkg;
                return ev;
            }
        }
    }

    if (const auto p = payload_after("screen_toggled")) {
        const std::string v = trim(*p);
        int n = 0;
        // tolerate "1", "[1]"
        const auto first = std::find_if(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        if (first != v.end()) {
            std::from_chars(&*first, v.data() + v.size(), n);
            ev.kind = EventKind::Screen;
            ev.screen_on = (n != 0);
        }
    }
    return ev;
}

std::map<std::string, unsigned> parse_packages_list(
    std::string_view text, const std::set<std::string>& wanted)
{
    std::map<std::string, unsigned> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1U;

        const auto sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) continue;
        const std::string pkg(line.substr(0, sp1));
        if (wanted.count(pkg) == 0) continue;

        std::string_view rest = line.substr(sp1 + 1U);
        const auto sp2 = rest.find(' ');
        rest = rest.substr(0, sp2);
        unsigned uid = 0;
        const auto res = std::from_chars(rest.data(), rest.data() + rest.size(), uid);
        if (res.ec == std::errc{} && res.ptr == rest.data() + rest.size()) {
            out[pkg] = uid;
        }
    }
    return out;
}

std::set<std::string> parse_declared_games(std::string_view out)
{
    std::set<std::string> games;
    std::string current;
    std::size_t pos = 0;
    while (pos < out.size()) {
        auto nl = out.find('\n', pos);
        if (nl == std::string_view::npos) nl = out.size();
        const std::string_view line = out.substr(pos, nl - pos);
        pos = nl + 1U;

        if (const auto at = line.find("packageName="); at != std::string_view::npos) {
            std::size_t e = at + 12U;
            const std::size_t s = e;
            while (e < line.size() && std::isspace(static_cast<unsigned char>(line[e])) == 0 && line[e] != ',') ++e;
            const std::string pkg(line.substr(s, e - s));
            current = valid_package(pkg) ? pkg : std::string{};
        }
        // whole-token match so "category=0" never matches e.g. "category=05"
        std::size_t t = 0;
        while ((t = line.find("category=0", t)) != std::string_view::npos) {
            const std::size_t end = t + 10U;
            const bool boundary = end >= line.size() || std::isspace(static_cast<unsigned char>(line[end])) != 0 || line[end] == ',';
            if (boundary && !current.empty()) {
                games.insert(current);
            }
            t = end;
        }
    }
    return games;
}

namespace {

[[nodiscard]]
std::vector<unsigned> sorted_unique(std::vector<unsigned> v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

} // namespace

std::string make_qos_script(const std::vector<unsigned>& uids, bool dscp)
{
    std::string s = "*mangle\n:";
    s += kQosChain;
    s += " - [0:0]\n";
    for (const unsigned uid : sorted_unique(uids)) {
        s += "-A ";
        s += kQosChain;
        s += " -m owner --uid-owner " + std::to_string(uid);
        s += dscp ? " -j DSCP --set-dscp 46\n" : " -j MARK --set-mark 0x40000000/0x40000000\n";
    }
    s += "COMMIT\n";
    return s;
}

std::string make_jail_script(const std::vector<unsigned>& uids)
{
    std::string s = "*filter\n:";
    s += kJailChain;
    s += " - [0:0]\n";
    for (const unsigned uid : sorted_unique(uids)) {
        s += "-A ";
        s += kJailChain;
        s += " -m owner --uid-owner " + std::to_string(uid) + " -j REJECT\n";
    }
    s += "COMMIT\n";
    return s;
}
