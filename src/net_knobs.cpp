#include "net_knobs.h"

#include "json_config.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

std::string normalize_ws(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending_space = !out.empty();
        } else {
            if (pending_space) {
                out.push_back(' ');
                pending_space = false;
            }
            out.push_back(c);
        }
    }
    return out;
}

std::string read_boot_id()
{
    std::ifstream f("/proc/sys/kernel/random/boot_id");
    std::string id;
    if (f) {
        std::getline(f, id);
    }
    return id;
}

KnobStore::KnobStore(std::string root, std::string snapshot_path, std::string boot_id)
    : root_(std::move(root))
    , snapshot_path_(std::move(snapshot_path))
    , boot_id_(std::move(boot_id))
{
    load();
}

std::string KnobStore::full(std::string_view path) const
{
    return root_ + std::string(path);
}

std::optional<std::string> KnobStore::read(std::string_view path) const
{
    std::ifstream f(full(path));
    if (!f) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return normalize_ws(ss.str());
}

bool KnobStore::write_raw(const std::string& full_path, std::string_view value) const
{
    const int fd = ::open(full_path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);  // same as `echo >`
    if (fd < 0) {
        return false;
    }
    std::string line(value);
    line.push_back('\n');
    const ssize_t n = ::write(fd, line.data(), line.size());
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return n == static_cast<ssize_t>(line.size());
}

KnobResult KnobStore::set(std::string_view path, std::string_view value, std::string_view owner)
{
    const auto current = read(path);
    if (!current) {
        return KnobResult::Missing;
    }

    const std::string want = normalize_ws(value);
    if (*current == want) {
        return KnobResult::Unchanged;
    }

    // Snapshot BEFORE the first write to this path, never after.
    const std::string key(path);
    if (orig_.find(key) == orig_.end()) {
        orig_[key] = Orig{*current, std::string(owner)};
        dirty_ = true;
    }

    if (!write_raw(full(path), value)) {
        return KnobResult::Failed;
    }
    return KnobResult::Applied;
}

std::size_t KnobStore::restore(std::string_view owner)
{
    std::size_t restored = 0;
    for (auto it = orig_.begin(); it != orig_.end();) {
        if (it->second.owner != owner) {
            ++it;
            continue;
        }
        const auto now = read(it->first);
        if (!now || *now == it->second.value || write_raw(full(it->first), it->second.value)) {
            ++restored;
            it = orig_.erase(it);
            dirty_ = true;
        } else {
            ++it; // could not write it back — keep the snapshot, try again later
        }
    }
    return restored;
}

std::size_t KnobStore::restore_all()
{
    std::size_t restored = 0;
    for (auto it = orig_.begin(); it != orig_.end();) {
        const auto now = read(it->first);
        if (!now || *now == it->second.value || write_raw(full(it->first), it->second.value)) {
            ++restored;
            it = orig_.erase(it);
            dirty_ = true;
        } else {
            ++it;
        }
    }
    return restored;
}

std::size_t KnobStore::changed_count(std::string_view owner) const noexcept
{
    return static_cast<std::size_t>(std::count_if(orig_.begin(), orig_.end(),
        [&](const auto& kv) { return kv.second.owner == owner; }));
}

void KnobStore::flush()
{
    if (!dirty_ || snapshot_path_.empty()) {
        dirty_ = false;
        return;
    }
    Json j;
    j["boot_id"] = boot_id_;
    Json orig = Json::object();
    for (const auto& [path, o] : orig_) {
        orig[path] = {{"v", o.value}, {"o", o.owner}};
    }
    j["orig"] = std::move(orig);
    if (save_json(snapshot_path_, j)) {
        dirty_ = false;
    }
}

void KnobStore::load()
{
    if (snapshot_path_.empty() || boot_id_.empty()) {
        return;
    }
    const auto j = load_json(snapshot_path_);
    if (!j || !j->is_object() || j->value("boot_id", std::string{}) != boot_id_) {
        return; // new boot (or unreadable): the kernel already reset everything
    }
    if (!j->contains("orig") || !(*j)["orig"].is_object()) {
        return;
    }
    for (const auto& [path, o] : (*j)["orig"].items()) {
        if (o.is_object() && o.contains("v") && o["v"].is_string()) {
            orig_[path] = Orig{o["v"].get<std::string>(), o.value("o", std::string{})};
        }
    }
}

Stash::Stash(std::string path)
    : path_(std::move(path))
{
    if (path_.empty()) {
        return;
    }
    const auto j = load_json(path_);
    if (j && j->is_object()) {
        for (const auto& [k, v] : j->items()) {
            if (v.is_string()) {
                map_[k] = v.get<std::string>();
            }
        }
    }
}

std::optional<std::string> Stash::get(std::string_view key) const
{
    const auto it = map_.find(std::string(key));
    if (it == map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Stash::has(std::string_view key) const
{
    return map_.find(std::string(key)) != map_.end();
}

void Stash::put(std::string_view key, std::string_view value)
{
    map_[std::string(key)] = std::string(value);
    save();
}

void Stash::erase(std::string_view key)
{
    if (map_.erase(std::string(key)) > 0) {
        save();
    }
}

void Stash::save() const
{
    if (path_.empty()) {
        return;
    }
    Json j = Json::object();
    for (const auto& [k, v] : map_) {
        j[k] = v;
    }
    static_cast<void>(save_json(path_, j));
}
