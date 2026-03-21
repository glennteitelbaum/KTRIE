// kstrie_test.cpp — comprehensive kstrie test suite
//
// Build: g++ -std=c++23 -O2 -march=x86-64-v4 -o kstrie_test kstrie_test.cpp
// Run:   ./kstrie_test

#include "kstrie.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

using namespace gteitelbaum;

static int pass_count = 0;

#define TEST(name) static void name()
#define RUN(name) do { name(); ++pass_count; printf("  %s: PASS\n", #name); } while(0)

// ============================================================================
// 1. Empty trie
// ============================================================================

TEST(test_empty) {
    kstrie<int> t;
    assert(t.empty());
    assert(t.size() == 0);
    assert(t.find("x") == t.end());
    assert(t.erase("x") == 0);
    assert(t.begin() == t.end());
    assert(t.memory_usage() > 0);  // at least sentinel
}

// ============================================================================
// 2. Single entry
// ============================================================================

TEST(test_single_insert_find_erase) {
    kstrie<int> t;
    assert(t.insert("hello", 42));
    assert(t.size() == 1);
    assert(!t.empty());

    auto it = t.find("hello");
    assert(it != t.end());
    assert(it.key() == "hello");
    assert(it.value() == 42);

    assert(t.find("world") == t.end());

    assert(t.erase("hello") == 1);
    assert(t.empty());
    assert(t.find("hello") == t.end());
}

// ============================================================================
// 3. Empty string key (EOS)
// ============================================================================

TEST(test_empty_key) {
    kstrie<int> t;
    assert(t.insert("", 99));
    assert(t.size() == 1);

    auto it = t.find("");
    assert(it != t.end());
    assert(it.key() == "");
    assert(it.value() == 99);

    // Empty key coexists with non-empty
    t.insert("a", 1);
    t.insert("ab", 2);
    assert(t.size() == 3);
    assert(t.find("").value() == 99);
    assert(t.find("a").value() == 1);

    assert(t.erase("") == 1);
    assert(t.size() == 2);
    assert(t.find("") == t.end());
}

// ============================================================================
// 4. Duplicate insert
// ============================================================================

TEST(test_duplicate_insert) {
    kstrie<int> t;
    assert(t.insert("key", 1));
    assert(!t.insert("key", 2));
    assert(t.find("key").value() == 1);  // first value kept
    assert(t.size() == 1);
}

// ============================================================================
// 5. insert_or_assign
// ============================================================================

TEST(test_insert_or_assign) {
    kstrie<int> t;
    t.insert_or_assign("k", 1);
    assert(t.find("k").value() == 1);
    t.insert_or_assign("k", 2);
    assert(t.find("k").value() == 2);
    assert(t.size() == 1);
}

// ============================================================================
// 6. assign
// ============================================================================

TEST(test_assign) {
    kstrie<int> t;
    assert(!t.assign("missing", 1));  // no-op on miss
    assert(t.empty());

    t.insert("k", 10);
    assert(t.assign("k", 20));
    assert(t.find("k").value() == 20);
}

// ============================================================================
// 7. modify
// ============================================================================

TEST(test_modify_existing) {
    kstrie<int64_t> t;
    t.insert("hello", 10);
    assert(t.modify("hello", [](int64_t& v) { v += 5; }));
    assert(t.find("hello").value() == 15);
}

TEST(test_modify_missing) {
    kstrie<int64_t> t;
    t.insert("a", 1);
    assert(!t.modify("missing", [](int64_t& v) { v++; }));
    assert(t.size() == 1);
}

TEST(test_modify_with_default_insert) {
    kstrie<int64_t> t;
    assert(!t.modify("new", [](int64_t& v) { v *= 2; }, 42));
    assert(t.find("new").value() == 42);  // default inserted, fn not called
}

TEST(test_modify_with_default_update) {
    kstrie<int64_t> t;
    t.insert("k", 10);
    assert(t.modify("k", [](int64_t& v) { v *= 2; }, 0));
    assert(t.find("k").value() == 20);
}

TEST(test_modify_empty_key) {
    kstrie<int64_t> t;
    t.insert("", 100);
    assert(t.modify("", [](int64_t& v) { v++; }));
    assert(t.find("").value() == 101);
}

TEST(test_modify_bulk_accumulate) {
    kstrie<int64_t> counts;
    for (int i = 0; i < 1000; ++i)
        counts.modify("key_" + std::to_string(i % 50),
                       [](int64_t& v) { v++; }, 1);
    assert(counts.size() == 50);
    assert(counts.find("key_0").value() == 20);
}

// ============================================================================
// 8. erase_when
// ============================================================================

TEST(test_erase_when_hit) {
    kstrie<int64_t> t;
    t.insert("a", 10);
    assert(t.erase_when("a", [](const int64_t& v) { return v == 10; }));
    assert(t.empty());
}

TEST(test_erase_when_pred_false) {
    kstrie<int64_t> t;
    t.insert("a", 10);
    assert(!t.erase_when("a", [](const int64_t& v) { return v == 99; }));
    assert(t.size() == 1);
}

TEST(test_erase_when_missing) {
    kstrie<int64_t> t;
    t.insert("a", 1);
    assert(!t.erase_when("missing", [](const int64_t&) { return true; }));
}

TEST(test_erase_when_empty) {
    kstrie<int64_t> t;
    assert(!t.erase_when("x", [](const int64_t&) { return true; }));
}

TEST(test_erase_when_bulk_conditional) {
    kstrie<int64_t> t;
    for (int i = 0; i < 100; ++i)
        t.insert("item_" + std::to_string(i), i);
    for (int i = 0; i < 100; ++i)
        t.erase_when("item_" + std::to_string(i),
                      [](const int64_t& v) { return v % 2 == 0; });
    assert(t.size() == 50);
    assert(t.find("item_0") == t.end());
    assert(t.find("item_1") != t.end());
}

// ============================================================================
// 9. Iterator ordering vs std::map
// ============================================================================

TEST(test_iteration_order) {
    std::vector<std::string> keys = {
        "banana", "apple", "cherry", "date", "elderberry",
        "fig", "grape", "app", "application", "ban"
    };
    kstrie<int> t;
    std::map<std::string, int> ref;
    for (int i = 0; i < (int)keys.size(); ++i) {
        t.insert(keys[i], i);
        ref[keys[i]] = i;
    }

    auto ti = t.begin();
    auto ri = ref.begin();
    while (ti != t.end() && ri != ref.end()) {
        assert(ti.key() == ri->first);
        assert(ti.value() == ri->second);
        ++ti;
        ++ri;
    }
    assert(ti == t.end());
    assert(ri == ref.end());
}

// ============================================================================
// 10. Reverse iteration
// ============================================================================

TEST(test_reverse_iteration) {
    kstrie<int> t;
    t.insert("a", 1);
    t.insert("b", 2);
    t.insert("c", 3);

    std::vector<std::string> keys;
    for (auto it = t.rbegin(); it != t.rend(); ++it)
        keys.push_back((*it).first);
    assert(keys == (std::vector<std::string>{"c", "b", "a"}));
}

// ============================================================================
// 11. Copy / move / swap
// ============================================================================

TEST(test_copy) {
    kstrie<int> t;
    t.insert("a", 1);
    t.insert("b", 2);

    kstrie<int> copy = t;
    assert(copy.size() == 2);
    assert(copy.find("a").value() == 1);

    // Mutate original, copy unaffected
    t.erase("a");
    assert(t.size() == 1);
    assert(copy.size() == 2);
}

TEST(test_move) {
    kstrie<int> t;
    t.insert("x", 42);

    kstrie<int> moved = std::move(t);
    assert(moved.size() == 1);
    assert(moved.find("x").value() == 42);
    assert(t.empty());
}

TEST(test_swap) {
    kstrie<int> a, b;
    a.insert("a", 1);
    b.insert("b", 2);
    b.insert("c", 3);

    swap(a, b);
    assert(a.size() == 2);
    assert(b.size() == 1);
    assert(a.find("b") != a.end());
    assert(b.find("a") != b.end());
}

// ============================================================================
// 12. Range erase
// ============================================================================

TEST(test_range_erase) {
    kstrie<int> t;
    for (int i = 0; i < 10; ++i)
        t.insert(std::string(1, 'a' + i), i);
    assert(t.size() == 10);

    auto first = t.lower_bound("c");
    auto last = t.lower_bound("g");
    t.erase(first, last);  // erase c, d, e, f
    assert(t.size() == 6);
    assert(t.find("b") != t.end());
    assert(t.find("c") == t.end());
    assert(t.find("f") == t.end());
    assert(t.find("g") != t.end());
}

// ============================================================================
// 13. lower_bound / upper_bound / equal_range
// ============================================================================

TEST(test_bounds) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("banana", 2);
    t.insert("cherry", 3);

    auto lb = t.lower_bound("banana");
    assert(lb != t.end());
    assert(lb.key() == "banana");

    auto ub = t.upper_bound("banana");
    assert(ub != t.end());
    assert(ub.key() == "cherry");

    // lower_bound on missing key
    auto lb2 = t.lower_bound("blueberry");
    assert(lb2 != t.end());
    assert(lb2.key() == "cherry");

    auto [eq_first, eq_last] = t.equal_range("banana");
    assert(eq_first.key() == "banana");
    assert(eq_last.key() == "cherry");
}

// ============================================================================
// 14. prefix() iterator range
// ============================================================================

TEST(test_prefix_iterator_range) {
    kstrie<int> t;
    t.insert("app", 1);
    t.insert("apple", 2);
    t.insert("application", 3);
    t.insert("banana", 4);

    auto [first, last] = t.prefix("app");
    std::vector<std::string> keys;
    for (auto it = first; it != last; ++it)
        keys.push_back(it.key());
    assert(keys.size() == 3);
    assert(keys[0] == "app");
    assert(keys[1] == "apple");
    assert(keys[2] == "application");
}

// ============================================================================
// 15. prefix_count
// ============================================================================

TEST(test_prefix_count) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);
    t.insert("band", 5);
    t.insert("ban", 6);

    assert(t.prefix_count("app") == 3);
    assert(t.prefix_count("ban") == 3);
    assert(t.prefix_count("x") == 0);
    assert(t.prefix_count("") == 6);
    assert(t.prefix_count("apple") == 1);
    assert(t.prefix_count("apples") == 0);
    assert(t.prefix_count("b") == 3);
    assert(t.prefix_count("a") == 3);
}

// ============================================================================
// 16. prefix_walk
// ============================================================================

TEST(test_prefix_walk) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);

    int count = 0;
    t.prefix_walk("app", [&](std::string_view k, const int&) {
        assert(k.starts_with("app"));
        ++count;
    });
    assert(count == 3);

    // Empty prefix walks all
    count = 0;
    t.prefix_walk("", [&](std::string_view, const int&) { ++count; });
    assert(count == 4);
}

// ============================================================================
// 17. prefix_vector
// ============================================================================

TEST(test_prefix_vector) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);

    auto v = t.prefix_vector("app");
    assert(v.size() == 3);
    assert(v[0].first == "app" && v[0].second == 2);
    assert(v[1].first == "apple" && v[1].second == 1);
    assert(v[2].first == "application" && v[2].second == 3);

    assert(t.prefix_vector("xyz").empty());
    assert(t.prefix_vector("").size() == 4);
}

// ============================================================================
// 18. prefix_copy
// ============================================================================

TEST(test_prefix_copy) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);
    t.insert("band", 5);
    t.insert("ban", 6);

    auto sub = t.prefix_copy("ban");
    assert(sub.size() == 3);
    assert(t.size() == 6);  // source unchanged
    assert(sub.find("banana") != sub.end());
    assert(sub.find("band") != sub.end());
    assert(sub.find("ban") != sub.end());
    assert(sub.find("apple") == sub.end());
}

TEST(test_prefix_copy_independent) {
    kstrie<int> t;
    t.insert("a/1", 1);
    t.insert("a/2", 2);
    t.insert("b/1", 3);

    auto sub = t.prefix_copy("a/");
    t.prefix_erase("a/");
    // sub still has its data
    assert(sub.size() == 2);
    assert(sub.find("a/1").value() == 1);
}

// ============================================================================
// 19. prefix_erase
// ============================================================================

TEST(test_prefix_erase) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);
    t.insert("band", 5);
    t.insert("ban", 6);

    size_t n = t.prefix_erase("ban");
    assert(n == 3);
    assert(t.size() == 3);
    assert(t.find("banana") == t.end());
    assert(t.find("apple") != t.end());
}

TEST(test_prefix_erase_no_match) {
    kstrie<int> t;
    t.insert("a", 1);
    assert(t.prefix_erase("xyz") == 0);
    assert(t.size() == 1);
}

TEST(test_prefix_erase_all) {
    kstrie<int> t;
    for (int i = 0; i < 100; ++i)
        t.insert("key_" + std::to_string(i), i);
    assert(t.prefix_erase("") == 100);
    assert(t.empty());
}

TEST(test_prefix_erase_compact_filtered) {
    kstrie<int> t;
    t.insert("abc", 1);
    t.insert("abd", 2);
    t.insert("xyz", 3);
    size_t n = t.prefix_erase("ab");
    assert(n == 2);
    assert(t.size() == 1);
    assert(t.find("xyz") != t.end());
    assert(t.find("abc") == t.end());
}

// ============================================================================
// 20. prefix_split
// ============================================================================

TEST(test_prefix_split) {
    kstrie<int> t;
    t.insert("apple", 1);
    t.insert("app", 2);
    t.insert("application", 3);
    t.insert("banana", 4);
    t.insert("band", 5);
    t.insert("ban", 6);

    auto split = t.prefix_split("app");
    assert(split.size() == 3);
    assert(t.size() == 3);
    assert(split.find("apple") != split.end());
    assert(split.find("app") != split.end());
    assert(split.find("application") != split.end());
    assert(t.find("apple") == t.end());
    assert(t.find("banana") != t.end());
}

TEST(test_prefix_split_no_match) {
    kstrie<int> t;
    t.insert("hello", 1);
    auto sp = t.prefix_split("xyz");
    assert(sp.empty());
    assert(t.size() == 1);
}

TEST(test_prefix_split_all) {
    kstrie<int> t;
    t.insert("a", 1);
    t.insert("b", 2);
    auto sp = t.prefix_split("");
    assert(sp.size() == 2);
    assert(t.empty());
}

TEST(test_prefix_split_compact_filtered) {
    kstrie<int> t;
    t.insert("abc", 1);
    t.insert("abd", 2);
    t.insert("xyz", 3);
    auto sp = t.prefix_split("ab");
    assert(sp.size() == 2);
    assert(t.size() == 1);
    assert(sp.find("abc") != sp.end());
    assert(sp.find("abd") != sp.end());
    assert(t.find("xyz") != t.end());
}

TEST(test_prefix_split_large) {
    kstrie<int> t;
    for (int i = 0; i < 1000; ++i)
        t.insert("item/" + std::to_string(i / 100) + "/" + std::to_string(i), i);

    auto sp = t.prefix_split("item/3/");
    assert(sp.size() == 100);
    assert(t.size() == 900);

    // Verify split contents
    int count = 0;
    sp.prefix_walk("", [&](std::string_view k, const int&) {
        assert(k.starts_with("item/3/"));
        ++count;
    });
    assert(count == 100);

    // Verify source
    assert(t.find("item/5/550") != t.end());
    assert(t.find("item/3/300") == t.end());
}

// ============================================================================
// 21. prefix_exact_key_with_children
// ============================================================================

TEST(test_prefix_exact_key_with_children) {
    kstrie<int> t;
    t.insert("ab", 10);
    t.insert("abc", 20);
    assert(t.prefix_count("ab") == 2);
    auto v = t.prefix_vector("ab");
    assert(v.size() == 2);
    assert(v[0].first == "ab");
    assert(v[1].first == "abc");
}

// ============================================================================
// 22. Prefix on empty trie
// ============================================================================

TEST(test_prefix_on_empty) {
    kstrie<int> t;
    assert(t.prefix_count("x") == 0);
    assert(t.prefix_vector("x").empty());
    assert(t.prefix_erase("x") == 0);
    auto sp = t.prefix_split("x");
    assert(sp.empty());
}

// ============================================================================
// 23. Value types: bool, double, string (heap)
// ============================================================================

TEST(test_value_bool) {
    kstrie<bool> t;
    t.insert("yes", true);
    t.insert("no", false);
    assert(t.find("yes").value() == true);
    assert(t.find("no").value() == false);
    assert(t.size() == 2);
}

TEST(test_value_double) {
    kstrie<double> t;
    t.insert("pi", 3.14159);
    t.insert("e", 2.71828);
    assert(t.find("pi").value() - 3.14159 < 1e-10);
    t.modify("pi", [](double& v) { v *= 2; });
    assert(t.find("pi").value() - 6.28318 < 1e-4);
}

TEST(test_value_string_heap) {
    kstrie<std::string> t;
    t.insert("k1", "hello");
    t.insert("k2", "world");
    assert(t.find("k1").value() == "hello");

    // modify heap type
    t.modify("k1", [](std::string& v) { v += "!"; });
    assert(t.find("k1").value() == "hello!");

    t.erase("k1");
    assert(t.find("k1") == t.end());
    assert(t.size() == 1);
}

TEST(test_value_vector_heap) {
    kstrie<std::vector<int>> t;
    t.insert("k", std::vector<int>{1, 2, 3});
    assert(t.find("k").value() == (std::vector<int>{1, 2, 3}));

    t.modify("k", [](std::vector<int>& v) { v.push_back(4); });
    assert(t.find("k").value() == (std::vector<int>{1, 2, 3, 4}));
}

// ============================================================================
// 24. Large-scale random insert + ordered iteration
// ============================================================================

TEST(test_large_random) {
    constexpr int N = 50000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> len_dist(1, 30);
    std::uniform_int_distribution<int> char_dist('a', 'z');

    kstrie<int> t;
    std::map<std::string, int> ref;

    for (int i = 0; i < N; ++i) {
        int len = len_dist(rng);
        std::string key(len, ' ');
        for (int j = 0; j < len; ++j)
            key[j] = static_cast<char>(char_dist(rng));
        t.insert(key, i);
        ref.emplace(key, i);
    }

    assert(t.size() == ref.size());

    // Verify ordered iteration matches std::map
    auto ti = t.begin();
    auto ri = ref.begin();
    int checked = 0;
    while (ti != t.end() && ri != ref.end()) {
        assert(ti.key() == ri->first);
        assert(ti.value() == ri->second);
        ++ti;
        ++ri;
        ++checked;
    }
    assert(ti == t.end());
    assert(ri == ref.end());
    assert(checked == (int)ref.size());
}

// ============================================================================
// 25. Clear
// ============================================================================

TEST(test_clear) {
    kstrie<int> t;
    for (int i = 0; i < 1000; ++i)
        t.insert("key_" + std::to_string(i), i);
    assert(t.size() == 1000);
    t.clear();
    assert(t.empty());
    assert(t.find("key_0") == t.end());

    // Re-insert after clear
    t.insert("new", 1);
    assert(t.size() == 1);
}

// ============================================================================
// 26. Prefix sharing at collapse boundaries
// ============================================================================

TEST(test_prefix_sharing_collapse) {
    kstrie<int> t;
    // Insert keys that share long prefixes to trigger compact leaf sharing
    for (int i = 0; i < 100; ++i)
        t.insert("shared_prefix_" + std::to_string(i), i);

    assert(t.size() == 100);

    // Erase half — may trigger collapses
    for (int i = 0; i < 50; ++i)
        t.erase("shared_prefix_" + std::to_string(i));

    assert(t.size() == 50);

    // Verify remaining entries are correct
    for (int i = 50; i < 100; ++i) {
        auto it = t.find("shared_prefix_" + std::to_string(i));
        assert(it != t.end());
        assert(it.value() == i);
    }
}

// ============================================================================
// 27. Memory usage decreases on erase
// ============================================================================

TEST(test_memory_decreases) {
    kstrie<int> t;
    for (int i = 0; i < 10000; ++i)
        t.insert("entry_" + std::to_string(i), i);

    size_t mem_full = t.memory_usage();

    for (int i = 0; i < 9000; ++i)
        t.erase("entry_" + std::to_string(i));

    size_t mem_partial = t.memory_usage();
    assert(mem_partial < mem_full);
}

// ============================================================================
// 15. lower_bound / upper_bound / equal_range / count
// ============================================================================

TEST(test_lower_bound) {
    kstrie<int> t;
    t.insert("apple", 1); t.insert("banana", 2); t.insert("cherry", 3);

    auto it = t.lower_bound("banana");
    assert(it != t.end() && it.key() == "banana");

    it = t.lower_bound("b");
    assert(it != t.end() && it.key() == "banana");

    it = t.lower_bound("d");
    assert(it == t.end());

    it = t.lower_bound("");
    assert(it != t.end() && it.key() == "apple");
}

TEST(test_upper_bound) {
    kstrie<int> t;
    t.insert("apple", 1); t.insert("banana", 2); t.insert("cherry", 3);

    auto it = t.upper_bound("banana");
    assert(it != t.end() && it.key() == "cherry");

    it = t.upper_bound("cherry");
    assert(it == t.end());

    it = t.upper_bound("a");
    assert(it != t.end() && it.key() == "apple");
}

TEST(test_equal_range) {
    kstrie<int> t;
    t.insert("apple", 1); t.insert("banana", 2); t.insert("cherry", 3);

    auto [lo, hi] = t.equal_range("banana");
    assert(lo != t.end() && lo.key() == "banana");
    assert(hi != t.end() && hi.key() == "cherry");

    auto [lo2, hi2] = t.equal_range("blueberry");
    assert(lo2 == hi2); // not found: both at insertion point
}

TEST(test_count) {
    kstrie<int> t;
    t.insert("apple", 1); t.insert("banana", 2);
    assert(t.count("apple") == 1);
    assert(t.count("missing") == 0);
    assert(t.count("") == 0);
    t.insert("", 99);
    assert(t.count("") == 1);
}

// ============================================================================
// 16. lower_bound/upper_bound at scale vs std::map
// ============================================================================

TEST(test_bounds_vs_map) {
    kstrie<int> t;
    std::map<std::string, int> ref;
    std::mt19937 rng(42);
    for (int i = 0; i < 5000; i++) {
        int len = 3 + rng() % 10;
        std::string s;
        for (int j = 0; j < len; j++) s += ('a' + rng() % 26);
        t.insert_or_assign(s, i);
        ref[s] = i;
    }
    // Probe with 1000 random keys
    std::mt19937 prng(123);
    for (int i = 0; i < 1000; i++) {
        int len = 3 + prng() % 10;
        std::string probe;
        for (int j = 0; j < len; j++) probe += ('a' + prng() % 26);

        auto ti = t.lower_bound(probe);
        auto ri = ref.lower_bound(probe);
        if (ri == ref.end()) {
            assert(ti == t.end());
        } else {
            assert(ti != t.end());
            assert(ti.key() == ri->first);
        }

        auto tu = t.upper_bound(probe);
        auto ru = ref.upper_bound(probe);
        if (ru == ref.end()) {
            assert(tu == t.end());
        } else {
            assert(tu != t.end());
            assert(tu.key() == ru->first);
        }
    }
}

// ============================================================================
// 17. Binary keys (null bytes, high bytes)
// ============================================================================

TEST(test_binary_keys) {
    kstrie<int> t;
    std::string k1{'\0', '\0', '\x01'};
    std::string k2{'\0', '\x01', '\x00'};
    std::string k3{'\xff', '\xfe', '\xfd'};
    std::string k4{'\x80'};

    t.insert(k1, 1); t.insert(k2, 2); t.insert(k3, 3); t.insert(k4, 4);
    assert(t.size() == 4);
    assert(t.find(k1).value() == 1);
    assert(t.find(k2).value() == 2);
    assert(t.find(k3).value() == 3);
    assert(t.find(k4).value() == 4);

    // Verify order: k1 < k2 < k4 < k3 (byte lexicographic)
    auto it = t.begin();
    assert(it.key() == k1); ++it;
    assert(it.key() == k2); ++it;
    assert(it.key() == k4); ++it;
    assert(it.key() == k3); ++it;
    assert(it == t.end());

    assert(t.erase(k2) == 1);
    assert(t.size() == 3);
    assert(t.find(k2) == t.end());
}

// ============================================================================
// 18. Long keys (stress keysuffix / skip handling)
// ============================================================================

TEST(test_long_keys) {
    kstrie<int> t;
    // Keys with 200-byte shared prefix, diverging at the end
    std::string prefix(200, 'x');
    for (int i = 0; i < 100; i++) {
        std::string k = prefix + std::string(1, static_cast<char>(i));
        t.insert(k, i);
    }
    assert(t.size() == 100);
    for (int i = 0; i < 100; i++) {
        std::string k = prefix + std::string(1, static_cast<char>(i));
        auto it = t.find(k);
        assert(it != t.end() && it.value() == i);
    }
    // Iteration order
    auto it = t.begin();
    for (int i = 0; i < 100; i++) {
        assert(it != t.end());
        assert(it.value() == i);
        ++it;
    }
    assert(it == t.end());
}

TEST(test_very_long_keys) {
    kstrie<int> t;
    // Single key of 10000 bytes
    std::string k(10000, 'a');
    t.insert(k, 42);
    assert(t.size() == 1);
    assert(t.find(k).value() == 42);
    // Slightly different key
    std::string k2 = k; k2[9999] = 'b';
    t.insert(k2, 99);
    assert(t.size() == 2);
    assert(t.find(k).value() == 42);
    assert(t.find(k2).value() == 99);
}

// ============================================================================
// 19. Interleaved insert/erase stress
// ============================================================================

TEST(test_interleaved_insert_erase) {
    kstrie<int> t;
    std::map<std::string, int> ref;
    std::mt19937 rng(777);

    for (int round = 0; round < 10; round++) {
        // Insert 1000
        for (int i = 0; i < 1000; i++) {
            int len = 1 + rng() % 15;
            std::string s;
            for (int j = 0; j < len; j++) s += ('a' + rng() % 5); // small alphabet = lots of sharing
            t.insert_or_assign(s, round * 1000 + i);
            ref[s] = round * 1000 + i;
        }
        // Erase ~half
        std::vector<std::string> keys;
        for (auto& [k, v] : ref) keys.push_back(k);
        for (auto& k : keys) {
            if (rng() % 2 == 0) { t.erase(k); ref.erase(k); }
        }
        assert(t.size() == ref.size());
    }
    // Verify all remaining entries
    auto ti = t.begin();
    auto ri = ref.begin();
    while (ti != t.end() && ri != ref.end()) {
        assert(ti.key() == ri->first);
        assert(ti.value() == ri->second);
        ++ti; ++ri;
    }
    assert(ti == t.end() && ri == ref.end());
}

// ============================================================================
// 20. Keysuffix sharing integrity after mutation
// ============================================================================

TEST(test_sharing_integrity) {
    kstrie<int> t;
    // Insert keys that form sharing chains
    t.insert("abc", 1);
    t.insert("abcd", 2);
    t.insert("abcde", 3);
    t.insert("abcdef", 4);
    t.insert("abd", 5);
    assert(t.size() == 5);

    // Verify all readable
    assert(t.find("abc").value() == 1);
    assert(t.find("abcd").value() == 2);
    assert(t.find("abcde").value() == 3);
    assert(t.find("abcdef").value() == 4);
    assert(t.find("abd").value() == 5);

    // Erase middle of chain
    t.erase("abcd");
    assert(t.size() == 4);
    assert(t.find("abcd") == t.end());
    // Others still correct
    assert(t.find("abc").value() == 1);
    assert(t.find("abcde").value() == 3);
    assert(t.find("abcdef").value() == 4);
    assert(t.find("abd").value() == 5);

    // Insert back
    t.insert("abcd", 22);
    assert(t.find("abcd").value() == 22);
    assert(t.size() == 5);
}

// ============================================================================
// 21. Prefix ops with empty key present
// ============================================================================

TEST(test_prefix_with_empty_key) {
    kstrie<int> t;
    t.insert("", 0);
    t.insert("a", 1);
    t.insert("ab", 2);
    t.insert("abc", 3);
    t.insert("b", 4);

    assert(t.prefix_count("") == 5);  // everything
    assert(t.prefix_count("a") == 3); // a, ab, abc
    assert(t.prefix_count("ab") == 2); // ab, abc
    assert(t.prefix_count("abc") == 1);
    assert(t.prefix_count("b") == 1);

    // Walk with empty prefix = all entries
    std::vector<std::string> walked;
    t.prefix_walk("", [&](std::string_view k, const int&) { walked.push_back(std::string(k)); });
    assert(walked.size() == 5);
    assert(walked[0] == "");
    assert(walked[1] == "a");
}

TEST(test_prefix_erase_with_empty_key) {
    kstrie<int> t;
    t.insert("", 0);
    t.insert("a", 1);
    t.insert("ab", 2);

    auto n = t.prefix_erase("a");
    assert(n == 2); // erases "a" and "ab"
    assert(t.size() == 1);
    assert(t.find("").value() == 0);
}

// ============================================================================
// main
// ============================================================================

int main() {
    printf("=== kstrie test suite ===\n\n");

    printf("Basic:\n");
    RUN(test_empty);
    RUN(test_single_insert_find_erase);
    RUN(test_empty_key);
    RUN(test_duplicate_insert);
    RUN(test_insert_or_assign);
    RUN(test_assign);

    printf("\nModify:\n");
    RUN(test_modify_existing);
    RUN(test_modify_missing);
    RUN(test_modify_with_default_insert);
    RUN(test_modify_with_default_update);
    RUN(test_modify_empty_key);
    RUN(test_modify_bulk_accumulate);

    printf("\nErase_when:\n");
    RUN(test_erase_when_hit);
    RUN(test_erase_when_pred_false);
    RUN(test_erase_when_missing);
    RUN(test_erase_when_empty);
    RUN(test_erase_when_bulk_conditional);

    printf("\nIteration:\n");
    RUN(test_iteration_order);
    RUN(test_reverse_iteration);

    printf("\nCopy/Move/Swap:\n");
    RUN(test_copy);
    RUN(test_move);
    RUN(test_swap);

    printf("\nRange/Bounds:\n");
    RUN(test_range_erase);
    RUN(test_bounds);

    printf("\nPrefix (iterator):\n");
    RUN(test_prefix_iterator_range);

    printf("\nPrefix (bulk):\n");
    RUN(test_prefix_count);
    RUN(test_prefix_walk);
    RUN(test_prefix_vector);
    RUN(test_prefix_copy);
    RUN(test_prefix_copy_independent);
    RUN(test_prefix_erase);
    RUN(test_prefix_erase_no_match);
    RUN(test_prefix_erase_all);
    RUN(test_prefix_erase_compact_filtered);
    RUN(test_prefix_split);
    RUN(test_prefix_split_no_match);
    RUN(test_prefix_split_all);
    RUN(test_prefix_split_compact_filtered);
    RUN(test_prefix_split_large);
    RUN(test_prefix_exact_key_with_children);
    RUN(test_prefix_on_empty);

    printf("\nValue types:\n");
    RUN(test_value_bool);
    RUN(test_value_double);
    RUN(test_value_string_heap);
    RUN(test_value_vector_heap);

    printf("\nScale:\n");
    RUN(test_large_random);
    RUN(test_clear);
    RUN(test_prefix_sharing_collapse);
    RUN(test_memory_decreases);

    printf("\nBounds/Count:\n");
    RUN(test_lower_bound);
    RUN(test_upper_bound);
    RUN(test_equal_range);
    RUN(test_count);
    RUN(test_bounds_vs_map);

    printf("\nBinary keys:\n");
    RUN(test_binary_keys);

    printf("\nLong keys:\n");
    RUN(test_long_keys);
    RUN(test_very_long_keys);

    printf("\nStress:\n");
    RUN(test_interleaved_insert_erase);

    printf("\nSharing integrity:\n");
    RUN(test_sharing_integrity);

    printf("\nPrefix with empty key:\n");
    RUN(test_prefix_with_empty_key);
    RUN(test_prefix_erase_with_empty_key);

    printf("\n=== ALL %d TESTS PASSED ===\n", pass_count);
}
