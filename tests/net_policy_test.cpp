// Host-side unit tests for src/net_policy.cpp and src/net_presets.cpp.
//
//   g++ -std=c++20 -I src -I <nlohmann include dir> tests/net_policy_test.cpp src/net_policy.cpp src/net_presets.cpp -o /tmp/net_policy_test
//   /tmp/net_policy_test

#include "net_policy.h"
#include "net_presets.h"

#include <cstdio>

namespace {
int g_failed = 0, g_total = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_total;                                                               \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

void test_validation()
{
    CHECK(valid_package("com.mobile.legends"));
    CHECK(valid_package("a_b.c1"));
    CHECK(!valid_package(""));
    CHECK(!valid_package(".hidden"));
    CHECK(!valid_package("com.x; reboot"));
    CHECK(!valid_package("com.x y"));
    CHECK(!valid_package("../etc"));
    for (const char* p : {"off", "balanced", "game", "download", "streaming", "social", "browsing", "outdoor"})
        CHECK(valid_profile(p));
    CHECK(!valid_profile("turbo"));
    CHECK(!valid_profile(""));
}

void test_parse_config()
{
    std::string err;
    auto d = parse_net_config(Json{{"enabled", false}}, &err);
    CHECK(d && d->profile == "balanced" && !d->auto_mode && d->auto_detect_games);
    CHECK(d && d->confirm_ms == 4000 && d->debounce_ms == 1000 && d->tcp_algo.empty());

    Json full = {{"enabled", true}, {"profile", "GAME"}, {"auto", true}, {"tcp_algo", "BBR"},
                 {"boost_apps", {"a.b", "a.b", "c.d"}}, {"jail_apps", {"x.y"}},
                 {"app_profiles", {{"p.q", "Download"}}},
                 {"toggles", {{"ipv6_off", true}, {"conntrack", true}}},
                 {"confirm_ms", 1}, {"debounce_ms", 99999999}};
    auto c = parse_net_config(full, &err);
    CHECK(c.has_value());
    CHECK(c && c->profile == "game" && c->auto_mode && c->tcp_algo == "bbr");
    CHECK(c && c->boost_apps.size() == 2U);                    // de-duplicated, order kept
    CHECK(c && c->app_profiles.at("p.q") == "download");
    CHECK(c && c->toggles.ipv6_off && c->toggles.conntrack && !c->toggles.data_saver);
    CHECK(c && c->confirm_ms == kMinConfirmMs && c->debounce_ms == kMaxNetDebounceMs);   // clamped

    CHECK(!parse_net_config(Json{{"profile", "turbo"}}, &err).has_value());
    CHECK(!parse_net_config(Json{{"tcp_algo", "bbr; reboot"}}, &err).has_value());
    CHECK(!parse_net_config(Json{{"jail_apps", {"ok.app", "bad pkg"}}}, &err).has_value());
    CHECK(err.find("jail_apps") != std::string::npos);
    CHECK(!parse_net_config(Json{{"app_profiles", {{"p.q", "turbo"}}}}, &err).has_value());
    auto autoalgo = parse_net_config(Json{{"tcp_algo", "auto"}}, &err);
    CHECK(autoalgo && autoalgo->tcp_algo == "auto");
}

void test_profiles()
{
    NetConfig cfg;
    cfg.profile = "balanced";
    cfg.auto_mode = true;

    CHECK(classify_app("com.google.android.youtube") == "streaming");
    CHECK(classify_app("com.spotify.music") == "streaming");
    CHECK(classify_app("com.android.chrome") == "browsing");
    CHECK(classify_app("org.telegram.messenger") == "social");  // "tele-gram"
    CHECK(classify_app("com.instagram.android") == "social");
    CHECK(classify_app("com.whatsapp") == "social");
    CHECK(classify_app("com.example.calculator") == "");

    CHECK(profile_for_app(cfg, "com.whatsapp", false) == "social");
    CHECK(profile_for_app(cfg, "com.whatsapp", true) == "game");           // game beats category
    CHECK(profile_for_app(cfg, "com.example.calc", false) == "balanced");  // unknown -> base
    cfg.app_profiles["com.whatsapp"] = "download";
    CHECK(profile_for_app(cfg, "com.whatsapp", true) == "download");       // explicit beats everything

    const std::set<std::string> games{"com.mobile.legends"};
    AutoState st;
    st.committed_pkg = "com.mobile.legends";
    CHECK(effective_profile(cfg, st, games) == "game");
    st.screen_on = false;
    CHECK(effective_profile(cfg, st, games) == "balanced");                // screen off -> base
    st.screen_on = true;
    cfg.auto_mode = false;
    CHECK(effective_profile(cfg, st, games) == "balanced");                // auto off -> base
    cfg.auto_mode = true;
    cfg.auto_detect_games = false;
    CHECK(effective_profile(cfg, st, games) == "balanced");                // game detection off
    cfg.auto_detect_games = true;
    st.committed_pkg.clear();
    CHECK(effective_profile(cfg, st, games) == "balanced");                // nothing known yet
}

void test_tcp_and_irq()
{
    CHECK(tcp_preference("game").front() == "westwood");
    CHECK(tcp_preference("download").front() == "bbr");
    CHECK(tcp_preference("social").front() == "cubic");
    CHECK(pick_available(tcp_preference("game"), "reno bbr cubic") == "bbr");   // westwood missing
    CHECK(pick_available(tcp_preference("social"), "reno bbr cubic") == "cubic");
    CHECK(!pick_available(tcp_preference("game"), "vegas").has_value());
    CHECK(pick_available({"bbr"}, "  reno\tbbr \n") == "bbr");                  // tabs/newlines
    CHECK(!pick_available({"bbr"}, "bbr2 reno").has_value());                   // exact token only

    CHECK(irq_mask_for_cpus(7) == "c0");
    CHECK(irq_mask_for_cpus(3) == "c");
    CHECK(irq_mask_for_cpus(1) == "3");
    CHECK(is_wan_iface("wlan0") && is_wan_iface("rmnet_data0") && is_wan_iface("ccmni1"));
    CHECK(is_wan_iface("v4-rmnet_data0") && is_wan_iface("seth_lte0"));
    CHECK(!is_wan_iface("p2p0") && !is_wan_iface("tun0") && !is_wan_iface("lo") && !is_wan_iface("dummy0"));
}

void test_events()
{
    auto fg = parse_event_line("I/wm_set_resumed_activity( 1234): [0,com.mobile.legends/.MainActivity,resumeTopActivityInnerLocked]");
    CHECK(fg.kind == EventKind::Foreground && fg.pkg == "com.mobile.legends");

    auto fg2 = parse_event_line("I/wm_set_resumed_activity(  780): [0,com.miui.home/com.miui.home.launcher.Launcher,minimalResumeActivityLocked]");
    CHECK(fg2.kind == EventKind::Foreground && fg2.pkg == "com.miui.home");

    auto old = parse_event_line("I/am_focused_activity(  900): [0,com.android.settings/.Settings,reason]");
    CHECK(old.kind == EventKind::Foreground && old.pkg == "com.android.settings");

    auto on = parse_event_line("I/screen_toggled(  900): 1");
    CHECK(on.kind == EventKind::Screen && on.screen_on);
    auto off = parse_event_line("I/screen_toggled(  900): 0");
    CHECK(off.kind == EventKind::Screen && !off.screen_on);

    CHECK(parse_event_line("").kind == EventKind::None);
    CHECK(parse_event_line("--------- beginning of events").kind == EventKind::None);
    CHECK(parse_event_line("I/am_proc_start( 1): [0,123,10001,com.x,service,com.x/.S]").kind == EventKind::None);
    CHECK(parse_event_line("I/wm_set_resumed_activity( 1): [0,bad pkg!/x,r]").kind == EventKind::None);
    CHECK(parse_event_line("I/wm_set_resumed_activity( 1): garbage").kind == EventKind::None);
}

void test_packages()
{
    const std::string list =
        "com.mobile.legends 10234 0 /data/user/0/com.mobile.legends default:targetSdkVersion=31 3002,3003\n"
        "com.android.chrome 10101 0 /data/user/0/com.android.chrome default:targetSdkVersion=32 none\n"
        "com.other.app 10999 1 /data/user/0/com.other.app default none\n"
        "garbage line\n";
    auto m = parse_packages_list(list, {"com.mobile.legends", "com.android.chrome", "not.installed"});
    CHECK(m.size() == 2U);
    CHECK(m.at("com.mobile.legends") == 10234U && m.at("com.android.chrome") == 10101U);
    CHECK(m.count("com.other.app") == 0U);                      // not requested
    CHECK(parse_packages_list("", {"a.b"}).empty());
    CHECK(parse_packages_list("a.b notanumber 0\n", {"a.b"}).empty());
}

void test_declared_games()
{
    const std::string out =
        "  ActivityInfo:\n    name=com.game.Main\n    packageName=com.game.one\n"
        "    ApplicationInfo:\n      category=0\n"
        "  ActivityInfo:\n    packageName=com.tool.two\n    ApplicationInfo:\n      category=-1\n"
        "  ActivityInfo:\n    packageName=com.game.three\n      category=0 foo\n"
        "  ActivityInfo:\n    packageName=com.maps.four\n      category=5\n"
        "      category=05\n";
    auto g = parse_declared_games(out);
    CHECK(g.count("com.game.one") == 1U && g.count("com.game.three") == 1U);
    CHECK(g.count("com.tool.two") == 0U && g.count("com.maps.four") == 0U);
    CHECK(g.size() == 2U);
    CHECK(parse_declared_games("").empty());
}

void test_scripts()
{
    const std::string q = make_qos_script({10200, 10100, 10100}, true);
    CHECK(q == "*mangle\n:YAY_QOS - [0:0]\n"
               "-A YAY_QOS -m owner --uid-owner 10100 -j DSCP --set-dscp 46\n"
               "-A YAY_QOS -m owner --uid-owner 10200 -j DSCP --set-dscp 46\nCOMMIT\n");
    CHECK(make_qos_script({10100}, false).find("MARK --set-mark 0x40000000/0x40000000") != std::string::npos);
    CHECK(make_qos_script({}, true) == "*mangle\n:YAY_QOS - [0:0]\nCOMMIT\n");         // empty list == clears the chain
    const std::string j = make_jail_script({10456});
    CHECK(j == "*filter\n:YAY_JAIL - [0:0]\n-A YAY_JAIL -m owner --uid-owner 10456 -j REJECT\nCOMMIT\n");
    CHECK(make_jail_script({}) == "*filter\n:YAY_JAIL - [0:0]\nCOMMIT\n");
}

void test_presets()
{
    CHECK(all_presets().size() == 6U);
    for (const Preset& p : all_presets()) {
        CHECK(p.count == 42U);
        for (std::size_t i = 0; i < p.count; ++i) CHECK(!p.knobs[i].key.empty() && !p.knobs[i].value.empty());
    }
    CHECK(find_preset("browsing") == find_preset("social"));
    CHECK(find_preset("game") != nullptr && find_preset("game")->low_latency);
    CHECK(find_preset("balanced") != nullptr && !find_preset("balanced")->low_latency);
    CHECK(find_preset("nope") == nullptr);
    // no preset may force tcp_abort_on_overflow (deliberate deviation from PingPimp)
    for (const Preset& p : all_presets())
        for (std::size_t i = 0; i < p.count; ++i)
            if (p.knobs[i].key == "ipv4/tcp_abort_on_overflow") CHECK(p.knobs[i].value == "0");
    // every preset covers the same keys in the same order (switching never leaves a stale key)
    const Preset* base = find_preset("balanced");
    for (const Preset& p : all_presets())
        for (std::size_t i = 0; i < p.count; ++i) CHECK(p.knobs[i].key == base->knobs[i].key);
}
} // namespace

int main()
{
    test_validation(); test_parse_config(); test_profiles(); test_tcp_and_irq();
    test_events(); test_packages(); test_declared_games(); test_scripts(); test_presets();
    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
