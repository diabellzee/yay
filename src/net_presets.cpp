// Network presets: sysctl value sets for yay_net.
//
// DERIVED from PingPimp (https://github.com/fuckyoustan/PingPimp,
// Apache License 2.0, (c) fuckyoustan) — the OP_DEFAULT / OP_GAME /
// OP_DOWNLOAD / OP_STREAMING / OP_SOCIAL / OP_OUTDOOR tables in
// src/system/bin/PingPimp.c. See NOTICE and LICENSES/.
//
// Changes from the original (Apache-2.0 §4(b)):
//   * Re-expressed as data (one key list, one value row per preset) instead
//     of six 44-entry op tables.
//   * "default" is named "balanced": it is PingPimp's own baseline, NOT the
//     device's original values. The real originals are snapshotted by
//     KnobStore and restored by the "off" profile.
//   * ipv4/route/flush dropped (kernels >= 3.6 have no routing cache; the
//     write is a no-op).
//   * ipv4/tcp_abort_on_overflow is 0 in every preset (the original "game"
//     preset set it to 1, which resets connections when an accept queue
//     overflows — meaningless for a phone acting as a client).
//   * core/default_qdisc is not touched (only affects newly created qdiscs).
//
// Values are PingPimp's; they have not been independently benchmarked.

#include "net_presets.h"

namespace {
constexpr Knob k_balanced[] = {
    {"core/netdev_max_backlog", "4096"},
    {"core/somaxconn", "4096"},
    {"core/netdev_budget", "4096"},
    {"core/netdev_budget_usecs", "8000"},
    {"core/optmem_max", "204800"},
    {"core/rmem_default", "262144"},
    {"core/rmem_max", "1048576"},
    {"core/wmem_default", "262144"},
    {"core/wmem_max", "1048576"},
    {"ipv4/tcp_slow_start_after_idle", "0"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "1"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "3"},
    {"ipv4/tcp_ecn", "0"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "4"},
    {"ipv4/tcp_synack_retries", "4"},
    {"ipv4/tcp_rmem", "4096 87380 1048576"},
    {"ipv4/tcp_wmem", "4096 65536 524288"},
    {"ipv4/tcp_mem", "16384 32768 49152"},
    {"ipv4/udp_rmem_min", "16384"},
    {"ipv4/udp_wmem_min", "16384"},
    {"ipv4/tcp_notsent_lowat", "16384"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "300"},
    {"ipv4/tcp_adv_win_scale", "1"},
    {"ipv4/tcp_max_syn_backlog", "2048"},
    {"ipv4/tcp_max_tw_buckets", "8192"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "30"},
    {"ipv4/tcp_keepalive_time", "600"},
    {"ipv4/tcp_keepalive_intvl", "30"},
    {"ipv4/tcp_keepalive_probes", "5"},
};

constexpr Knob k_game[] = {
    {"core/netdev_max_backlog", "1000"},
    {"core/somaxconn", "1024"},
    {"core/netdev_budget", "6000"},
    {"core/netdev_budget_usecs", "6000"},
    {"core/optmem_max", "102400"},
    {"core/rmem_default", "131072"},
    {"core/rmem_max", "524288"},
    {"core/wmem_default", "131072"},
    {"core/wmem_max", "524288"},
    {"ipv4/tcp_slow_start_after_idle", "0"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "0"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "5"},
    {"ipv4/tcp_ecn", "0"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "2"},
    {"ipv4/tcp_synack_retries", "2"},
    {"ipv4/tcp_rmem", "4096 32768 131072"},
    {"ipv4/tcp_wmem", "4096 32768 131072"},
    {"ipv4/tcp_mem", "8192 16384 24576"},
    {"ipv4/udp_rmem_min", "32768"},
    {"ipv4/udp_wmem_min", "32768"},
    {"ipv4/tcp_notsent_lowat", "2048"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "100"},
    {"ipv4/tcp_adv_win_scale", "0"},
    {"ipv4/tcp_max_syn_backlog", "1024"},
    {"ipv4/tcp_max_tw_buckets", "4096"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "10"},
    {"ipv4/tcp_keepalive_time", "300"},
    {"ipv4/tcp_keepalive_intvl", "10"},
    {"ipv4/tcp_keepalive_probes", "3"},
};

constexpr Knob k_download[] = {
    {"core/netdev_max_backlog", "16384"},
    {"core/somaxconn", "8192"},
    {"core/netdev_budget", "60000"},
    {"core/netdev_budget_usecs", "8000"},
    {"core/optmem_max", "409600"},
    {"core/rmem_default", "524288"},
    {"core/rmem_max", "16777216"},
    {"core/wmem_default", "524288"},
    {"core/wmem_max", "16777216"},
    {"ipv4/tcp_slow_start_after_idle", "0"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "1"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "3"},
    {"ipv4/tcp_ecn", "1"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "5"},
    {"ipv4/tcp_synack_retries", "5"},
    {"ipv4/tcp_rmem", "4096 87380 16777216"},
    {"ipv4/tcp_wmem", "4096 65536 16777216"},
    {"ipv4/tcp_mem", "131072 262144 393216"},
    {"ipv4/udp_rmem_min", "16384"},
    {"ipv4/udp_wmem_min", "16384"},
    {"ipv4/tcp_notsent_lowat", "4294967295"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "300"},
    {"ipv4/tcp_adv_win_scale", "2"},
    {"ipv4/tcp_max_syn_backlog", "4096"},
    {"ipv4/tcp_max_tw_buckets", "16384"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "30"},
    {"ipv4/tcp_keepalive_time", "1800"},
    {"ipv4/tcp_keepalive_intvl", "60"},
    {"ipv4/tcp_keepalive_probes", "9"},
};

constexpr Knob k_streaming[] = {
    {"core/netdev_max_backlog", "8192"},
    {"core/somaxconn", "4096"},
    {"core/netdev_budget", "30000"},
    {"core/netdev_budget_usecs", "8000"},
    {"core/optmem_max", "204800"},
    {"core/rmem_default", "262144"},
    {"core/rmem_max", "8388608"},
    {"core/wmem_default", "262144"},
    {"core/wmem_max", "8388608"},
    {"ipv4/tcp_slow_start_after_idle", "0"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "1"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "3"},
    {"ipv4/tcp_ecn", "0"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "3"},
    {"ipv4/tcp_synack_retries", "3"},
    {"ipv4/tcp_rmem", "4096 87380 8388608"},
    {"ipv4/tcp_wmem", "4096 65536 4194304"},
    {"ipv4/tcp_mem", "65536 131072 196608"},
    {"ipv4/udp_rmem_min", "32768"},
    {"ipv4/udp_wmem_min", "32768"},
    {"ipv4/tcp_notsent_lowat", "131072"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "200"},
    {"ipv4/tcp_adv_win_scale", "1"},
    {"ipv4/tcp_max_syn_backlog", "2048"},
    {"ipv4/tcp_max_tw_buckets", "8192"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "20"},
    {"ipv4/tcp_keepalive_time", "1200"},
    {"ipv4/tcp_keepalive_intvl", "30"},
    {"ipv4/tcp_keepalive_probes", "5"},
};

constexpr Knob k_social[] = {
    {"core/netdev_max_backlog", "2048"},
    {"core/somaxconn", "2048"},
    {"core/netdev_budget", "4096"},
    {"core/netdev_budget_usecs", "4000"},
    {"core/optmem_max", "204800"},
    {"core/rmem_default", "131072"},
    {"core/rmem_max", "524288"},
    {"core/wmem_default", "131072"},
    {"core/wmem_max", "524288"},
    {"ipv4/tcp_slow_start_after_idle", "0"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "1"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "3"},
    {"ipv4/tcp_ecn", "0"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "3"},
    {"ipv4/tcp_synack_retries", "3"},
    {"ipv4/tcp_rmem", "4096 65536 524288"},
    {"ipv4/tcp_wmem", "4096 65536 262144"},
    {"ipv4/tcp_mem", "16384 32768 49152"},
    {"ipv4/udp_rmem_min", "16384"},
    {"ipv4/udp_wmem_min", "16384"},
    {"ipv4/tcp_notsent_lowat", "32768"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "300"},
    {"ipv4/tcp_adv_win_scale", "1"},
    {"ipv4/tcp_max_syn_backlog", "1024"},
    {"ipv4/tcp_max_tw_buckets", "4096"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "15"},
    {"ipv4/tcp_keepalive_time", "600"},
    {"ipv4/tcp_keepalive_intvl", "15"},
    {"ipv4/tcp_keepalive_probes", "4"},
};

constexpr Knob k_outdoor[] = {
    {"core/netdev_max_backlog", "4096"},
    {"core/somaxconn", "2048"},
    {"core/netdev_budget", "8000"},
    {"core/netdev_budget_usecs", "8000"},
    {"core/optmem_max", "102400"},
    {"core/rmem_default", "262144"},
    {"core/rmem_max", "2097152"},
    {"core/wmem_default", "262144"},
    {"core/wmem_max", "2097152"},
    {"ipv4/tcp_slow_start_after_idle", "1"},
    {"ipv4/tcp_sack", "1"},
    {"ipv4/tcp_window_scaling", "1"},
    {"ipv4/tcp_moderate_rcvbuf", "1"},
    {"ipv4/tcp_timestamps", "1"},
    {"ipv4/tcp_no_metrics_save", "0"},
    {"ipv4/tcp_mtu_probing", "1"},
    {"ipv4/tcp_rfc1337", "1"},
    {"ipv4/tcp_fastopen", "3"},
    {"ipv4/tcp_frto", "2"},
    {"ipv4/tcp_recovery", "1"},
    {"ipv4/tcp_early_retrans", "1"},
    {"ipv4/tcp_reordering", "10"},
    {"ipv4/tcp_ecn", "0"},
    {"ipv4/tcp_tw_reuse", "1"},
    {"ipv4/tcp_syn_retries", "5"},
    {"ipv4/tcp_synack_retries", "5"},
    {"ipv4/tcp_rmem", "4096 87380 2097152"},
    {"ipv4/tcp_wmem", "4096 65536 1048576"},
    {"ipv4/tcp_mem", "32768 65536 98304"},
    {"ipv4/udp_rmem_min", "16384"},
    {"ipv4/udp_wmem_min", "16384"},
    {"ipv4/tcp_notsent_lowat", "65536"},
    {"ipv4/tcp_abort_on_overflow", "0"},
    {"ipv4/tcp_max_reordering", "600"},
    {"ipv4/tcp_adv_win_scale", "1"},
    {"ipv4/tcp_max_syn_backlog", "2048"},
    {"ipv4/tcp_max_tw_buckets", "8192"},
    {"ipv4/ip_local_port_range", "1024 65535"},
    {"ipv4/tcp_fin_timeout", "30"},
    {"ipv4/tcp_keepalive_time", "1200"},
    {"ipv4/tcp_keepalive_intvl", "30"},
    {"ipv4/tcp_keepalive_probes", "5"},
};

constexpr Preset k_presets[] = {
    {"balanced", k_balanced, 42, false},
    {"game", k_game, 42, true},
    {"download", k_download, 42, false},
    {"streaming", k_streaming, 42, false},
    {"social", k_social, 42, false},
    {"outdoor", k_outdoor, 42, false},
};

} // namespace

const Preset* find_preset(std::string_view name) noexcept
{
    if (name == "browsing") {
        name = "social"; // PingPimp maps browsing onto the social table
    }
    for (const Preset& p : k_presets) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

std::span<const Preset> all_presets() noexcept
{
    return k_presets;
}
