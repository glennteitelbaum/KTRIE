#include "ktrie.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace gteitelbaum;

int main() {
    std::cout << "Testing ktrie<std::string, int>...\n";
    {
        ktrie<std::string, int> K;
        K.insert({"hello", 1});
        K.insert({"world", 2});
        K.insert({"help", 3});
        K["test"] = 4;
        
        if (K.size() != 4) { std::cout << "FAIL: size\n"; return 1; }
        if (!K.contains("hello")) { std::cout << "FAIL: contains\n"; return 1; }
        if (K.at("hello") != 1) { std::cout << "FAIL: at\n"; return 1; }
        
        int count = 0;
        for (const auto& [k, v] : K) { count++; (void)k; (void)v; }
        if (count != 4) { std::cout << "FAIL: iteration\n"; return 1; }
        
        K.erase("hello");
        if (K.contains("hello")) { std::cout << "FAIL: erase\n"; return 1; }
        
        std::cout << "  PASS: basic operations\n";
    }
    
    std::cout << "Testing ktrie<int, int>...\n";
    {
        ktrie<int, int> K;
        K.insert({-100, 1});
        K.insert({0, 2});
        K.insert({100, 3});
        
        if (K.size() != 3) { std::cout << "FAIL: size\n"; return 1; }
        if (K.at(-100) != 1) { std::cout << "FAIL: negative key\n"; return 1; }
        
        std::vector<int> keys;
        for (const auto& [k, v] : K) { keys.push_back(k); (void)v; }
        if (keys.size() != 3 || keys[0] != -100 || keys[1] != 0 || keys[2] != 100) {
            std::cout << "FAIL: order\n"; return 1;
        }
        
        std::cout << "  PASS: int keys\n";
    }
    
    std::cout << "Testing ktrie<uint64_t, int>...\n";
    {
        ktrie<uint64_t, int> K;
        std::mt19937_64 rng(42);
        std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
        std::vector<uint64_t> keys;
        for (int i = 0; i < 1000; ++i) {
            uint64_t k = dist(rng);
            keys.push_back(k);
            K.insert(k, i);
        }
        
        bool all_found = true;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!K.contains(keys[i])) { all_found = false; break; }
        }
        if (!all_found) { std::cout << "FAIL: find\n"; return 1; }
        
        for (size_t i = 0; i < keys.size(); ++i) K.erase(keys[i]);
        if (!K.empty()) { std::cout << "FAIL: erase all\n"; return 1; }
        
        std::cout << "  PASS: uint64 keys\n";
    }
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
