#pragma once

// Kernel knob writer with a safety net: before the FIRST write to a path the
// original value is snapshotted, and every change can be undone — exactly,
// to what the device had, not to a hard-coded "default".
//
// PingPimp restored by writing its own constants back (e.g. IRQ affinity
// "ff" for every IRQ, whatever it was before). That is wrong on any device
// whose stock values differ, and it only backed up conntrack at all. Here
// every path is covered the same way.
//
// Sysctl/sysfs state resets at reboot, so the snapshot file is tied to
// /proc/sys/kernel/random/boot_id and ignored after a reboot (a stale
// snapshot would otherwise "restore" values the new boot never had).

#include <map>
#include <optional>
#include <string>
#include <string_view>

enum class KnobResult { Applied, Unchanged, Missing, Failed };

// Collapse runs of whitespace (sysctl prints tabs: "4096\t87380\t6291456")
// and trim, so "4096 87380 6291456" compares equal.
[[nodiscard]] std::string normalize_ws(std::string_view s);

class KnobStore {
public:
    // `root` prefixes every absolute path ("" on a device; a temp dir in
    // tests). `snapshot_path` may be empty to disable persistence.
    KnobStore(std::string root, std::string snapshot_path, std::string boot_id);

    // Write `value` to `path`. `owner` groups knobs so a feature can be
    // reverted without touching the others ("profile", "ipv6", "irq", ...).
    KnobResult set(std::string_view path, std::string_view value, std::string_view owner);

    // Undo every knob that `owner` changed; returns how many were restored.
    std::size_t restore(std::string_view owner);
    std::size_t restore_all();

    [[nodiscard]] std::optional<std::string> read(std::string_view path) const;
    [[nodiscard]] std::size_t changed_count() const noexcept { return orig_.size(); }
    [[nodiscard]] std::size_t changed_count(std::string_view owner) const noexcept;

    // Persist the snapshot if it changed. Call once after a batch of set()s.
    void flush();

private:
    struct Orig {
        std::string value;
        std::string owner;
    };

    [[nodiscard]] std::string full(std::string_view path) const;
    [[nodiscard]] bool write_raw(const std::string& full_path, std::string_view value) const;
    void load();

    std::string root_;
    std::string snapshot_path_;
    std::string boot_id_;
    std::map<std::string, Orig> orig_;
    bool dirty_ = false;
};

// Small persistent string map for state that must survive a REBOOT — e.g. the
// original value of a Settings.Global key we overwrote (Settings persist; the
// boot-gated KnobStore snapshot would be gone by the time we need it).
class Stash {
public:
    explicit Stash(std::string path);

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const;
    void put(std::string_view key, std::string_view value);
    void erase(std::string_view key);

private:
    void save() const;

    std::string path_;
    std::map<std::string, std::string> map_;
};

// Current boot id, "" if unavailable.
[[nodiscard]] std::string read_boot_id();
