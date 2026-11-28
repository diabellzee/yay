#pragma once

// The stateful half of yay_net: turns a NetConfig + an effective profile into
// writes (sysctl via KnobStore, Settings via `cmd`, Wi-Fi via `iw`, per-app
// rules via TrafficRules) and can undo every one of them.

#include "net_knobs.h"
#include "net_policy.h"
#include "net_rules.h"

#include <map>
#include <string>

class NetEngine {
public:
    // `root` prefixes /proc, /sys and /data paths ("" on a device).
    NetEngine(KnobStore& knobs, Stash& stash, std::string root);

    // Apply everything for this config. `profile` is the EFFECTIVE profile
    // (base or foreground-driven). Idempotent: only differences are written.
    void apply(const NetConfig& cfg, std::string_view profile);

    // After a network change: re-assert what the driver/netd may have reset.
    void reassert(const NetConfig& cfg);

    // Undo every change this engine ever made (knobs, Settings, rules).
    void restore_all();

    [[nodiscard]] Json status(const NetConfig& cfg) const;
    [[nodiscard]] const std::string& applied_profile() const noexcept { return profile_; }

private:
    void apply_profile(const NetConfig& cfg, std::string_view profile);
    void apply_tcp(const NetConfig& cfg, std::string_view profile);
    void apply_toggles(const NetConfig& cfg, std::string_view profile);
    void apply_rules(const NetConfig& cfg);

    void set_ipv6(bool on);
    void set_conntrack(bool on);
    void set_irq(bool on);
    void set_ksoft(bool on);
    void set_wifi_ps(bool off);
    void set_low_latency(bool on);
    void set_netstats(bool off);
    void set_data_saver(bool on);

    [[nodiscard]] std::string wlan_iface() const;
    [[nodiscard]] std::optional<std::string> settings_get(const char* key) const;
    void settings_put(const char* key, const std::string& value) const;
    void settings_del(const char* key) const;

    KnobStore& knobs_;
    Stash& stash_;
    std::string root_;
    TrafficRules rules_;

    std::string profile_;                 // last applied profile name
    std::size_t last_changed_ = 0;        // knobs written by the last profile pass
    std::size_t last_failed_ = 0;
    std::size_t last_missing_ = 0;

    // In-memory memo of what we have already asked of exec-based features, so
    // a repeated apply() costs no process spawns.
    bool ll_applied_ = false;
    bool wps_applied_ = false;
    std::string wps_orig_;                // "on" / "off" before we touched it
    std::string wps_iface_;               // interface we changed
    bool netstats_applied_ = false;
    bool saver_applied_ = false;
    bool ksoft_applied_ = false;
    std::map<int, int> ksoft_orig_;       // pid -> original nice
    bool iw_missing_ = false;
    std::size_t missing_packages_ = 0;
    std::string last_rules_msg_;
};
