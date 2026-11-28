#pragma once

// Plumbing shared by long-lived yay daemons: a single-instance lock that the
// kernel releases on any kind of process death, a self-pipe so poll() with NO
// timeout can still wake for SIGTERM/SIGINT/SIGHUP without a race, an
// interruptible sleep, and a double-fork detach.

#include <sys/types.h>

namespace daemon_util {

class Fd {
public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept
    {
        if (this != &o) {
            reset();
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    ~Fd() noexcept { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void reset() noexcept;

private:
    int fd_;
};

// POSIX record lock on `path`; invalid Fd if another instance holds it.
[[nodiscard]] Fd acquire_lock(const char* path);

// pid of the process holding the lock, or 0.
[[nodiscard]] pid_t lock_holder(const char* path) noexcept;

// Installs SIGTERM/SIGINT (stop) and SIGHUP (reload) handlers that wake a
// pipe. One instance per process.
class Signals {
public:
    [[nodiscard]] bool install() noexcept;
    [[nodiscard]] int fd() const noexcept { return rd_.get(); }
    void drain() const noexcept;
    [[nodiscard]] static bool stop_requested() noexcept;
    // True once per SIGHUP.
    [[nodiscard]] static bool take_reload() noexcept;
    static void request_stop() noexcept;

private:
    Fd rd_;
    Fd wr_;
};

// Sleep up to `ms`, returning early (true) when a stop is requested. A
// pending reload also cuts the sleep short (returns false).
[[nodiscard]] bool wait_ms(const Signals& sig, int ms) noexcept;

// Fork twice, setsid, detach stdio, then run `fn` in the grandchild and
// _exit with its return code. The caller (original process) returns after
// the intermediate child has exited; true on success of the fork.
[[nodiscard]] bool detach_and_run(int (*fn)());

} // namespace daemon_util
