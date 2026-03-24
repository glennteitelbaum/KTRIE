#include "kstrie.hpp"
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

using namespace gteitelbaum;

int main() {
    // Test 1: Forward iteration order
    {
        kstrie<int64_t> t;
        std::vector<std::string> keys = {"foo", "bar", "baz", "abc", "xyz", "hello", "world"};
        for (size_t i = 0; i < keys.size(); ++i)
            t.insert(keys[i], static_cast<int64_t>(i));

        std::sort(keys.begin(), keys.end());
        std::vector<std::string> got;
        for (auto it = t.begin(); it != t.end(); ++it)
            got.push_back((*it).first);
        assert(got == keys);
        std::printf("test1 fwd order: OK\n");
    }

    // Test 2: Reverse iteration
    {
        kstrie<int64_t> t;
        std::vector<std::string> keys = {"a", "b", "c", "d", "e"};
        for (auto& k : keys) t.insert(k, 0);

        std::vector<std::string> rev;
        auto it = t.end();
        while (it != t.begin()) {
            --it;
            rev.push_back((*it).first);
        }
        std::vector<std::string> expected = {"e", "d", "c", "b", "a"};
        assert(rev == expected);
        std::printf("test2 reverse: OK\n");
    }

    // Test 3: EOS key (empty string)
    {
        kstrie<int64_t> t;
        t.insert("", 0);
        t.insert("a", 1);
        t.insert("ab", 2);
        t.insert("b", 3);
        
        std::vector<std::string> got;
        for (auto it = t.begin(); it != t.end(); ++it)
            got.push_back((*it).first);
        std::vector<std::string> expected = {"", "a", "ab", "b"};
        assert(got == expected);
        std::printf("test3 EOS: OK\n");
    }

    // Test 4: find() returns correct iterator
    {
        kstrie<int64_t> t;
        t.insert("alpha", 1);
        t.insert("beta", 2);
        t.insert("gamma", 3);

        auto it = t.find("beta");
        assert(it != t.end());
        assert((*it).first == "beta");
        assert((*it).second == 2);

        ++it;
        assert(it != t.end());
        assert((*it).first == "gamma");
        assert((*it).second == 3);

        ++it;
        assert(it == t.end());

        assert(t.find("missing") == t.end());
        std::printf("test4 find: OK\n");
    }

    // Test 5: lower_bound
    {
        kstrie<int64_t> t;
        t.insert("b", 1);
        t.insert("d", 2);
        t.insert("f", 3);

        auto it = t.lower_bound("a");
        assert(it != t.end());
        assert((*it).first == "b");

        it = t.lower_bound("b");
        assert((*it).first == "b");

        it = t.lower_bound("c");
        assert((*it).first == "d");

        it = t.lower_bound("g");
        assert(it == t.end());
        std::printf("test5 lower_bound: OK\n");
    }

    // Test 6: upper_bound
    {
        kstrie<int64_t> t;
        t.insert("b", 1);
        t.insert("d", 2);
        t.insert("f", 3);

        auto it = t.upper_bound("b");
        assert(it != t.end());
        assert((*it).first == "d");

        it = t.upper_bound("e");
        assert((*it).first == "f");

        it = t.upper_bound("f");
        assert(it == t.end());
        std::printf("test6 upper_bound: OK\n");
    }

    // Test 7: Large shuffled insert + iteration
    {
        kstrie<int64_t> t;
        std::vector<std::string> keys;
        for (int i = 0; i < 5000; ++i)
            keys.push_back("key" + std::to_string(i));
        std::mt19937 rng(42);
        std::shuffle(keys.begin(), keys.end(), rng);
        for (size_t i = 0; i < keys.size(); ++i)
            t.insert(keys[i], static_cast<int64_t>(i));

        std::sort(keys.begin(), keys.end());
        size_t count = 0;
        std::string prev;
        for (auto it = t.begin(); it != t.end(); ++it) {
            auto [k, v] = *it;
            if (count > 0) assert(k > prev);
            assert(k == keys[count]);
            prev = k;
            count++;
        }
        assert(count == 5000);
        std::printf("test7 large iter: OK\n");
    }

    // Test 8: Copy and iterate both
    {
        kstrie<int64_t> t;
        t.insert("x", 1);
        t.insert("y", 2);
        t.insert("z", 3);
        
        kstrie<int64_t> t2 = t;
        std::vector<std::string> k1, k2;
        for (auto it = t.begin(); it != t.end(); ++it) k1.push_back((*it).first);
        for (auto it = t2.begin(); it != t2.end(); ++it) k2.push_back((*it).first);
        assert(k1 == k2);
        std::printf("test8 copy: OK\n");
    }

    // Test 9: Bool specialization iteration
    {
        kstrie<bool> t;
        t.insert("a", true);
        t.insert("b", false);
        t.insert("c", true);
        
        std::vector<std::pair<std::string, bool>> got;
        for (auto it = t.begin(); it != t.end(); ++it) {
            auto [k, v] = *it;
            got.push_back({std::string(k), v});
        }
        assert(got.size() == 3);
        assert(got[0].first == "a" && got[0].second == true);
        assert(got[1].first == "b" && got[1].second == false);
        assert(got[2].first == "c" && got[2].second == true);
        std::printf("test9 bool: OK\n");
    }

    // Test 10: Shared prefix keys
    {
        kstrie<int64_t> t;
        t.insert("/api/v1/users", 1);
        t.insert("/api/v1/users/123", 2);
        t.insert("/api/v1/users/456", 3);
        t.insert("/api/v2/items", 4);
        t.insert("/api/v2/items/789", 5);
        
        std::vector<std::string> got;
        for (auto it = t.begin(); it != t.end(); ++it)
            got.push_back((*it).first);
        
        std::vector<std::string> expected = {
            "/api/v1/users", "/api/v1/users/123", "/api/v1/users/456",
            "/api/v2/items", "/api/v2/items/789"
        };
        assert(got == expected);
        std::printf("test10 shared prefix: OK\n");
    }

    std::printf("\nALL ITERATOR TESTS OK\n");
}

// Test 11: insert returns pair<iterator, bool>
void test_insert_pair() {
    kstrie<int64_t> t;
    auto [it1, ins1] = t.insert("hello", 42);
    assert(ins1 == true);
    assert((*it1).first == "hello");
    assert((*it1).second == 42);

    auto [it2, ins2] = t.insert("hello", 99);
    assert(ins2 == false);
    assert((*it2).first == "hello");
    assert((*it2).second == 42);  // not overwritten
    std::printf("test11 insert pair: OK\n");
}

// Test 12: insert_or_assign returns pair<iterator, bool>
void test_insert_or_assign() {
    kstrie<int64_t> t;
    auto [it1, ins1] = t.insert_or_assign("key", 10);
    assert(ins1 == true);
    assert((*it1).second == 10);

    auto [it2, ins2] = t.insert_or_assign("key", 20);
    assert(ins2 == false);
    assert((*it2).second == 20);  // overwritten
    std::printf("test12 insert_or_assign: OK\n");
}

// Test 13: erase(iterator)
void test_erase_iter() {
    kstrie<int64_t> t;
    t.insert("a", 1);
    t.insert("b", 2);
    t.insert("c", 3);

    auto it = t.find("b");
    assert(it != t.end());
    auto next = t.erase(it);
    assert(t.size() == 2);
    assert(!t.contains("b"));
    assert(next != t.end());
    assert((*next).first == "c");
    std::printf("test13 erase iter: OK\n");
}

// Test 14: at()
void test_at() {
    kstrie<int64_t> t;
    t.insert("x", 99);
    assert(t.at("x") == 99);
    bool threw = false;
    try { t.at("missing"); } catch (const std::out_of_range&) { threw = true; }
    assert(threw);
    std::printf("test14 at: OK\n");
}

// Test 15: emplace / try_emplace
void test_emplace() {
    kstrie<int64_t> t;
    auto [it1, ins1] = t.emplace("k", 10);
    assert(ins1 && (*it1).second == 10);

    auto [it2, ins2] = t.try_emplace("k", 99);
    assert(!ins2 && (*it2).second == 10);  // not overwritten

    auto [it3, ins3] = t.try_emplace("k2", 20);
    assert(ins3 && (*it3).second == 20);
    std::printf("test15 emplace: OK\n");
}
