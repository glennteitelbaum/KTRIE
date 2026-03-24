#include "kstrie.hpp"
#include <cstdio>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gteitelbaum;

void test_subscript();
void test_subscript_bool();

int main() {
    // insert returns pair<iterator, bool>
    {
        kstrie<int64_t> t;
        auto [it1, ins1] = t.insert("hello", 42);
        assert(ins1 == true);
        assert((*it1).first == "hello");
        assert((*it1).second == 42);

        auto [it2, ins2] = t.insert("hello", 99);
        assert(ins2 == false);
        assert((*it2).first == "hello");
        assert((*it2).second == 42);  // not overwritten
        std::printf("test insert pair: OK\n");
    }

    // insert_or_assign returns pair<iterator, bool>
    {
        kstrie<int64_t> t;
        auto [it1, ins1] = t.insert_or_assign("key", 10);
        assert(ins1 == true);
        assert((*it1).second == 10);

        auto [it2, ins2] = t.insert_or_assign("key", 20);
        assert(ins2 == false);
        assert((*it2).second == 20);  // overwritten
        std::printf("test insert_or_assign: OK\n");
    }

    // erase(iterator)
    {
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
        std::printf("test erase(iter): OK\n");
    }

    // erase(iterator) at last element
    {
        kstrie<int64_t> t;
        t.insert("a", 1);
        t.insert("b", 2);
        auto it = t.find("b");
        auto next = t.erase(it);
        assert(next == t.end());
        assert(t.size() == 1);
        std::printf("test erase(iter) last: OK\n");
    }

    // erase(first, last)
    {
        kstrie<int64_t> t;
        t.insert("a", 1);
        t.insert("b", 2);
        t.insert("c", 3);
        t.insert("d", 4);

        auto first = t.find("b");
        auto last  = t.find("d");
        auto result = t.erase(first, last);
        assert(t.size() == 2);
        assert(t.contains("a"));
        assert(!t.contains("b"));
        assert(!t.contains("c"));
        assert(t.contains("d"));
        assert(result != t.end());
        assert((*result).first == "d");
        std::printf("test erase(range): OK\n");
    }

    // at()
    {
        kstrie<int64_t> t;
        t.insert("x", 99);
        assert(t.at("x") == 99);
        bool threw = false;
        try { t.at("missing"); } catch (const std::out_of_range&) { threw = true; }
        assert(threw);
        std::printf("test at: OK\n");
    }

    // emplace / try_emplace
    {
        kstrie<int64_t> t;
        auto [it1, ins1] = t.emplace("k", 10);
        assert(ins1 && (*it1).second == 10);

        auto [it2, ins2] = t.try_emplace("k", 99);
        assert(!ins2 && (*it2).second == 10);  // not overwritten

        auto [it3, ins3] = t.try_emplace("k2", 20);
        assert(ins3 && (*it3).second == 20);
        std::printf("test emplace/try_emplace: OK\n");
    }

    // insert returns working iterator (can advance)
    {
        kstrie<int64_t> t;
        t.insert("a", 1);
        t.insert("c", 3);
        auto [it, ins] = t.insert("b", 2);
        assert(ins);
        assert((*it).first == "b");
        ++it;
        assert((*it).first == "c");
        std::printf("test insert iter advance: OK\n");
    }

    // Bool specialization insert
    {
        kstrie<bool> t;
        auto [it1, ins1] = t.insert("yes", true);
        assert(ins1);
        assert((*it1).second == true);
        auto [it2, ins2] = t.insert("no", false);
        assert(ins2);
        assert((*it2).second == false);
        std::printf("test bool insert: OK\n");
    }

    test_subscript();
    test_subscript_bool();

    std::printf("\nALL API TESTS OK\n");
}

// Test: operator[]
void test_subscript() {
    kstrie<int64_t> t;
    t["x"] = 42;
    assert(t["x"] == 42);
    assert(t.size() == 1);
    
    // Default-insert on missing key
    int64_t v = t["missing"];
    assert(v == 0);  // default VALUE{}
    assert(t.size() == 2);
    
    // Modify via operator[]
    t["x"] = 100;
    assert(t["x"] == 100);
    
    // Compound assignment
    t["x"] += 5;
    assert(t["x"] == 105);
    std::printf("test operator[]: OK\n");
}

// Test: bool operator[] with bool_ref
void test_subscript_bool() {
    kstrie<bool> t;
    t["a"] = true;
    t["b"] = false;
    assert(t["a"] == true);
    assert(t["b"] == false);
    assert(t.size() == 2);
    
    // Flip via operator[]
    t["a"] = false;
    assert(t["a"] == false);
    
    // Default-insert (bool{} = false)
    bool v = t["new"];
    assert(v == false);
    assert(t.size() == 3);
    std::printf("test bool operator[]: OK\n");
}
