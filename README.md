# yay v1.5

Personal Android system tweaks as a Magisk module.  
**ARM64 only. Android 11 (API 30) minimum, optimized for Android 12+ (API 31).**

Also bundles a fingerprint-locked Zygisk fix for a SystemUI MediaMetadata
NPE crash — see "SystemUI MediaMetadata NPE Fix" below. It only ever
activates on the exact device/firmware it targets and is an inert no-op
everywhere else, so it ships as part of this module rather than a separate
install.

---

## Architecture

No persistent daemon by default. Binary runs, does its job, exits. The only
resident processes are the **opt-in** ones — the inotify watcher, Auto
Private DNS (`yay_dns`) and the network engine (`yay_net`) — all of which
sleep in `poll()` with no timeout while nothing happens.

| Trigger | What runs | How |
|---|---|---|
| Install / update | `yay_apply --full` | `customize.sh` |
| Early boot (post-fs-data) | `yay_apply --post` | `post-fs-data.sh` |
| Boot completed | `yay_apply --boot` | `service.sh` |
| rules.json changed | `yay_apply --rules` | inotify watcher (opt-in) |
| game_config.json changed | `yay_apply --game` | inotify watcher (opt-in) |
| Boot completed (opt-in via `dns_config.json`) | `yay_dns --start` | `service.sh` |
| Wi-Fi / mobile-data change | `yay_dns` switches Private DNS | netlink event (no polling) |
| Boot completed (opt-in via `net_config.json`) | `yay_net --start` | `service.sh` |
| Foreground app / screen change | `yay_net` switches profile (Smart Auto) | `logcat -b events` line (no polling) |
| App process forked (fingerprint match only) | `systemui_media_fix.so` PLT hook | Zygisk |

Hash caching: `--rules` is skipped at boot if `rules.json` hasn't changed
since last run — component disable and appops mode overrides are persistent
Android-side state (they survive reboot on their own), so re-applying them
when nothing changed would just be redundant binder calls. `--game` and
`--io` are **not** hash-guarded and always run in full on every invocation:
downscale, log cleanup, the I/O scheduler, and TCP congestion control are
all runtime state that Android itself resets every boot, so they need to be
re-applied regardless of whether their config changed.

Every config file is also checked for basic *structural* validity (right
field types, arrays where arrays are expected) before it's handed to any
apply path — see "Config validation" below. A config that fails this check
is rejected outright and logged; nothing partial is applied from it.

---

## What it does

**post-fs-data (early boot)**
- `resetprop`: dex2oat compiler filter, debug flags, phantom process monitor

**boot**
- I/O scheduler: kyber → mq-deadline → deadline (first available per device)
- Disables iostat accounting and entropy contribution per block device
- TCP congestion control: cubic
- Component disable: analytics, measurement, OTA update receivers (see `config/rules.json`)
- Appops: per-package op overrides (see `config/rules.json`)
- Game mode: per-package resolution downscale via Android GameManager API
- fstrim: deferred 60s after boot
- Auto Private DNS (opt-in): Wi-Fi → your NextDNS host, mobile data → Cloudflare (see below)
- Network engine (opt-in): sysctl profiles, Smart Auto by foreground app, per-app boost / isolate, TCP choice, reversible tweaks (see below)

Component disable and appops calls each require one `cmd` invocation per
entry (Android's `pm`/`appops` shell commands don't support a batch form),
but they run with bounded concurrency (several at once, not strictly
one-at-a-time) and a per-item timeout, so a large `rules.json` doesn't turn
into a long serial wait — see `exec_batch()` in `src/exec_util.h` for the
exact mechanism and tunables. If spawning a command fails repeatedly in a
row (5 consecutive failures by default — almost always a sign the device is
out of file descriptors or process slots), the batch stops attempting
further spawns and fails the rest immediately rather than retrying in a
tight loop while resources are already under pressure.

---

## Config validation

Every load of `rules.json`, `game_config.json`, or `io_config.json` goes
through two layers before anything is acted on:

1. **Structural validation** (`validate_config_shape` in `json_config.cpp`)
   — catches a config that parses as valid JSON but has the wrong shape:
   `"components"` present but not an array, an array entry that isn't an
   object, a `package`/`component`/`op` field of the wrong type. A file
   that fails this check is rejected as a whole and logged with the
   specific reason — nothing partial is applied.
2. **Per-entry validation** (in `rules_engine.cpp`) — even a structurally
   valid file can contain individual entries with malformed values (stray
   whitespace, control characters, `..`, empty segments). These are
   skipped individually with a warning rather than passed through to
   `cmd package` / `cmd appops`, and duplicate entries (same package +
   component, or same package + op) are deduplicated so they aren't
   executed twice.

This matters most once config files may be written by something other
than a human hand-editing JSON — e.g. a future web UI — where a bug on
that side should surface as a clear rejected-config error in the log,
not as a partially-applied or silently-wrong run.

### Dry-run mode

`yay_apply --rules --dry-run` loads and validates `rules.json` exactly
like a real `--rules` run, but logs every `cmd package` / `cmd appops`
invocation it *would* make instead of actually running them — no
subprocess is spawned, and the hash cache is neither read nor written.
Useful for previewing the effect of a config change (by hand or from a
future web UI) before committing to it. Output goes to the normal log
(`/data/adb/yay/run.log` / `adb logcat -s yay`).

```sh
/data/adb/modules/yay/bin/yay_apply --rules --dry-run
```

`--dry-run` is only meaningful paired with `--rules`; it's ignored (with a
log warning) if combined with any other mode.

---

## Build

Requires Android NDK r25c+.

```sh
# Set NDK path (one of):
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/26.x.x

# Build release binaries
./build.sh release

# Binaries land in bin/
# yay_apply  — one-shot applier
# yay_watch  — optional inotify watcher
# yay_dns    — Auto Private DNS daemon
# yay_net    — network engine (PingPimp port)
```

Targets API 31, ABI arm64-v8a, C++20, static libc++ (NDK's `c++_static`).

nlohmann/json is pulled automatically via CMake `FetchContent` (pinned to a
tagged release) during configure — no manual vendoring step, and no
internet access needed beyond what `cmake -B` itself does on first
configure (cached afterward in the build directory).

---

## Configuration

All config lives in `/data/adb/yay/config/` after install.  
Edit JSON files directly. If inotify watcher is enabled, changes apply immediately.

### rules.json

```json
{
  "components": [
    {
      "package": "com.example.app",
      "component": "com.example.app.SomeService",
      "action": "disable",
      "enabled": true,
      "note": "optional comment"
    }
  ],
  "appops": [
    {
      "package": "com.example.app",
      "op": 40,
      "mode": "ignore",
      "enabled": true,
      "note": "op 40 = GET_USAGE_STATS"
    }
  ]
}
```

Set `"enabled": false` to skip an entry without deleting it.

`package` and `component` must look like ordinary Java-style dotted
identifiers (letters, digits, `.`, `_`, `$`, no leading/trailing dot, no
`..`) — anything else is skipped with a warning rather than passed to
`cmd package`. `op` may be an integer or a numeric string; negative values
are rejected.

### game_config.json

```json
{
  "games": [
    {
      "package": "com.example.game",
      "enabled": true,
      "downscale": 0.7,
      "cleanup_logs": true,
      "log_dirs": [
        "/data/media/0/Android/data/com.example.game/files/logs"
      ]
    }
  ]
}
```

`downscale`: 0.0–1.0 (1.0 = native resolution). Requires Android 12+ (API 31).  
`cleanup_logs`: purge contents of `log_dirs` on apply. No bind-mount, no mount table footprint.

`log_dirs` entries must resolve under the app's own
`/data/media/0/Android/{data,obb}/<package>/...` tree — anything else
(wrong app, wrong root, `..`, or just the app's storage root with nothing
under it) is rejected and logged instead of acted on.

### io_config.json

```json
{
  "scheduler_preference": ["kyber", "mq-deadline", "deadline", "cfq"],
  "read_ahead_kb": 128
}
```

---

## Optional: inotify watcher

The watcher (`yay_watch`) watches `/data/adb/yay/config/` for file changes and
calls `yay_apply` with the targeted mode. It blocks in `inotify_wait` — near-zero
CPU and memory overhead when idle.

To enable (or use the WebUI toggle in Settings):
```sh
touch /data/adb/yay/enable_watch
# Takes effect on next boot, or restart service.sh manually
```

To disable:
```sh
rm /data/adb/yay/enable_watch
# Kill running watcher:
pkill yay_watch
```

The flag lives in `/data/adb/yay/` so it survives module updates. (Up to
v1.3 it was `/data/adb/modules/yay/enable_watch`, which every update wiped —
the watcher silently turned off. The installer and `service.sh` migrate an
old flag automatically.)

---

## Auto Private DNS (`yay_dns`)

Some carriers selectively block NextDNS (SNI/DPI filtering), which leaves
Android showing *"Private DNS server cannot be accessed"* on mobile data.
`yay_dns` swaps Android's Private DNS (DNS-over-TLS) endpoint by connection
type, and does nothing else:

| Active connection | Private DNS |
|---|---|
| Wi-Fi only | `wifi_host` — your personal NextDNS endpoint |
| Mobile data only | `mobile_host` — default `1dot1dot1dot1.cloudflare-dns.com` |
| **Wi-Fi + mobile data both up** | **mobile data wins** → `mobile_host` (set `"prefer": "wifi"` to flip) |
| Nothing usable (airplane mode, no signal) | left untouched |

**Opt-in.** It needs your own NextDNS endpoint, which no default can supply.
Enable it in the WebUI (*Settings → Auto Private DNS*), or edit
`/data/adb/yay/config/dns_config.json`:

```json
{
  "version": 1,
  "enabled": true,
  "wifi_host": "abc123.dns.nextdns.io",
  "mobile_host": "1dot1dot1dot1.cloudflare-dns.com",
  "debounce_ms": 1500,
  "cellular_debounce_ms": 300
}
```

`prefer` (`"cellular"` default, or `"wifi"`) decides who wins when both are up.

Optional advanced keys: `wifi_ifaces` (default `["wlan0"]`) and
`cellular_prefixes` (default covers Qualcomm `rmnet*`, MediaTek `ccmni*`,
Unisoc `seth*`/`sipa*`, plus `v4-` 464XLAT variants). Hosts must be real
hostnames — IP literals are rejected.

### How it works

- **Zero polling.** Blocks in `poll()` with an infinite timeout on a
  `NETLINK_ROUTE` socket subscribed to link + IPv4/IPv6 address changes. On a
  stable network it has no timers and no wakeups. Events for unrelated
  interfaces (`tun*`, `dummy*`, `p2p-*`, …) are filtered out immediately.
- **No VPN / TUN, no packet interception.** The only thing it writes is
  `Settings.Global` `private_dns_mode` / `private_dns_specifier` through
  `cmd settings`. Traffic stays entirely in the kernel/netd.
- **Which network?** A network counts as active when its interface has a
  usable address (IPv4 non-link-local, or IPv6 global). Wi-Fi is the station
  interface only (`wlan0`, matched exactly) — hotspot/Wi-Fi Direct interfaces
  don't count as "connected to Wi-Fi". If both are up, `prefer` decides
  (default: the SIM wins). A phone with mobile data **switched off** still
  keeps an IMS/VoLTE cellular interface with an address, so when both are up
  the daemon also reads `settings get global mobile_data`: if it is `0`, the
  cellular interface is not treated as data and Wi-Fi wins. That check is
  skipped when only cellular is up, so an odd reading (e.g. dual-SIM) can
  never stop the mobile profile from applying.
- **Fast where it matters.** The debounce is direction-aware: when mobile
  data is the *only* network up, the carrier may block the NextDNS profile
  and Private DNS fails until we switch, so that direction uses
  `cellular_debounce_ms` (default 300 ms). Moving to Wi-Fi breaks nothing in
  the meantime, so it keeps the full `debounce_ms` (default 1500 ms).
  Measured handover: Wi-Fi drops → Cloudflare applied in ~0.3 s instead of
  ~1.5 s. Trade-off: a Wi-Fi roam where `wlan0` loses its address for longer
  than 300 ms now causes two harmless writes; raise `cellular_debounce_ms`
  if you prefer fewer.
- **Robust.** Debounced (trailing edge, capped so endless flapping still
  converges), idempotent (never rewrites a value that's already correct),
  every write is read back and retried on failure, waits for the Settings
  Provider after boot, and handles netlink overflow by re-evaluating from
  scratch. Single instance via a kernel record lock — no stale pidfile.
- **Respects manual changes.** The daemon remembers what it wrote. If
  Private DNS is something else — off, "Automatic", a custom host — it is
  treated as *your* choice and left alone, across any number of network
  changes. Automation resumes when the setting is put back on one of the two
  module hosts (checked at the next network event — it never polls), or after
  a reboot. Editing `wifi_host`/`mobile_host` is not mistaken for a manual
  change.

### Commands

```sh
yay_dns --start    # start detached, or make the running one reload config
yay_dns --stop
yay_dns --once     # evaluate + apply once, no daemon (debugging)
yay_dns --status   # JSON: running, network, current Private DNS, hosts
yay_dns            # foreground
```

Logs go to `run.log` / `adb logcat -s yay_dns`, and only on actual state
changes (switch, manual override detected, resume).

### Limitations

- "Active" is decided by interface addresses, not by Android's validated
  default network. If you're on a Wi-Fi with no internet while mobile data
  carries the traffic, the Wi-Fi profile is applied.
- With the default `prefer: cellular`, on Wi-Fi *with mobile data enabled*
  the mobile profile (Cloudflare) is what's active — NextDNS applies on Wi-Fi
  only when mobile data is off or there is no SIM data. Toggling mobile data
  is picked up at the next network event; if your ROM keeps the data
  interface up after toggling, it may lag until one occurs.
- Manual changes are noticed at the next network event that changes the kind
  of network, not instantly. Events that don't change it (IPv6 address churn,
  a second address on the same interface) are skipped without reading
  Settings at all, so they cost no `cmd` processes.
- Stopping/removing the module leaves Private DNS at whatever it was last set to.
- A VPN that sets its own DNS takes precedence over Private DNS, as on stock Android.

### Tests

The decision logic (interface classification, manual-override state machine,
hostname validation) is pure and has host-side unit tests, run by CI:

```sh
g++ -std=c++20 -Isrc -I<nlohmann include dir> \
    tests/dns_policy_test.cpp src/dns_policy.cpp -o /tmp/t && /tmp/t
```

---

## Network engine (`yay_net`)

A port of [PingPimp](https://github.com/fuckyoustan/PingPimp) (Apache-2.0,
see `NOTICE`), re-built around this module's rules: event-driven, reversible,
and tested. Opt-in — enable it in the WebUI's **Network** tab.

| Feature | What it does |
|---|---|
| **Profiles** | `balanced`, `game`, `download`, `streaming`, `social`, `outdoor` — sets of 42 sysctls under `/proc/sys/net`. `off` = the device's own values. |
| **Smart Auto** | Foreground app → profile: your Games list (+ apps that declare the GAME category) → `game`; video apps → `streaming`; chat/browsers → `social`; anything else → your base profile. Explicit `app_profiles` in the config beat every heuristic. Screen off → base profile. |
| **TCP congestion control** | Leave alone, `auto` (per profile: westwood/bbr for games, bbr for downloads, cubic otherwise), or a specific algorithm the kernel offers. |
| **Boost apps** | DSCP 46 (MARK fallback if the kernel lacks `xt_DSCP`) on the app's outgoing traffic. Only helps where the router honours DSCP/WMM. |
| **Isolate apps** | `REJECT` for the app's UID — it loses internet access. |
| **Toggles** | IPv6 off, Wi-Fi power-save off (needs `iw`), Wi-Fi low-latency, conntrack tuning, network-stats off, ultra data saver, NIC IRQs on big cores, ksoftirqd priority. |

### How it works

- **No polling.** `poll()` with no timeout over: a `NETLINK_ROUTE` socket
  (link/address changes), the stdout of `logcat -b events` (AOSP's
  `system_server` logs each resumed activity and each screen on/off as it
  happens — `yay_net --selftest` checks that your ROM does), and a signal
  pipe. Nothing runs while you stay in one app on a stable network.
  The only timers are one-shot: a debounce after a network burst, and the
  "app must stay foreground `confirm_ms`" confirmation (so flicking through
  apps never churns the kernel). A rapid flick commits only the app you
  settle on.
- **Everything is reversible, to the device's real values.** Before the first
  write to any path its original value is snapshotted (tied to the boot id —
  a snapshot from a previous boot is ignored). `yay_net --restore` — or just
  switching the engine off — puts every sysctl, IRQ mask, Settings key and
  iptables chain back. Settings changes (`netstats_enabled`, data saver) are
  kept in a stash that survives reboots, because those persist.
- **Per-app rules are atomic.** Boost/Isolate live in dedicated chains
  (`YAY_QOS` in mangle, `YAY_JAIL` in filter), each referenced by exactly one
  jump, and replaced with a single `iptables-restore --noflush` transaction
  per family (IPv4 + IPv6). Re-applying never adds rules; netd rebuilding
  `OUTPUT` is repaired on the next network event.
- **Packages resolved with one file read** (`/data/system/packages.list`).
- **Writes are skipped when the value is already right**, so a repeated apply
  costs a few `read()`s and no process spawns.

### What changed from PingPimp

| PingPimp | yay_net |
|---|---|
| Daemon wakes every 10 s to run `dumpsys power`, every 10 s more to run `cmd activity stack info` and re-exec itself, every 30 s to `ping` 1.1.1.1 ×4 | No timers while idle; foreground/screen come from `logcat -b events` |
| `pm list packages -U` (a framework dump) **per app** | one read of `packages.list` |
| Rules added with `iptables -I OUTPUT …` each pass; as written, nothing removes the previous ones, so the chain grows | dedicated chains replaced atomically; jump asserted once |
| "Default" preset written back as its own constants; IRQ affinity "restored" to `ff` for every IRQ | exact per-path restore from a snapshot |
| `tcp_abort_on_overflow=1` in the game preset | 0 everywhere (it only resets connections when an accept queue overflows — meaningless for a client) |
| `service.sh` waits in a loop for `wpa_supplicant.conf` before starting anything | no such wait |
| Uninstall forces `private_dns_mode=off` | uninstall restores what it changed and never touches your Private DNS |
| Private DNS flip (screen-on poll, `sleep(3)`, no manual-override handling) | superseded by `yay_dns` (event-driven, respects manual changes); PingPimp's provider list is available as presets in Settings |

### Not ported, on purpose

- **Active probing** (4× `ping` per evaluation, MTU discovery by pinging): it
  sends traffic and wakes the radio, a carrier that filters ICMP reads as
  "100 % loss", and guessing an MTU can break connections.
- **"IPv6 off" via airplane-mode toggling and rewriting the APN protocol**:
  drops calls/hotspot, persists in the telephony database, and "re-enable"
  forces `IPV4V6` on every APN instead of restoring each one. The sysctl
  toggle is ported.
- **Editing `wpa_supplicant.conf` and the MediaTek `wifi.cfg` overlay** (EDCCA
  / roaming thresholds): persistent Wi-Fi-stack changes on a *Wi-Fi driver
  config*, with regulatory implications. Not something to ship by default.
- **NIC offload (`ethtool -K gro/gso off`)**, **tc qdisc/filter setup**, and
  **per-SSID DNS**: low value or untestable here; open to a follow-up.

### Commands

```sh
yay_net --start      # start detached, or make the running one reload config
yay_net --stop       # stop (leaves applied values in place)
yay_net --restore    # stop AND put everything back to the device's values
yay_net --once       # apply the base profile once, no daemon
yay_net --status     # JSON: profile, foreground app, rules, tcp, knobs
yay_net --selftest   # does this ROM emit the events Smart Auto needs?
```

### `net_config.json`

```json
{
  "enabled": true,
  "profile": "balanced",
  "auto": true,
  "auto_detect_games": true,
  "confirm_ms": 4000,
  "tcp_algo": "auto",
  "boost_apps": ["com.mobile.legends"],
  "jail_apps": [],
  "app_profiles": { "com.example.torrent": "download" },
  "toggles": { "wifi_ps_off": true }
}
```

### Limitations — read these

- **The presets are PingPimp's values and have not been independently
  benchmarked.** On a modern phone most of them are marginal; treat them as
  presets to try, not guaranteed wins. Use `off` to see the difference.
- **Smart Auto depends on ROM logging.** Run `yay_net --selftest` (or the
  button in the Network tab) once; if it reports no foreground events, Smart
  Auto cannot work on that ROM and the base profile simply stays in force.
- Per-app rules cover the primary user (user 0) only.
- Boost helps only with routers/APs that honour DSCP; on mobile data it is
  usually ignored by the network.
- Not yet run on a real Android device: it was exercised on a Linux kernel
  with real sysctls, netfilter and netlink, and fake `logcat`/`cmd`/`iw`.
  The first thing to check on-device is `--selftest` and `--status`.

---

## Logs

```
/data/adb/yay/run.log   — all apply runs
adb logcat -s yay       — real-time (tag: yay / yay_watch)
```

---

## Data paths

```
/data/adb/yay/
├── config/
│   ├── rules.json
│   ├── game_config.json
│   ├── io_config.json
│   ├── dns_config.json
│   └── net_config.json
├── cache/
│   ├── rules.hash      ← hash of last applied rules.json (rules.json only — see Hash caching above)
│   └── yay_dns.lock    ← single-instance lock for yay_dns
└── run.log
```

Config and cache survive module updates. Uninstalling Magisk module does NOT
delete `/data/adb/yay/` — remove manually if desired.

---

## SystemUI MediaMetadata NPE Fix (Zygisk)

Originally a standalone module, merged into yay so there's one module to
install instead of two. Works around a null `MediaMetadata` crash in
TranSystemUI on one specific firmware build.

**Fingerprint-locked at both install time and runtime** — it only ever
does anything on:
- SDK 31
- `ro.build.fingerprint` = `Infinix/X6815B-OP/Infinix-X6815B:12/SP1A.210812.016/231020V486:user/release-keys`
- `com.android.systemui` versionCode `202309220` / versionName `12.0.2.187`
- arm64-v8a

`customize.sh` checks this at install time — on a mismatch, only the
Zygisk `.so` is skipped (removed from the installed module); every other
yay feature still installs and runs normally. `module.cpp`'s
`isSupportedBuild()` re-checks the same fingerprint at runtime on every
Zygisk load, independent of the install-time check, so a system update
that changes the fingerprint after install causes the fix to self-disable
rather than run against a binary layout it was never verified against.

Requires **Magisk v26.1+** (or a Zygisk-capable KernelSU/APatch setup —
Zygisk Next / ReZygisk) for the bundled Zygisk component to load; the
rest of yay has no such requirement.

No toggle for this in the webui — it's a firmware-bug workaround with its
own auto-detection, not a preference to switch on or off.

---

## Module structure

```
yay/
├── bin/
│   ├── yay_apply       ← compiled ARM64 binary
│   ├── yay_watch       ← compiled ARM64 binary
│   ├── yay_inspect     ← compiled ARM64 binary
│   ├── yay_dns         ← compiled ARM64 binary (Auto Private DNS)
│   └── yay_net         ← compiled ARM64 binary (network engine)
├── zygisk/
│   └── arm64-v8a.so    ← SystemUI MediaMetadata NPE Fix (fingerprint-locked)
├── config/             ← default configs (copied to /data/adb/yay/config/ on install)
│   ├── rules.json
│   ├── game_config.json
│   ├── io_config.json
│   ├── dns_config.json
│   └── net_config.json
├── tests/              ← host-side unit tests (not shipped)
├── src/                ← C++20 source (not shipped in module zip)
│   └── media_fix/       ← Zygisk module source (module.cpp, binder.cpp/hpp, zygisk.hpp)
├── webroot/             ← built WebUI (KernelSU WebUI X / MMRL)
├── CMakeLists.txt
├── build.sh
├── customize.sh
├── post-fs-data.sh
├── service.sh
├── uninstall.sh        ← stops daemons, runs `yay_net --restore`
├── NOTICE              ← third-party notices (PingPimp, Apache-2.0)
├── LICENSES/
├── skip_mount
└── module.prop
```

---

## Changelog

**v1.5**
- New **network engine** (`yay_net`, opt-in) — a port of PingPimp: profiles,
  Smart Auto by foreground app, TCP choice, per-app Boost / Isolate, and a
  set of reversible toggles. Event-driven (netlink + `logcat -b events`), every
  change snapshotted and undone to the device's real values, per-app rules in
  dedicated iptables chains replaced atomically. See the section above for
  what changed from PingPimp and what was deliberately not ported.
- WebUI: new **Network** tab (grouped like PingPimp's Config page), app pickers
  with real launcher icons, DNS provider presets in Settings (86 providers).
- `uninstall.sh`: stops the daemons and runs `yay_net --restore`.
- Shared `daemon_util` / `netlink_util`; new unit tests for the policy logic and
  the knob store; WebUI tests for the store, picker and every tab.

**v1.4**

*Fixes*
- **Watcher setting survived nothing:** `enable_watch` lived inside the module
  directory, which every update replaces — the watcher silently turned off
  after each update. It now lives in `/data/adb/yay/` and the installer /
  `service.sh` migrate an old flag automatically.
- **Logger:** now notices when another process rotated `run.log` out from
  under it (by inode). Long-lived `yay_dns` / `yay_watch` used to keep
  appending to `run.log.1`, leaving the WebUI's "Current" tab empty.
- **CI:** `webroot/` is replaced, not merged, on every build — Vite's hashed
  filenames used to pile old assets up inside the module zip.

*Daemons*
- `yay_dns`: direction-aware debounce — after Wi-Fi drops, the mobile profile
  is applied in ~0.3 s instead of ~1.5 s (new key `cellular_debounce_ms`).
- `yay_dns`: Settings reads run concurrently and the write is verified by
  reading back only what was written (serial spawns on the handover critical
  path: 5 → 3).
- `yay_dns`: events that don't change the network kind no longer spawn
  `cmd settings` (30 such events: 60 spawns → 0).
- `yay_watch`: no longer wakes every 5 s while idle (`poll` with no timeout +
  self-pipe for SIGTERM/SIGINT); retries `waitpid` on EINTR.

*WebUI*
- **One root exec for all data.** Every config + the watcher flag is read in
  a single shell call at startup and served from memory; previously each tab
  paid for its own root-shell spawn the first time it was opened (~11 across
  the app). Refreshing Home is one batched read instead of four.
- **One JS + one CSS file** (down from 13 after the short-lived code-split in
  an earlier build): on KernelSU every asset is a root file read, and a tab
  tapped before its chunk arrived stalled with no feedback.
- **Motion:** tab changes fade through (opacity-only — see the note in
  `main.css` about `position: fixed` FABs); the bottom-nav indicator grows in;
  every button squashes on press with a springy release; the switch thumb is
  transform-only; sheets/snackbars enter decelerating and leave
  accelerating; all of it honours `prefers-reduced-motion`.
- **No spinners on first paint:** skeleton placeholders shaped like the
  content; Settings switches show disabled instead of being swapped for a
  spinner.
- Navbar without `backdrop-blur`; Home/Log refresh when their tab is shown
  again (stale-while-revalidate); Log renders the newest 300 lines;
  Rules/Games rows use identity keys + `content-visibility`; no
  `getComputedStyle` in Ripple; `touch-action: manipulation`; scroll
  containment.
- Version shown in the UI is injected from `module.prop` at build time.
- WebUI tests (`bun run test`, run in CI): the batch snapshot against a real
  shell, store behaviour, and a full-app smoke test over every tab.

**v1.3**
- New **Auto Private DNS** (`yay_dns`, opt-in): Wi-Fi → your NextDNS host,
  mobile data → Cloudflare (mobile data wins when both are up; `prefer`
  key), to get around carrier blocking of NextDNS. Netlink event-driven
  (zero polling), no VPN/TUN, debounced and idempotent, respects manual
  Private DNS changes. WebUI toggle + host fields in Settings.
- `ConfigKind::DnsConfig` structural validation for `dns_config.json`.
- Host-side unit tests for the policy logic, run in CI.

**v1.2**
- Merged the standalone "SystemUI MediaMetadata NPE Fix" Zygisk module into
  yay — one module to install instead of two. See the dedicated section
  above for the fix itself; nothing about its logic changed, only how it's
  packaged and built.
- Its build moved from a separate `ndk-build`/`Android.mk` project into
  this repo's CMake build (`systemui_media_fix` target) — same compiler
  flags, same hardening (LTO, RELRO, BIND_NOW, no exec stack, no TEXTREL,
  stripped, no STL), now built by the same `cmake --build` invocation as
  `yay_apply`/`yay_watch`/`yay_inspect` instead of a second toolchain
  invocation.
- `customize.sh` now also verifies the target firmware fingerprint and
  either enables or removes the Zygisk `.so` accordingly — a mismatch does
  not abort installing the rest of yay.
- Minimum Magisk version raised to v26.1 (from v20.4) — needed for stable
  built-in Zygisk support, which the merged fix depends on. Only relevant
  when installing via Magisk; KernelSU/APatch installs are unaffected.
- Added `skip_mount` (neither yay nor the merged fix touch a `system/`
  overlay).

**v1.1**
- `rules.json` entries (`package`, `component`, `op`) are now validated
  against a strict identifier format before being passed to `cmd package` /
  `cmd appops`; malformed entries are skipped and logged instead of
  executed as-is.
- Duplicate `rules.json` entries (same target) are deduplicated before
  execution.
- New `validate_config_shape()` structural check runs on every config load
  (`rules.json`, `game_config.json`, `io_config.json`) — a file with the
  wrong top-level shape is rejected outright with a specific reason logged,
  instead of partially applying whatever happened to parse.
- New `yay_apply --rules --dry-run`: logs every command that would run
  without executing anything or touching the hash cache.
- `exec_batch()` now opens a circuit breaker after repeated consecutive
  spawn failures (default: 5) instead of retrying fork()/pipe2() in a tight
  loop under resource pressure.
- Minor clarity fix in `game_mode.cpp`'s `is_safe_purge_target` (no
  functional change).
