#include "daemon_util.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace daemon_util {

namespace {

volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_reload = 0;
int g_wake_wr = -1;

void on_signal(int sig)
{
    const int saved = errno;
    if (sig == SIGHUP) {
        g_reload = 1;
    } else {
        g_stop = 1;
    }
    if (g_wake_wr >= 0) {
        const char c = 0;
        static_cast<void>(::write(g_wake_wr, &c, 1));
    }
    errno = saved;
}

} // namespace

void Fd::reset() noexcept
{
    if (fd_ >= 0) {
        while (::close(fd_) < 0 && errno == EINTR) {
        }
        fd_ = -1;
    }
}

Fd acquire_lock(const char* path)
{
    Fd fd(::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!fd.valid()) {
        return Fd();
    }

    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd.get(), F_SETLK, &fl) != 0) {
        return Fd();
    }

    // Informational only (humans doing `cat`) — the lock is the truth.
    if (::ftruncate(fd.get(), 0) == 0) {
        char line[24];
        const int n = std::snprintf(line, sizeof(line), "%d\n", static_cast<int>(::getpid()));
        if (n > 0) {
            static_cast<void>(::write(fd.get(), line, static_cast<size_t>(n)));
        }
    }
    return fd;
}

pid_t lock_holder(const char* path) noexcept
{
    Fd fd(::open(path, O_RDWR | O_CLOEXEC));
    if (!fd.valid()) {
        return 0;
    }
    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (::fcntl(fd.get(), F_GETLK, &fl) != 0 || fl.l_type == F_UNLCK) {
        return 0;
    }
    return fl.l_pid;
}

bool Signals::install() noexcept
{
    int p[2] = {-1, -1};
    if (::pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) {
        return false;
    }
    rd_ = Fd(p[0]);
    wr_ = Fd(p[1]);
    g_wake_wr = wr_.get();

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
    return true;
}

void Signals::drain() const noexcept
{
    char buf[64];
    while (::read(rd_.get(), buf, sizeof(buf)) > 0) {
    }
}

bool Signals::stop_requested() noexcept
{
    return g_stop != 0;
}

bool Signals::take_reload() noexcept
{
    if (g_reload) {
        g_reload = 0;
        return true;
    }
    return false;
}

void Signals::request_stop() noexcept
{
    g_stop = 1;
}

bool wait_ms(const Signals& sig, int ms) noexcept
{
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::milliseconds(ms);

    while (!Signals::stop_requested()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - clock::now()).count();
        if (left <= 0) {
            break;
        }
        pollfd pfd{sig.fd(), POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(left));
        if (rc > 0) {
            sig.drain();
        } else if (rc < 0 && errno != EINTR) {
            break;
        }
        if (g_reload) {
            break;
        }
    }
    return Signals::stop_requested();
}

bool detach_and_run(int (*fn)())
{
    const pid_t p1 = ::fork();
    if (p1 < 0) {
        return false;
    }
    if (p1 > 0) {
        int status = 0;
        static_cast<void>(::waitpid(p1, &status, 0));
        return true;
    }

    // Intermediate child: new session, then fork again so the daemon is never
    // a session leader and can't reacquire a controlling terminal.
    ::setsid();
    const pid_t p2 = ::fork();
    if (p2 != 0) {
        _exit(p2 < 0 ? 1 : 0);
    }

    ::umask(077);
    static_cast<void>(::chdir("/"));
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            ::close(devnull);
        }
    }
    _exit(fn());
}

} // namespace daemon_util
