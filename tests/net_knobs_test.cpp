// Host-side tests for KnobStore / Stash against a throw-away directory tree.
//
//   g++ -std=c++20 -I src -I <nlohmann include dir> tests/net_knobs_test.cpp src/net_knobs.cpp src/json_config.cpp src/logger.cpp -o /tmp/net_knobs_test

#include "net_knobs.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
int g_failed = 0, g_total = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_total;                                                               \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

fs::path g_root;
void put(const char* rel, const std::string& v)
{
    const fs::path p = g_root / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << v;
}
std::string get(const char* rel)
{
    std::ifstream f(g_root / rel);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    while (!s.empty() && (s.back() == '\n')) s.pop_back();
    return s;
}

void test_basic()
{
    put("proc/sys/net/core/rmem_max", "212992\n");
    put("proc/sys/net/ipv4/tcp_rmem", "4096\t87380\t6291456\n");     // sysctl prints tabs
    const std::string snap = (g_root / "snap.json").string();

    KnobStore k(g_root.string(), snap, "boot-A");
    CHECK(normalize_ws("  4096\t87380 \n 6291456 ") == "4096 87380 6291456");

    CHECK(k.set("/proc/sys/net/core/rmem_max", "1048576", "profile") == KnobResult::Applied);
    CHECK(get("proc/sys/net/core/rmem_max") == "1048576");
    CHECK(k.set("/proc/sys/net/core/rmem_max", "1048576", "profile") == KnobResult::Unchanged);
    CHECK(k.set("/proc/sys/net/ipv4/tcp_rmem", "4096 87380 6291456", "profile") == KnobResult::Unchanged); // tabs vs spaces
    CHECK(k.set("/proc/sys/net/does/not/exist", "1", "profile") == KnobResult::Missing);
    CHECK(k.changed_count() == 1U);

    // a second change to the SAME path must not overwrite the first snapshot
    CHECK(k.set("/proc/sys/net/core/rmem_max", "2097152", "profile") == KnobResult::Applied);
    CHECK(k.restore("profile") == 1U);
    CHECK(get("proc/sys/net/core/rmem_max") == "212992");            // the DEVICE's value, not an intermediate
    CHECK(k.changed_count() == 0U);
}

void test_owners_and_persistence()
{
    put("proc/sys/net/core/rmem_max", "212992");
    put("proc/sys/net/ipv6/conf/all/disable_ipv6", "0");
    put("proc/irq/45/smp_affinity", "ff");
    put("proc/irq/46/smp_affinity", "7f");                            // a non-default stock mask
    const std::string snap = (g_root / "snap2.json").string();

    {
        KnobStore k(g_root.string(), snap, "boot-A");
        CHECK(k.set("/proc/sys/net/core/rmem_max", "1", "profile") == KnobResult::Applied);
        CHECK(k.set("/proc/sys/net/ipv6/conf/all/disable_ipv6", "1", "ipv6") == KnobResult::Applied);
        CHECK(k.set("/proc/irq/45/smp_affinity", "c0", "irq") == KnobResult::Applied);
        CHECK(k.set("/proc/irq/46/smp_affinity", "c0", "irq") == KnobResult::Applied);
        k.flush();
        CHECK(k.changed_count("irq") == 2U && k.changed_count("ipv6") == 1U);

        // reverting one owner leaves the others alone
        CHECK(k.restore("ipv6") == 1U);
        CHECK(get("proc/sys/net/ipv6/conf/all/disable_ipv6") == "0");
        CHECK(get("proc/sys/net/core/rmem_max") == "1");
        k.flush();
    }

    // "daemon restarted in the same boot": snapshot survives, restore still exact
    {
        KnobStore k(g_root.string(), snap, "boot-A");
        CHECK(k.changed_count() == 3U);
        CHECK(k.restore_all() == 3U);
        CHECK(get("proc/irq/45/smp_affinity") == "ff");
        CHECK(get("proc/irq/46/smp_affinity") == "7f");               // NOT blanket "ff" like PingPimp's unirq
        CHECK(get("proc/sys/net/core/rmem_max") == "212992");
        k.flush();
    }

    // reboot: the kernel already reset everything; a stale snapshot must be ignored
    put("proc/sys/net/core/rmem_max", "212992");
    {
        KnobStore k(g_root.string(), snap, "boot-A");
        CHECK(k.set("/proc/sys/net/core/rmem_max", "5", "profile") == KnobResult::Applied);
        k.flush();
    }
    put("proc/sys/net/core/rmem_max", "212992");                      // "reboot" resets the knob
    {
        KnobStore k(g_root.string(), snap, "boot-B");
        CHECK(k.changed_count() == 0U);                               // boot-A snapshot discarded
    }
}

void test_stash()
{
    const std::string path = (g_root / "stash.json").string();
    {
        Stash s(path);
        CHECK(!s.has("k"));
        s.put("k", "null");
        s.put("n", "1");
        CHECK(s.get("k") == "null");
    }
    {
        Stash s(path);                       // survives a restart (and a reboot: no boot gating)
        CHECK(s.has("k") && s.get("n") == "1");
        s.erase("k");
        CHECK(!s.has("k"));
    }
    CHECK(!Stash(path).has("k") && Stash(path).has("n"));
}
} // namespace

int main()
{
    g_root = fs::temp_directory_path() / ("yay-knobs-" + std::to_string(::getpid()));
    fs::create_directories(g_root);
    test_basic(); test_owners_and_persistence(); test_stash();
    fs::remove_all(g_root);
    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
