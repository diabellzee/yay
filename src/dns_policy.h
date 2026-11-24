#pragma once

// Pure decision logic for yay_dns (the Auto Private DNS daemon).
//
// Deliberately free of any I/O — no sockets, no exec, no filesystem apart
// from parse_dns_config()'s JSON input — so every rule below (which
// interface counts as Wi-Fi/cellular, what is a "usable" address, when a
// Private DNS value is considered a manual user override) can be unit
// tested on a plain host machine. See tests/dns_policy_test.cpp.

#include "json_config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <netinet/in.h>

enum class NetKind { None, Wifi, Cellular };
enum class IfaceClass { Wifi, Cellular, Other };

// One (private_dns_mode, private_dns_specifier) pair, as stored in
// Settings.Global. `mode` is "off" | "opportunistic" | "hostname", or ""
// when the key is unset. `host` is "" when unset.
struct DnsSetting {
    std::string mode;
    std::string host;
};

// Case-insensitive on `host` (DNS names are), exact on `mode`.
[[nodiscard]]
bool same_setting(const DnsSetting& a, const DnsSetting& b) noexcept;

struct AutoDnsConfig {
    bool enabled = false;

    // Personal NextDNS endpoint, e.g. "abc123.dns.nextdns.io". Has no
    // sensible default — it is per-user — so it must be provided.
    std::string wifi_host;
    std::string mobile_host = "1dot1dot1dot1.cloudflare-dns.com";

    // Which network wins when Wi-Fi and a SIM data connection are both up.
    // Default: cellular (config "prefer": "cellular" | "wifi").
    bool prefer_cellular = true;

    // Trailing-edge debounce for bursts of netlink events.
    int debounce_ms = 1500;

    // Station interface(s) only. Exact match, NOT a "wlan*" prefix:
    // softap / Wi-Fi Direct interfaces (ap0, swlan0, wlan1, p2p-*) hold
    // addresses too while the phone is the *hotspot*, which must not be
    // mistaken for "connected to Wi-Fi".
    std::vector<std::string> wifi_ifaces{"wlan0"};

    // Name prefixes of mobile-data interfaces across SoC vendors:
    // Qualcomm (rmnet_data*), MediaTek (ccmni*), Unisoc (seth_*, sipa_*),
    // plus the "v4-" 464XLAT stacked variants.
    std::vector<std::string> cellular_prefixes{
        "rmnet", "v4-rmnet", "ccmni", "v4-ccmni", "ccemni",
        "seth", "v4-seth", "sipa", "pdp", "ppp", "wwan",
    };

    [[nodiscard]] DnsSetting wifi_profile() const;
    [[nodiscard]] DnsSetting mobile_profile() const;
};

constexpr int kMinDebounceMs = 200;
constexpr int kMaxDebounceMs = 30000;

// "abc.dns.nextdns.io" style names only: >= 2 labels, [a-z0-9-] labels of
// 1..63 chars not starting/ending with '-', total <= 253, and a
// non-numeric TLD (so IP literals are rejected — Android's Private DNS
// specifier must be a hostname). Case-insensitive input.
[[nodiscard]]
bool valid_hostname(std::string_view host) noexcept;

// Parse + range-check a (shape-validated) dns_config.json.
// Hosts are lowercased. When `enabled` is true, both hosts must be valid;
// when false they may be empty/invalid (the feature is off, so a
// half-filled config is not an error).
[[nodiscard]]
std::optional<AutoDnsConfig> parse_dns_config(
    const Json& cfg, std::string* error_out = nullptr);

// ── interface / address classification ──────────────────────────────────────

[[nodiscard]]
IfaceClass classify_iface(std::string_view name, const AutoDnsConfig& cfg) noexcept;

// An address that proves the interface can carry traffic: IPv4 other than
// loopback/unspecified/link-local(169.254/16)/multicast; IPv6 global
// unicast only (no ::1, fe80::/10, ff00::/8, or ULA fc00::/7 — a
// ULA-only interface is local-network-only, not "internet connected").
// `addr_be` is in network byte order.
[[nodiscard]]
bool usable_ipv4(std::uint32_t addr_be) noexcept;

[[nodiscard]]
bool usable_ipv6(const in6_addr& addr) noexcept;

// Resolve "which network is the active one" from what is up.
//   both up -> `prefer_cellular` decides, unless `mobile_data_off` (the user
//              switched mobile data off: the cellular interface that is
//              still up is then IMS/VoLTE etc., not internet) -> Wifi.
//   one up  -> that one (mobile_data_off is deliberately ignored here: with
//              only cellular up, we must not stop working just because the
//              mobile_data setting reads oddly, e.g. on dual-SIM).
[[nodiscard]]
NetKind pick_network(
    bool wifi_up,
    bool cellular_up,
    bool prefer_cellular,
    bool mobile_data_off) noexcept;

// ── manual-override state tracking ──────────────────────────────────────────

struct Tracker {
    // False until the daemon has successfully taken ownership once this
    // run. The first evaluation after a (re)start adopts whatever is
    // there — that is what makes "resume after reboot" work.
    bool adopted = false;

    // True while a manual change by the user is being respected.
    bool suspended = false;

    // What the daemon itself last wrote (or confirmed already in place).
    std::optional<DnsSetting> last_applied;
};

enum class Verdict {
    NoNetwork,   // nothing usable up — leave everything alone
    InSync,      // already what we would set
    Apply,       // write `target`
    HoldManual,  // user override detected — do not touch
};

struct Decision {
    Verdict verdict = Verdict::NoNetwork;
    DnsSetting target;           // valid for Apply / InSync / HoldManual
    bool resumed = false;        // leaving suspension (log once)
    bool entered_hold = false;   // entering suspension (log once)
};

// Core rule. `current` is what Settings.Global holds right now.
//
//   current == desired                     -> InSync (clears suspension)
//   first run since (re)start              -> Apply (adopt, whatever it was)
//   current is one of OUR values           -> Apply (module-owned, just
//     (wifi profile / mobile profile /        the other network's profile
//      last_applied)                          or a stale one)
//   anything else (off, opportunistic,     -> HoldManual (user's choice)
//     some other host)
//
// Mutates `tr` only for suspension/ownership bookkeeping that does not
// depend on a write succeeding; call note_applied() after a verified write.
[[nodiscard]]
Decision decide(
    Tracker& tr,
    NetKind net,
    const DnsSetting& current,
    const AutoDnsConfig& cfg);

void note_applied(Tracker& tr, const DnsSetting& applied);
