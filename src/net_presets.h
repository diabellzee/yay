#pragma once

// Sysctl presets for yay_net (see net_presets.cpp for provenance).

#include <cstddef>
#include <span>
#include <string_view>

struct Knob {
    std::string_view key;   // relative to /proc/sys/net/, e.g. "core/rmem_max"
    std::string_view value;
};

struct Preset {
    std::string_view name;
    const Knob* knobs;
    std::size_t count;
    bool low_latency;       // also force Wi-Fi low-latency mode while active
};

// nullptr if unknown. "browsing" is an alias of "social".
[[nodiscard]] const Preset* find_preset(std::string_view name) noexcept;
[[nodiscard]] std::span<const Preset> all_presets() noexcept;
