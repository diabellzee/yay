#include "dns_policy.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>

namespace {

constexpr const char* kModeHostname = "hostname";
constexpr size_t kMaxIfaceNameLen = 15U; // IFNAMSIZ - 1

[[nodiscard]]
char lower(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]]
std::string to_lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out) {
        c = lower(c);
    }
    return out;
}

[[nodiscard]]
std::string trim(std::string_view s)
{
    const auto is_space = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1U);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1U);
    }
    return std::string(s);
}

[[nodiscard]]
bool ieq(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool valid_iface_token(std::string_view s) noexcept
{
    if (s.empty() || s.size() > kMaxIfaceNameLen) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.';
    });
}

// Replace `out` with the list at cfg[key] if present and non-empty;
// absent/empty keeps the built-in default (an empty list would make the
// feature silently never match anything).
[[nodiscard]]
bool read_token_list(
    const Json& cfg,
    const char* key,
    std::vector<std::string>& out,
    std::string* error_out)
{
    if (!cfg.contains(key) || cfg[key].empty()) {
        return true;
    }

    std::vector<std::string> parsed;
    for (const auto& item : cfg[key]) {
        const std::string tok = trim(item.get<std::string>());
        if (!valid_iface_token(tok)) {
            if (error_out) {
                *error_out = std::string(key) + ": invalid interface name \"" + tok + "\"";
            }
            return false;
        }
        parsed.push_back(tok);
    }
    out = std::move(parsed);
    return true;
}

} // namespace

bool same_setting(const DnsSetting& a, const DnsSetting& b) noexcept
{
    return a.mode == b.mode && ieq(a.host, b.host);
}

DnsSetting AutoDnsConfig::wifi_profile() const
{
    return DnsSetting{kModeHostname, wifi_host};
}

DnsSetting AutoDnsConfig::mobile_profile() const
{
    return DnsSetting{kModeHostname, mobile_host};
}

bool valid_hostname(std::string_view host) noexcept
{
    if (host.empty() || host.size() > 253U) {
        return false;
    }

    size_t labels = 0;
    size_t start = 0;
    std::string_view last_label;

    while (start <= host.size()) {
        const size_t dot = host.find('.', start);
        const size_t end = (dot == std::string_view::npos) ? host.size() : dot;
        const std::string_view label = host.substr(start, end - start);

        if (label.empty() || label.size() > 63U) {
            return false;
        }
        if (label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (const char c : label) {
            const auto u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) != 0 || c == '-')) {
                return false;
            }
        }

        ++labels;
        last_label = label;

        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1U;
    }

    if (labels < 2U) {
        return false;
    }

    // Reject IP literals like "1.1.1.1": the TLD must contain a letter.
    return std::any_of(last_label.begin(), last_label.end(), [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    });
}

std::optional<AutoDnsConfig> parse_dns_config(const Json& cfg, std::string* error_out)
{
    const auto fail = [&](std::string msg) -> std::optional<AutoDnsConfig> {
        if (error_out) {
            *error_out = std::move(msg);
        }
        return std::nullopt;
    };

    try {
        AutoDnsConfig out;
        out.enabled = cfg.value("enabled", false);
        out.wifi_host = to_lower(trim(cfg.value("wifi_host", std::string{})));
        out.mobile_host = to_lower(trim(cfg.value("mobile_host", out.mobile_host)));

        const std::string prefer = to_lower(trim(cfg.value("prefer", std::string("cellular"))));
        if (prefer != "cellular" && prefer != "wifi") {
            return fail("prefer must be \"cellular\" or \"wifi\", got \"" + prefer + "\"");
        }
        out.prefer_cellular = (prefer == "cellular");

        const auto debounce = cfg.value("debounce_ms", out.debounce_ms);
        out.debounce_ms = std::clamp(debounce, kMinDebounceMs, kMaxDebounceMs);

        std::string err;
        if (!read_token_list(cfg, "wifi_ifaces", out.wifi_ifaces, &err) ||
            !read_token_list(cfg, "cellular_prefixes", out.cellular_prefixes, &err)) {
            return fail(err);
        }

        if (out.enabled) {
            if (out.wifi_host.empty()) {
                return fail("wifi_host is not set (your NextDNS endpoint, "
                            "e.g. abc123.dns.nextdns.io)");
            }
            if (!valid_hostname(out.wifi_host)) {
                return fail("wifi_host is not a valid hostname: " + out.wifi_host);
            }
            if (!valid_hostname(out.mobile_host)) {
                return fail("mobile_host is not a valid hostname: " + out.mobile_host);
            }
        }

        return out;
    } catch (const Json::exception& e) {
        return fail(std::string("dns_config: ") + e.what());
    } catch (...) {
        return fail("dns_config: unknown error");
    }
}

IfaceClass classify_iface(std::string_view name, const AutoDnsConfig& cfg) noexcept
{
    for (const auto& w : cfg.wifi_ifaces) {
        if (name == w) {
            return IfaceClass::Wifi;
        }
    }
    for (const auto& p : cfg.cellular_prefixes) {
        if (!p.empty() && name.substr(0, p.size()) == p) {
            return IfaceClass::Cellular;
        }
    }
    return IfaceClass::Other;
}

bool usable_ipv4(std::uint32_t addr_be) noexcept
{
    const std::uint32_t a = ntohl(addr_be);

    if (a == 0U) {
        return false;                        // 0.0.0.0
    }
    if ((a >> 24U) == 127U) {
        return false;                        // loopback
    }
    if ((a >> 16U) == ((169U << 8U) | 254U)) {
        return false;                        // link-local
    }
    if ((a >> 28U) >= 14U) {
        return false;                        // multicast 224/4, reserved 240/4
    }
    return true;                             // incl. 100.64/10 CGNAT — common on mobile
}

bool usable_ipv6(const in6_addr& addr) noexcept
{
    const auto* b = addr.s6_addr;

    const bool all_zero_prefix = std::all_of(b, b + 15, [](std::uint8_t v) { return v == 0U; });
    if (all_zero_prefix && (b[15] == 0U || b[15] == 1U)) {
        return false;                        // :: and ::1
    }
    if (b[0] == 0xfeU && (b[1] & 0xc0U) == 0x80U) {
        return false;                        // fe80::/10 link-local
    }
    if (b[0] == 0xffU) {
        return false;                        // multicast
    }
    if ((b[0] & 0xfeU) == 0xfcU) {
        return false;                        // fc00::/7 ULA
    }
    return true;
}

NetKind pick_network(
    bool wifi_up,
    bool cellular_up,
    bool prefer_cellular,
    bool mobile_data_off) noexcept
{
    if (wifi_up && cellular_up) {
        return (prefer_cellular && !mobile_data_off) ? NetKind::Cellular : NetKind::Wifi;
    }
    if (cellular_up) {
        return NetKind::Cellular;
    }
    if (wifi_up) {
        return NetKind::Wifi;
    }
    return NetKind::None;
}

Decision decide(
    Tracker& tr,
    NetKind net,
    const DnsSetting& current,
    const AutoDnsConfig& cfg)
{
    Decision d;

    if (net == NetKind::None) {
        d.verdict = Verdict::NoNetwork;
        return d;
    }

    const DnsSetting wifi = cfg.wifi_profile();
    const DnsSetting mobile = cfg.mobile_profile();
    const DnsSetting& desired = (net == NetKind::Wifi) ? wifi : mobile;
    d.target = desired;

    if (same_setting(current, desired)) {
        d.verdict = Verdict::InSync;
        d.resumed = tr.suspended;
        tr.suspended = false;
        tr.adopted = true;
        tr.last_applied = desired;
        return d;
    }

    const bool ours =
        same_setting(current, wifi) ||
        same_setting(current, mobile) ||
        (tr.last_applied.has_value() && same_setting(current, *tr.last_applied));

    if (!tr.adopted || ours) {
        d.verdict = Verdict::Apply;
        d.resumed = tr.suspended;
        return d;
    }

    d.verdict = Verdict::HoldManual;
    d.entered_hold = !tr.suspended;
    tr.suspended = true;
    return d;
}

void note_applied(Tracker& tr, const DnsSetting& applied)
{
    tr.adopted = true;
    tr.suspended = false;
    tr.last_applied = applied;
}
