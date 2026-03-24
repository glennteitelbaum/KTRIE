#include "kstrie.hpp"
#include <cstdio>
#include <cassert>
#include <string>

using namespace gteitelbaum;

int main() {
    // Basic CRUD
    kstrie<int64_t> t;
    for (int i = 0; i < 5000; ++i) {
        std::string key = "key" + std::to_string(i);
        t.insert(key, static_cast<int64_t>(i));
    }
    assert(t.size() == 5000);
    assert(t.contains("key0"));
    assert(t.contains("key4999"));
    assert(!t.contains("missing"));
    
    // Iteration (snapshot, still works)
    size_t count = 0;
    std::string prev;
    for (auto it = t.begin(); it != t.end(); ++it) {
        auto [k, v] = *it;
        if (count > 0) assert(k > prev);
        prev = k;
        count++;
    }
    assert(count == 5000);
    
    // Erase half
    for (int i = 0; i < 5000; i += 2)
        t.erase("key" + std::to_string(i));
    assert(t.size() == 2500);
    
    // Bool specialization
    kstrie<bool> tb;
    for (int i = 0; i < 1000; ++i)
        tb.insert("k" + std::to_string(i), i & 1);
    assert(tb.size() == 1000);
    
    // EOS key (empty string)
    kstrie<int64_t> te;
    te.insert("", 0);
    te.insert("a", 1);
    te.insert("ab", 2);
    assert(te.size() == 3);
    assert(te.contains(""));
    
    std::printf("ALL OK\n");
}
