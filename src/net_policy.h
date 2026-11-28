#pragma once

// Pure decision logic for yay_net (the network engine). No I/O beyond the
// JSON handed to parse_net_config(), so every rule — what a foreground app
// maps to, how a logcat line is read, what the iptables script looks like —
// is unit-testable on a plain host. See tests/net_policy_test.cpp.

#include "json_config.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct NetToggles {
    bool ipv6_off = false;
    bool wifi_ps_off = false;    // keep Wi-Fi power-save off (anti-jitter)
    bool low_latency = false;    // force Wi-Fi low-latency mode
    bool conntrack = false;      // longer conntrack timeouts / bigger table
    bool netstats_off = false;   // stop network-stats collection
    bool data_saver = false;     // restrict background + ingress rate limit
    bool irq_affinity = false;   // pin NIC IRQs to the big cores
    bool ksoft_boost = false;    // raise ksoftirqd priority

    [[nodiscard]] bool operator==(const NetToggles&) const noexcept = default;
};

struct NetConfig {
    bool enabled = false;

    // Base profile: "off" (device originals) or a preset name.
    std::string profile = "balanced";

    // Foreground-app-driven profile switching.
    bool auto_mode = false;
    bool auto_detect_games = true; // app-declared category GAME + game_config.json
    int confirm_ms = 4000;         // app must stay foreground this long

    // Debounce for network-change bursts.
    int debounce_ms = 1000;

    // "" = leave alone, "auto" = per-profile preference, else an algorithm.
    std::string tcp_algo;

    std::vector<std::string> boost_apps; // DSCP/MARK
    std::vector<std::string> jail_apps;  // blocked

    // Explicit package -> profile, beats every heuristic.
    std::map<std::string, std::string> app_profiles;

    NetToggles toggles;
};

constexpr int kMinConfirmMs = 500;
constexpr int kMaxConfirmMs = 60000;
constexpr int kMinNetDebounceMs = 200;
constexpr int kMaxNetDebounceMs = 30000;

// Android package name: [A-Za-z0-9._] only, no leading '.', <= 255 chars.
[[nodiscard]] bool valid_package(std::string_view pkg) noexcept;

// "off", "browsing" or a preset name.
[[nodiscard]] bool valid_profile(std::string_view name) noexcept;

[[nodiscard]] std::optional<NetConfig> parse_net_config(
    const Json& cfg, std::string* error_out = nullptr);

// ── app -> profile ──────────────────────────────────────────────────────────

// "streaming" | "browsing" | "social" | "" (unknown). Substring heuristics
// carried over from PingPimp's classify_pkg().
[[nodiscard]] std::string classify_app(std::string_view pkg);

// Profile for a foreground app: explicit mapping > game > category > base.
[[nodiscard]] std::string profile_for_app(
    const NetConfig& cfg, std::string_view pkg, bool is_game);

struct AutoState {
    std::string committed_pkg; // foreground app that has held for confirm_ms
    bool screen_on = true;
};

// Profile that should be in force right now.
//   auto off / screen off / no committed app  -> the base profile
//   otherwise                                 -> profile_for_app()
[[nodiscard]] std::string effective_profile(
    const NetConfig& cfg, const AutoState& st, const std::set<std::string>& games);

// TCP congestion-control preference per profile, "auto" mode only.
[[nodiscard]] std::vector<std::string_view> tcp_preference(std::string_view profile);

// First of `prefs` that appears in the kernel's space-separated list.
[[nodiscard]] std::optional<std::string> pick_available(
    const std::vector<std::string_view>& prefs, std::string_view available);

// IRQ affinity mask for the big cores, PingPimp's heuristic.
[[nodiscard]] std::string irq_mask_for_cpus(long max_cpu_index);

// Interfaces whose address/link changes mean "the network changed":
// Wi-Fi and the usual mobile-data names across SoC vendors.
[[nodiscard]] bool is_wan_iface(std::string_view name) noexcept;

// ── logcat events ───────────────────────────────────────────────────────────

enum class EventKind { None, Foreground, Screen };

struct NetEvent {
    EventKind kind = EventKind::None;
    std::string pkg;      // Foreground
    bool screen_on = true; // Screen
};

// One `logcat -b events -v brief` line, e.g.
//   I/wm_set_resumed_activity( 1234): [0,com.foo/.Main,reason]
//   I/screen_toggled( 1234): 0
[[nodiscard]] NetEvent parse_event_line(std::string_view line);

// ── package data ────────────────────────────────────────────────────────────

// /data/system/packages.list: "pkg uid debuggable dataPath seinfo gids".
// Returns pkg -> uid for the wanted packages only.
[[nodiscard]] std::map<std::string, unsigned> parse_packages_list(
    std::string_view text, const std::set<std::string>& wanted);

// `cmd package query-activities` output -> packages whose ApplicationInfo
// declares category=0 (CATEGORY_GAME). Best effort: the format is AOSP's
// ResolveInfo dump, not a stable API.
[[nodiscard]] std::set<std::string> parse_declared_games(std::string_view out);

// ── iptables-restore scripts ────────────────────────────────────────────────

inline constexpr std::string_view kQosChain = "YAY_QOS";
inline constexpr std::string_view kJailChain = "YAY_JAIL";

// Complete table section that atomically REPLACES the chain's content (a
// chain declared with ':' is flushed by `iptables-restore --noflush`).
// An empty uid list yields an empty chain — which is how "no apps" clears it.
[[nodiscard]] std::string make_qos_script(const std::vector<unsigned>& uids, bool dscp);
[[nodiscard]] std::string make_jail_script(const std::vector<unsigned>& uids);
