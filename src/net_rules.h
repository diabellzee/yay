#pragma once

// Per-app traffic rules (Boost = DSCP/MARK, Jail = REJECT) in dedicated
// iptables chains, replaced ATOMICALLY with iptables-restore.
//
// PingPimp inserted one `-I OUTPUT -m owner ...` rule per app with plain
// iptables, on every engine pass, and never removed the previous ones — so
// the OUTPUT chain grew by one rule per boosted app per network event, and
// every packet had to walk the pile. Here the rules live in YAY_QOS /
// YAY_JAIL, referenced by exactly one jump each, and "apply" means "replace
// the chain's whole content in one transaction".

#include <string>
#include <vector>

struct RulesState {
    std::size_t boost = 0;
    std::size_t jail = 0;
    bool dscp = true;   // false: kernel has no DSCP target, MARK fallback in use
    bool active = false;
    std::string error;  // last failure, "" if none
};

class TrafficRules {
public:
    // Replace both chains. An app in both lists is jailed, not boosted.
    // Empty lists remove everything this class ever installed.
    RulesState apply(const std::vector<unsigned>& boost_uids,
                     const std::vector<unsigned>& jail_uids);

    // Re-assert the jump rules (netd can rebuild OUTPUT and drop ours).
    void reassert();

    // Remove chains + jumps. Safe to call when nothing is installed.
    void teardown();

    [[nodiscard]] const RulesState& state() const noexcept { return state_; }

private:
    RulesState state_;
    std::string last_qos_;
    std::string last_jail_;
};
