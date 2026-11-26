// Host-side unit tests for src/dns_policy.cpp (pure logic, no Android needed).
//
//   g++ -std=c++20 -I src -I <dir containing nlohmann/json.hpp>
//       tests/dns_policy_test.cpp src/dns_policy.cpp -o /tmp/dns_policy_test
//   /tmp/dns_policy_test
//
// Not wired into CMake on purpose: the CMake build is an Android
// cross-compile, and these run natively.

#include "dns_policy.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failed = 0;
int g_total = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_total;                                                               \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

std::uint32_t v4(const char* s)
{
    in_addr a{};
    inet_pton(AF_INET, s, &a);
    return a.s_addr;
}

in6_addr v6(const char* s)
{
    in6_addr a{};
    inet_pton(AF_INET6, s, &a);
    return a;
}

AutoDnsConfig make_cfg()
{
    AutoDnsConfig c;
    c.enabled = true;
    c.wifi_host = "abc123.dns.nextdns.io";
    c.mobile_host = "1dot1dot1dot1.cloudflare-dns.com";
    return c;
}

const DnsSetting kOff{"off", ""};
const DnsSetting kAuto{"opportunistic", ""};
const DnsSetting kCustom{"hostname", "dns.example.org"};

void test_hostname()
{
    CHECK(valid_hostname("abc123.dns.nextdns.io"));
    CHECK(valid_hostname("1dot1dot1dot1.cloudflare-dns.com"));
    CHECK(valid_hostname("My-Phone--abc123.dns.nextdns.io"));
    CHECK(valid_hostname("dns.google"));

    CHECK(!valid_hostname(""));
    CHECK(!valid_hostname("localhost"));          // single label
    CHECK(!valid_hostname("1.1.1.1"));            // IP literal
    CHECK(!valid_hostname("2606.4700.4700.1111")); // numeric TLD
    CHECK(!valid_hostname("a..b.com"));           // empty label
    CHECK(!valid_hostname(".a.com"));
    CHECK(!valid_hostname("a.com."));             // trailing dot
    CHECK(!valid_hostname("-a.com"));
    CHECK(!valid_hostname("a-.com"));
    CHECK(!valid_hostname("a b.com"));
    CHECK(!valid_hostname("a.com; rm -rf /"));
    CHECK(!valid_hostname("a_b.com"));
    CHECK(!valid_hostname(std::string(64, 'a') + ".com")); // label > 63
    CHECK(!valid_hostname(std::string(250, 'a') + ".com") ); // > 253 total (and label)
}

void test_parse_config()
{
    std::string err;

    // enabled but no wifi host -> clear error
    {
        Json j = {{"enabled", true}, {"wifi_host", ""}};
        CHECK(!parse_dns_config(j, &err).has_value());
        CHECK(err.find("wifi_host") != std::string::npos);
    }
    // disabled with empty host is fine (half-filled config while off)
    {
        Json j = {{"enabled", false}, {"wifi_host", ""}};
        CHECK(parse_dns_config(j, &err).has_value());
    }
    // lower-cased + trimmed
    {
        Json j = {{"enabled", true}, {"wifi_host", "  ABC123.DNS.NextDNS.io "}};
        auto c = parse_dns_config(j, &err);
        CHECK(c.has_value());
        CHECK(c && c->wifi_host == "abc123.dns.nextdns.io");
        CHECK(c && c->mobile_host == "1dot1dot1dot1.cloudflare-dns.com"); // default
    }
    // debounce clamped
    {
        Json lo = {{"enabled", false}, {"debounce_ms", 1}};
        Json hi = {{"enabled", false}, {"debounce_ms", 10000000}};
        auto a = parse_dns_config(lo, &err);
        auto b = parse_dns_config(hi, &err);
        CHECK(a && a->debounce_ms == kMinDebounceMs);
        CHECK(b && b->debounce_ms == kMaxDebounceMs);
    }
    // bad iface token rejected; empty list keeps defaults
    {
        Json bad = {{"enabled", false}, {"wifi_ifaces", {"wlan0; reboot"}}};
        CHECK(!parse_dns_config(bad, &err).has_value());
        Json empty = {{"enabled", false}, {"wifi_ifaces", Json::array()}};
        auto c = parse_dns_config(empty, &err);
        CHECK(c && c->wifi_ifaces.size() == 1U && c->wifi_ifaces[0] == "wlan0");
    }
    // injection attempt in host
    {
        Json j = {{"enabled", true},
                  {"wifi_host", "a.nextdns.io"},
                  {"mobile_host", "x.com;reboot"}};
        CHECK(!parse_dns_config(j, &err).has_value());
    }
}

void test_classify()
{
    const AutoDnsConfig c = make_cfg();
    CHECK(classify_iface("wlan0", c) == IfaceClass::Wifi);

    // Hotspot / P2P / clat-on-wifi must NOT count as "connected to Wi-Fi".
    CHECK(classify_iface("wlan1", c) == IfaceClass::Other);
    CHECK(classify_iface("ap0", c) == IfaceClass::Other);
    CHECK(classify_iface("swlan0", c) == IfaceClass::Other);
    CHECK(classify_iface("p2p-wlan0-0", c) == IfaceClass::Other);
    CHECK(classify_iface("v4-wlan0", c) == IfaceClass::Other);

    // Mobile: Qualcomm / MediaTek / Unisoc / clat variants.
    CHECK(classify_iface("rmnet_data0", c) == IfaceClass::Cellular);
    CHECK(classify_iface("rmnet_data7", c) == IfaceClass::Cellular);
    CHECK(classify_iface("v4-rmnet_data0", c) == IfaceClass::Cellular);
    CHECK(classify_iface("ccmni1", c) == IfaceClass::Cellular);
    CHECK(classify_iface("seth_lte0", c) == IfaceClass::Cellular);
    CHECK(classify_iface("sipa_eth0", c) == IfaceClass::Cellular);

    // Never classified: VPN, loopback, usb tether, dummies.
    CHECK(classify_iface("tun0", c) == IfaceClass::Other);
    CHECK(classify_iface("lo", c) == IfaceClass::Other);
    CHECK(classify_iface("rndis0", c) == IfaceClass::Other);
    CHECK(classify_iface("dummy0", c) == IfaceClass::Other);
    CHECK(classify_iface("", c) == IfaceClass::Other);
}

void test_addresses()
{
    CHECK(usable_ipv4(v4("192.168.1.23")));
    CHECK(usable_ipv4(v4("10.20.30.40")));
    CHECK(usable_ipv4(v4("100.72.1.5")));      // CGNAT, common on mobile
    CHECK(usable_ipv4(v4("192.0.0.4")));       // 464XLAT clat
    CHECK(!usable_ipv4(v4("0.0.0.0")));
    CHECK(!usable_ipv4(v4("127.0.0.1")));
    CHECK(!usable_ipv4(v4("169.254.10.10")));  // link-local
    CHECK(!usable_ipv4(v4("224.0.0.1")));
    CHECK(!usable_ipv4(v4("255.255.255.255")));

    CHECK(usable_ipv6(v6("2404:c0:1234::1")));
    CHECK(usable_ipv6(v6("2001:4860:4860::8888")));
    CHECK(!usable_ipv6(v6("::")));
    CHECK(!usable_ipv6(v6("::1")));
    CHECK(!usable_ipv6(v6("fe80::1234:5678")));
    CHECK(!usable_ipv6(v6("ff02::1")));
    CHECK(!usable_ipv6(v6("fd12:3456::1")));   // ULA
}

void test_pick_network()
{
    // one network up
    CHECK(pick_network(true, false, true, false) == NetKind::Wifi);
    CHECK(pick_network(false, true, true, false) == NetKind::Cellular);
    CHECK(pick_network(false, false, true, false) == NetKind::None);

    // both up: SIM wins by default
    CHECK(pick_network(true, true, true, false) == NetKind::Cellular);
    // ...unless configured to prefer wifi
    CHECK(pick_network(true, true, false, false) == NetKind::Wifi);
    // mobile data switched off: the leftover cellular iface is IMS, not data
    CHECK(pick_network(true, true, true, true) == NetKind::Wifi);
    // but with ONLY cellular up, a weird mobile_data reading must not blind us
    CHECK(pick_network(false, true, true, true) == NetKind::Cellular);

    std::string err;
    auto def = parse_dns_config(Json{{"enabled", false}}, &err);
    CHECK(def && def->prefer_cellular);
    auto w = parse_dns_config(Json{{"enabled", false}, {"prefer", "WiFi"}}, &err);
    CHECK(w && !w->prefer_cellular);
    CHECK(!parse_dns_config(Json{{"enabled", false}, {"prefer", "both"}}, &err).has_value());
}

void test_debounce_keys()
{
    std::string err;
    auto d = parse_dns_config(Json{{"enabled", false}}, &err);
    CHECK(d && d->debounce_ms == 1500 && d->cellular_debounce_ms == 300);
    auto lo = parse_dns_config(Json{{"enabled", false}, {"cellular_debounce_ms", 1}}, &err);
    CHECK(lo && lo->cellular_debounce_ms == kMinFastDebounceMs);
    auto hi = parse_dns_config(Json{{"enabled", false}, {"cellular_debounce_ms", 99999999}}, &err);
    CHECK(hi && hi->cellular_debounce_ms == kMaxDebounceMs);
}

void test_decide_basic()
{
    const AutoDnsConfig c = make_cfg();
    const DnsSetting wifi = c.wifi_profile();
    const DnsSetting mobile = c.mobile_profile();

    // no network -> never touches anything
    {
        Tracker t;
        auto d = decide(t, NetKind::None, kOff, c);
        CHECK(d.verdict == Verdict::NoNetwork);
        CHECK(!t.adopted);
    }
    // first run after (re)start adopts whatever was there (reboot resume)
    {
        Tracker t;
        auto d = decide(t, NetKind::Wifi, kOff, c);
        CHECK(d.verdict == Verdict::Apply);
        CHECK(same_setting(d.target, wifi));
        CHECK(!t.adopted);          // only note_applied() adopts
        note_applied(t, d.target);
        CHECK(t.adopted && !t.suspended);
    }
    // already correct -> InSync, no write
    {
        Tracker t;
        auto d = decide(t, NetKind::Cellular, mobile, c);
        CHECK(d.verdict == Verdict::InSync);
        CHECK(t.adopted);
    }
    // case-insensitive host compare
    {
        Tracker t;
        DnsSetting shout{"hostname", "ABC123.DNS.NEXTDNS.IO"};
        CHECK(decide(t, NetKind::Wifi, shout, c).verdict == Verdict::InSync);
    }
}

void test_handover_flow()
{
    const AutoDnsConfig c = make_cfg();
    Tracker t;

    // boot on wifi, Private DNS was "Automatic"
    auto d = decide(t, NetKind::Wifi, kAuto, c);
    CHECK(d.verdict == Verdict::Apply);
    note_applied(t, d.target);
    DnsSetting cur = d.target;

    // wifi -> mobile: current is our wifi profile -> switch to cloudflare
    d = decide(t, NetKind::Cellular, cur, c);
    CHECK(d.verdict == Verdict::Apply);
    CHECK(same_setting(d.target, c.mobile_profile()));
    note_applied(t, d.target);
    cur = d.target;

    // airplane mode: nothing usable -> untouched, state kept
    d = decide(t, NetKind::None, cur, c);
    CHECK(d.verdict == Verdict::NoNetwork);

    // back to wifi
    d = decide(t, NetKind::Wifi, cur, c);
    CHECK(d.verdict == Verdict::Apply);
    CHECK(same_setting(d.target, c.wifi_profile()));
    note_applied(t, d.target);
    cur = d.target;

    // repeated event with no change -> InSync (idempotent, no write)
    d = decide(t, NetKind::Wifi, cur, c);
    CHECK(d.verdict == Verdict::InSync);
    CHECK(!d.resumed && !d.entered_hold);
}

void test_manual_override()
{
    const AutoDnsConfig c = make_cfg();
    Tracker t;
    DnsSetting cur = kAuto;

    auto d = decide(t, NetKind::Wifi, cur, c);
    note_applied(t, d.target);
    cur = d.target;

    // user switches Private DNS off while on wifi
    d = decide(t, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::HoldManual);
    CHECK(d.entered_hold);
    CHECK(t.suspended);

    // network flaps to mobile and back: still respected, no re-log
    d = decide(t, NetKind::Cellular, kOff, c);
    CHECK(d.verdict == Verdict::HoldManual);
    CHECK(!d.entered_hold);
    d = decide(t, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::HoldManual);
    CHECK(!d.entered_hold);

    // custom host instead of off: same
    d = decide(t, NetKind::Cellular, kCustom, c);
    CHECK(d.verdict == Verdict::HoldManual);

    // "Automatic" is also a manual choice once we own the setting
    d = decide(t, NetKind::Cellular, kAuto, c);
    CHECK(d.verdict == Verdict::HoldManual);

    // user puts it back on the profile for this network -> resumes
    d = decide(t, NetKind::Wifi, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::InSync);
    CHECK(d.resumed);
    CHECK(!t.suspended);

    // ...and the next handover is automated again
    d = decide(t, NetKind::Cellular, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::Apply);
}

void test_resume_via_other_profile()
{
    const AutoDnsConfig c = make_cfg();
    Tracker t;
    auto d = decide(t, NetKind::Wifi, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::InSync);

    // manual "off", then user re-enters one of the module's profiles on a
    // network where it is not the desired one -> module owns it again.
    d = decide(t, NetKind::Cellular, kOff, c);
    CHECK(d.verdict == Verdict::HoldManual);
    d = decide(t, NetKind::Cellular, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::Apply);
    CHECK(d.resumed);
    note_applied(t, d.target);
    CHECK(!t.suspended);
}

void test_reboot_resets()
{
    const AutoDnsConfig c = make_cfg();
    Tracker before;
    auto d = decide(before, NetKind::Wifi, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::InSync);
    d = decide(before, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::HoldManual);

    // "reboot" == brand-new Tracker: adopts even though user's "off" is
    // still in Settings.
    Tracker after;
    d = decide(after, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::Apply);
}

void test_failed_adoption_retries()
{
    const AutoDnsConfig c = make_cfg();
    Tracker t;

    // first adoption's write fails -> note_applied NOT called.
    auto d = decide(t, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::Apply);

    // next event must try again, not mistake "off" for a manual override.
    d = decide(t, NetKind::Wifi, kOff, c);
    CHECK(d.verdict == Verdict::Apply);
    CHECK(!t.suspended);
}

void test_config_hot_reload()
{
    AutoDnsConfig c = make_cfg();
    Tracker t;
    auto d = decide(t, NetKind::Wifi, c.wifi_profile(), c);
    CHECK(d.verdict == Verdict::InSync);   // last_applied = old wifi profile

    // user edits wifi_host in the WebUI; daemon reloads. The setting now
    // holds the OLD host, which is no longer a profile — but it is
    // last_applied, so it is ours, not a manual override.
    c.wifi_host = "new456.dns.nextdns.io";
    d = decide(t, NetKind::Wifi, DnsSetting{"hostname", "abc123.dns.nextdns.io"}, c);
    CHECK(d.verdict == Verdict::Apply);
    CHECK(same_setting(d.target, c.wifi_profile()));
}

} // namespace

int main()
{
    test_hostname();
    test_parse_config();
    test_classify();
    test_addresses();
    test_pick_network();
    test_debounce_keys();
    test_decide_basic();
    test_handover_flow();
    test_manual_override();
    test_resume_via_other_profile();
    test_reboot_resets();
    test_failed_adoption_retries();
    test_config_hot_reload();

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
