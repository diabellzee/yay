#include "net_rules.h"

#include "exec_util.h"
#include "logger.h"
#include "net_policy.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <set>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Family {
    const char* ipt;
    const char* restore;
};
constexpr Family kFamilies[] = {{"iptables", "iptables-restore"}, {"ip6tables", "ip6tables-restore"}};

// Run argv with `input` on stdin; merged output is returned in `out`.
// Exit code, or -1 if it could not be run / timed out.
int run_with_stdin(const std::vector<std::string>& argv, std::string_view input,
                   std::string& out, int timeout_sec)
{
    out.clear();
    int in_p[2];
    int out_p[2];
    if (::pipe2(in_p, O_CLOEXEC) != 0) return -1;
    if (::pipe2(out_p, O_CLOEXEC) != 0) {
        ::close(in_p[0]); ::close(in_p[1]);
        return -1;
    }

    std::vector<char*> args;
    args.reserve(argv.size() + 1U);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1]}) ::close(fd);
        return -1;
    }
    if (pid == 0) {
        ::dup2(in_p[0], STDIN_FILENO);
        ::dup2(out_p[1], STDOUT_FILENO);
        ::dup2(out_p[1], STDERR_FILENO);
        ::execvp(args[0], args.data());
        _exit(127);
    }

    ::close(in_p[0]);
    ::close(out_p[1]);

    // A dead child must not kill us with SIGPIPE while we feed it.
    struct sigaction ign{}, old{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ::sigaction(SIGPIPE, &ign, &old);

    std::size_t off = 0;
    while (off < input.size()) {
        const ssize_t n = ::write(in_p[1], input.data() + off, input.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    ::close(in_p[1]);
    ::sigaction(SIGPIPE, &old, nullptr);

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(timeout_sec);
    char buf[512];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            ::kill(pid, SIGKILL);
            break;
        }
        pollfd pfd{out_p[0], POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(left));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) continue;
        const ssize_t n = ::read(out_p[0], buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < 1024U) out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0 || errno != EINTR) {
            break;
        }
    }
    ::close(out_p[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// `iptables-restore --noflush`: a chain declared with ':' is replaced
// atomically; every other chain/table is left alone. `-w` (wait for the
// xtables lock netd may hold) is not in every Android build, so retry
// without it.
bool restore(const Family& fam, const std::string& script, std::string& err)
{
    std::string out;
    if (run_with_stdin({fam.restore, "-w", "--noflush"}, script, out, 8) == 0) {
        return true;
    }
    if (run_with_stdin({fam.restore, "--noflush"}, script, out, 8) == 0) {
        return true;
    }
    err = std::string(fam.restore) + ": " + out.substr(0, 160);
    return false;
}

bool ipt(const Family& fam, std::initializer_list<std::string_view> args)
{
    std::vector<std::string_view> argv{fam.ipt, "-w"};
    argv.insert(argv.end(), args.begin(), args.end());
    return exec_cmd(std::span<const std::string_view>(argv), std::chrono::seconds(5)).ok();
}

void ensure_jump(const Family& fam, std::string_view table, std::string_view chain)
{
    if (ipt(fam, {"-t", table, "-C", "OUTPUT", "-j", chain})) {
        return;
    }
    static_cast<void>(ipt(fam, {"-t", table, "-I", "OUTPUT", "1", "-j", chain}));
}

void drop_chain(const Family& fam, std::string_view table, std::string_view chain)
{
    // Remove every jump (a manually duplicated one would otherwise survive).
    for (int i = 0; i < 16 && ipt(fam, {"-t", table, "-D", "OUTPUT", "-j", chain}); ++i) {
    }
    static_cast<void>(ipt(fam, {"-t", table, "-F", chain}));
    static_cast<void>(ipt(fam, {"-t", table, "-X", chain}));
}

} // namespace

RulesState TrafficRules::apply(const std::vector<unsigned>& boost_uids,
                               const std::vector<unsigned>& jail_uids)
{
    std::set<unsigned> jail(jail_uids.begin(), jail_uids.end());
    std::set<unsigned> boost_set;
    for (const unsigned u : boost_uids) {
        if (jail.count(u) == 0) boost_set.insert(u); // a jailed app is not worth prioritising
    }
    const std::vector<unsigned> boost(boost_set.begin(), boost_set.end());
    const std::vector<unsigned> jail_v(jail.begin(), jail.end());

    state_.error.clear();

    if (boost.empty() && jail_v.empty()) {
        if (state_.active) {
            teardown();
        }
        return state_;
    }

    bool ok = true;

    // QoS: try DSCP; a kernel without xt_DSCP rejects the whole transaction,
    // in which case fall back to the fwmark variant.
    {
        bool dscp = state_.dscp;
        std::string script = make_qos_script(boost, dscp);
        if (script != last_qos_) {
            std::string err;
            bool done = true;
            for (const auto& fam : kFamilies) {
                if (!restore(fam, script, err)) { done = false; break; }
            }
            if (!done && dscp) {
                dscp = false;
                script = make_qos_script(boost, false);
                done = true;
                for (const auto& fam : kFamilies) {
                    if (!restore(fam, script, err)) { done = false; break; }
                }
            }
            if (done) {
                state_.dscp = dscp;
                last_qos_ = script;
                for (const auto& fam : kFamilies) ensure_jump(fam, "mangle", kQosChain);
            } else {
                state_.error = err;
                ok = false;
            }
        }
    }

    {
        const std::string script = make_jail_script(jail_v);
        if (script != last_jail_) {
            std::string err;
            bool done = true;
            for (const auto& fam : kFamilies) {
                if (!restore(fam, script, err)) { done = false; break; }
            }
            if (done) {
                last_jail_ = script;
                for (const auto& fam : kFamilies) ensure_jump(fam, "filter", kJailChain);
            } else {
                state_.error = err;
                ok = false;
            }
        }
    }

    state_.boost = boost.size();
    state_.jail = jail_v.size();
    state_.active = true;
    if (!ok) {
        Logger::warn("yay_net: traffic rules not fully applied: " + state_.error);
    }
    return state_;
}

void TrafficRules::reassert()
{
    if (!state_.active) {
        return;
    }
    for (const auto& fam : kFamilies) {
        if (!last_qos_.empty()) ensure_jump(fam, "mangle", kQosChain);
        if (!last_jail_.empty()) ensure_jump(fam, "filter", kJailChain);
    }
}

void TrafficRules::teardown()
{
    for (const auto& fam : kFamilies) {
        drop_chain(fam, "mangle", kQosChain);
        drop_chain(fam, "filter", kJailChain);
    }
    last_qos_.clear();
    last_jail_.clear();
    state_.active = false;
    state_.boost = 0;
    state_.jail = 0;
}
