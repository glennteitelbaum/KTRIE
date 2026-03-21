#include "kntrie.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>
#include <algorithm>
#include <cinttypes>

using namespace gteitelbaum;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        std::printf("  FAIL [%s:%d]: ", __FILE__, __LINE__); \
        std::printf(__VA_ARGS__); \
        std::printf("\n"); \
        ++g_fail; \
        return false; \
    } \
} while(0)

#define PASS(name) do { ++g_pass; std::printf("  PASS: %s\n", name); } while(0)

// ======================================================================
// test_forward: begin->end, ascending order + correct count
// ======================================================================

template<typename KEY>
bool test_forward(kntrie<KEY, int>& t, size_t expected, const char* label) {
    std::printf("    [fwd] %s ...", label); fflush(stdout);
    size_t count = 0;
    KEY prev{};
    bool first = true;

    for (auto it = t.begin(); it != t.end(); ++it) {
        auto [k, v] = *it;
        if (!first) {
            CHECK(k > prev, "%s: not ascending at %zu: prev=%lld cur=%lld",
                  label, count, (long long)prev, (long long)k);
        }
        prev = k;
        first = false;
        ++count;
        if (count > expected + 10) {
            std::printf(" HUNG (count exceeded expected+10)\n");
            CHECK(false, "%s: forward iteration stuck, count=%zu expected=%zu",
                  label, count, expected);
        }
    }
    CHECK(count == expected, "%s: count %zu != expected %zu", label, count, expected);
    std::printf(" ok (%zu)\n", count);
    PASS(label);
    return true;
}

// ======================================================================
// test_backward: rbegin->rend, descending order + correct count
// ======================================================================

template<typename KEY>
bool test_backward(kntrie<KEY, int>& t, size_t expected, const char* label) {
    std::printf("    [bwd] %s ...", label); fflush(stdout);
    size_t count = 0;
    KEY prev{};
    bool first = true;

    for (auto it = t.rbegin(); it != t.rend(); ++it) {
        auto [k, v] = *it;
        if (!first) {
            CHECK(k < prev, "%s: not descending at %zu: prev=%lld cur=%lld",
                  label, count, (long long)prev, (long long)k);
        }
        prev = k;
        first = false;
        ++count;
        if (count > expected + 10) {
            std::printf(" HUNG (count exceeded expected+10)\n");
            CHECK(false, "%s: backward iteration stuck, count=%zu expected=%zu",
                  label, count, expected);
        }
    }
    CHECK(count == expected, "%s: count %zu != expected %zu", label, count, expected);
    std::printf(" ok (%zu)\n", count);
    PASS(label);
    return true;
}

// ======================================================================
// test_fwd_bwd_match: forward and backward produce same key set
// ======================================================================

template<typename KEY>
bool test_fwd_bwd_match(kntrie<KEY, int>& t, const char* label) {
    std::printf("    [match] %s ...", label); fflush(stdout);
    std::vector<KEY> fwd, bwd;

    for (auto it = t.begin(); it != t.end(); ++it)
        fwd.push_back((*it).first);
    for (auto it = t.rbegin(); it != t.rend(); ++it)
        bwd.push_back((*it).first);

    std::reverse(bwd.begin(), bwd.end());

    CHECK(fwd.size() == bwd.size(), "%s: sizes differ fwd=%zu bwd=%zu",
          label, fwd.size(), bwd.size());
    for (size_t i = 0; i < fwd.size(); ++i) {
        CHECK(fwd[i] == bwd[i], "%s: mismatch at %zu: fwd=%lld bwd=%lld",
              label, i, (long long)fwd[i], (long long)bwd[i]);
    }
    std::printf(" ok\n");
    PASS(label);
    return true;
}

// ======================================================================
// test_find: every inserted key findable with correct value
// ======================================================================

template<typename KEY>
bool test_find(kntrie<KEY, int>& t, const std::vector<KEY>& keys, const char* label) {
    std::printf("    [find] %s ...", label); fflush(stdout);
    for (auto k : keys) {
        CHECK(t.contains(k), "%s: key %lld not found via contains", label, (long long)k);
        auto it = t.find(k);
        CHECK(it != t.end(), "%s: find returned end for %lld", label, (long long)k);
    }
    std::printf(" ok (%zu keys)\n", keys.size());
    PASS(label);
    return true;
}

// ======================================================================
// test_erase: erase half, verify removed + remaining intact
// ======================================================================

template<typename KEY>
bool test_erase(kntrie<KEY, int>& t, std::vector<KEY>& unique_keys, const char* label) {
    std::printf("    [erase] %s ...", label); fflush(stdout);

    size_t orig = t.size();
    size_t half = orig / 2;

    std::vector<KEY> erased(unique_keys.begin(), unique_keys.begin() + half);
    std::vector<KEY> remaining(unique_keys.begin() + half, unique_keys.end());

    for (auto k : erased) {
        size_t r = t.erase(k);
        CHECK(r == 1, "%s: erase(%lld) returned %zu", label, (long long)k, r);
    }

    CHECK(t.size() == orig - half,
          "%s: size after erase %zu != expected %zu", label, t.size(), orig - half);

    for (auto k : erased)
        CHECK(!t.contains(k), "%s: erased key %lld still found", label, (long long)k);

    for (auto k : remaining)
        CHECK(t.contains(k), "%s: surviving key %lld missing", label, (long long)k);

    std::printf(" ok (erased %zu, left %zu)\n", half, remaining.size());
    PASS(label);
    return true;
}

// ======================================================================
// Data generators
// ======================================================================

template<typename KEY>
std::vector<KEY> make_sequential(size_t n) {
    std::vector<KEY> v;
    v.reserve(n);
    KEY start;
    if constexpr (std::is_signed_v<KEY>)
        start = static_cast<KEY>(-(long long)(n / 2));
    else
        start = KEY(0);
    for (size_t i = 0; i < n; ++i)
        v.push_back(static_cast<KEY>(start + (KEY)i));
    return v;
}

template<typename KEY>
std::vector<KEY> make_random(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<KEY> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back(static_cast<KEY>(rng()));
    return v;
}

// ======================================================================
// Run full suite for a key type + data pattern
// ======================================================================

template<typename KEY>
void run_suite(std::vector<KEY> keys, const char* type_name,
               const char* pattern_name) {
    char lbl[256];
    std::snprintf(lbl, sizeof(lbl), "%s/%s/n=%zu", type_name, pattern_name, keys.size());
    std::printf("\n  == %s ==\n", lbl);

    kntrie<KEY, int> t;

    // Insert
    std::printf("    [insert] %zu keys ...", keys.size()); fflush(stdout);
    for (auto k : keys)
        t.insert(k, static_cast<int>(k & 0x7FFFFFFF));
    std::printf(" done\n");

    // Deduplicate for expected size
    std::set<KEY> uniq_set(keys.begin(), keys.end());
    std::vector<KEY> unique_keys(uniq_set.begin(), uniq_set.end());
    size_t expected = unique_keys.size();

    std::printf("    size=%zu (unique=%zu)\n", t.size(), expected);
    if (t.size() != expected) {
        std::printf("  FAIL: size mismatch\n");
        ++g_fail;
        return;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s/%s/n=%zu", type_name, pattern_name, keys.size());

    test_find(t, unique_keys, buf);
    test_forward(t, expected, buf);
    test_backward(t, expected, buf);
    test_fwd_bwd_match(t, buf);

    // Erase half, then re-test iteration
    test_erase(t, unique_keys, buf);

    size_t post_erase = t.size();
    char buf2[256];
    std::snprintf(buf2, sizeof(buf2), "%s/%s/n=%zu post-erase", type_name, pattern_name, keys.size());
    test_forward(t, post_erase, buf2);
    test_backward(t, post_erase, buf2);
    test_fwd_bwd_match(t, buf2);
}

// ======================================================================
// Edge cases
// ======================================================================

template<typename KEY>
void test_edge_cases(const char* type_name) {
    char buf[256];
    std::printf("\n  == %s edge cases ==\n", type_name);

    // Empty
    {
        kntrie<KEY, int> t;
        std::printf("    [empty] fwd ..."); fflush(stdout);
        bool ok = (t.begin() == t.end());
        std::printf(ok ? " ok\n" : " FAIL\n");
        if (!ok) ++g_fail; else ++g_pass;

        std::printf("    [empty] bwd ..."); fflush(stdout);
        ok = (t.rbegin() == t.rend());
        std::printf(ok ? " ok\n" : " FAIL\n");
        if (!ok) ++g_fail; else ++g_pass;
    }

    // Single
    {
        kntrie<KEY, int> t;
        t.insert(KEY(42), 99);
        std::snprintf(buf, sizeof(buf), "%s/single", type_name);
        test_forward(t, 1, buf);
        test_backward(t, 1, buf);
    }

    // Min + Max
    {
        kntrie<KEY, int> t;
        t.insert(std::numeric_limits<KEY>::min(), 1);
        t.insert(std::numeric_limits<KEY>::max(), 2);
        std::snprintf(buf, sizeof(buf), "%s/min_max", type_name);
        test_forward(t, 2, buf);
        test_backward(t, 2, buf);
        test_fwd_bwd_match(t, buf);
    }

    // Insert + erase all -> empty
    {
        kntrie<KEY, int> t;
        std::vector<KEY> ks = {KEY(10), KEY(20), KEY(30), KEY(40), KEY(50)};
        for (auto k : ks) t.insert(k, (int)k);
        for (auto k : ks) t.erase(k);
        std::printf("    [erase_all] ..."); fflush(stdout);
        bool ok = (t.size() == 0 && t.begin() == t.end());
        std::printf(ok ? " ok\n" : " FAIL (size=%zu)\n", t.size());
        if (!ok) ++g_fail; else ++g_pass;
    }
}

// ======================================================================
// Main
// ======================================================================

int main() {
    // Sizes: below COMPACT_MAX(4096), at boundary, above
    const size_t sizes[] = {100, 1000, 5000, 10000};

    std::printf("=== kntrie sanity tests ===\n");
    std::printf("COMPACT_MAX = 4096\n");

    // ---- uint16_t ----
    std::printf("\n--- uint16_t ---\n");
    test_edge_cases<uint16_t>("u16");
    for (auto n : sizes) {
        run_suite(make_sequential<uint16_t>(n), "u16", "seq");
        run_suite(make_random<uint16_t>(n, 1000 + n), "u16", "rnd");
    }

    // ---- int32_t ----
    std::printf("\n--- int32_t ---\n");
    test_edge_cases<int32_t>("i32");
    for (auto n : sizes) {
        run_suite(make_sequential<int32_t>(n), "i32", "seq");
        run_suite(make_random<int32_t>(n, 2000 + n), "i32", "rnd");
    }

    // ---- uint64_t ----
    std::printf("\n--- uint64_t ---\n");
    test_edge_cases<uint64_t>("u64");
    for (auto n : sizes) {
        run_suite(make_sequential<uint64_t>(n), "u64", "seq");
        run_suite(make_random<uint64_t>(n, 3000 + n), "u64", "rnd");
    }

    // ---- Summary ----
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
