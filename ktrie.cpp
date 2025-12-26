/**
 * @file ktrie.cpp
 * @brief Implementation and tests for KTRIE
 * 
 * This file contains:
 * 1. Value type definitions for testing
 * 2. Test infrastructure
 * 3. Comprehensive test suite
 */

#ifndef NDEBUG
#define NDEBUG
#endif

#if 0
#include "ktrie.h"

using namespace gteitelbaum;

int main(int, char**) {
  ktrie<int, int> K{{0, 1}, {2, 4}, {6, 8}};

  return K.contains(0);
}
#else

#include "ktrie.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace gteitelbaum;

// Test data - 100 words
std::vector<std::string> tests = {
    "hello",    "world",       "hell",     "help",
    "h",        "he",          "hel",      "hello!",
    "helper",   "world!",      "bworld!",  "cworld!",
    "dworld!",  "eworld!",     "fworld!",  "gworld!",
    "hworld!",  "iworld!",     "jworld!",  "kworld!",
    "lworld!",  "mworld!",     "nworld!",  "oworld!",
    "pworld!",  "qworld!",     "rworld!",  "sworld!",
    "",         "abcdefghij",  "abcdefg",  "abcdefghijk",
    "apple",    "application", "apply",    "banana",
    "band",     "bandana",     "bank",     "banking",
    "car",      "card",        "care",     "careful",
    "carpet",   "carpool",     "category", "dog",
    "door",     "double",      "down",     "download",
    "dragon",   "dream",       "drive",    "driver",
    "elephant", "eleven",      "email",    "empty",
    "end",      "engine",      "enter",    "equal",
    "error",    "escape",      "event",    "example",
    "exist",    "exit",        "expect",   "experiment",
    "explain",  "express",     "extra",    "face",
    "fact",     "factory",     "fail",     "fair",
    "fall",     "false",       "family",   "famous",
    "fan",      "far",         "farm",     "fast",
    "father",   "favorite",    "fear",     "feature",
    "federal",  "fee",         "feed",     "feel",
    "feeling",  "feet",        "fellow",   "female"};

constexpr int NUM_KEYS = 1000000;

int passed = 0;
int failed = 0;

static void report(bool ok, const std::string& msg) {
  if (ok) {
    std::cout << "  OK: " << msg << std::endl;
    passed++;
  } else {
    std::cout << "  FAIL: " << msg << std::endl;
    failed++;
  }
}

// Helper to get type name
template <typename T>
constexpr const char* type_name() {
  if constexpr (std::is_same_v<T, int>)
    return "int";
  else if constexpr (std::is_same_v<T, long double>)
    return "long double";
  else
    return "unknown";
}

// Helper to create a value from an index
template <typename V>
V make_value(int i) {
  if constexpr (std::is_floating_point_v<V>) {
    return static_cast<long double>(i) * 1.5L + 0.123456789012345L;
  }
 return i;
}

// Helper to check value equality
template <typename V>
bool values_equal(const V& a, const V& b) {
  if constexpr (std::is_floating_point_v<V>) {
    return std::abs(a - b) < 1e-10L;
  }
  return a == b;
}

// ============================================================================
// char* tests - templated on value type
// ============================================================================
template <typename V>
void test_char_ptr() {
  std::cout << "\n=== Testing ktrie<char*, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<char*, V> K;

  // Insert
  int i = 0;
  for (const auto& t : tests) {
    K.insert(t.c_str(), t.size(), make_value<V>(i));
    i++;
  }
  report(K.size() == tests.size(),
         "Insert all - size = " + std::to_string(K.size()));

  // Find
  i = 0;
  bool all_found = true;
  for (const auto& t : tests) {
    auto ptr = K.find(t.c_str(), t.size());
    if (!ptr || !values_equal(*ptr, make_value<V>(i))) {
      all_found = false;
      std::cout << "    Missing or wrong: \"" << t << "\"" << std::endl;
    }
    i++;
  }
  report(all_found, "Find all inserted keys");

  // Contains
  bool contains_ok =
      K.contains("hello", 5) && K.contains("", 0) && !K.contains("notexist", 8);
  report(contains_ok, "Contains check");

  K.pretty_print(true);

  // Erase
  size_t erased = 0;
  for (const auto& t : tests) {
    erased += K.erase(t.c_str(), t.size());
  }
  report(erased == tests.size(),
         "Erase all - erased " + std::to_string(erased));
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// std::string tests - templated on value type
// ============================================================================
template <typename V>
void test_string() {
  std::cout << "\n=== Testing ktrie<std::string, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<std::string, V> K;

  // Insert
  int i = 0;
  for (const auto& t : tests) {
    K.insert({t, make_value<V>(i)});
    i++;
  }
  report(K.size() == tests.size(),
         "Insert all - size = " + std::to_string(K.size()));

  // Find via iterator
  i = 0;
  bool all_found = true;
  for (const auto& t : tests) {
    auto it = K.find(t);
    if (it == K.end() || !values_equal(it->second, make_value<V>(i))) {
      all_found = false;
      std::cout << "    Missing or wrong: \"" << t << "\"" << std::endl;
    }
    i++;
  }
  report(all_found, "Find all inserted keys");

  // Contains
  bool contains_ok =
      K.contains("hello") && K.contains("") && !K.contains("notexist");
  report(contains_ok, "Contains check");

  // operator[]
  K["newkey"] = make_value<V>(999);
  report(values_equal(K.at("newkey"), make_value<V>(999)),
         "operator[] insert and at()");

  // Iterator test
  int count = 0;
  for (const auto& [k, v] : K) {
    (void)k;
    (void)v;
    count++;
  }
  report(count == static_cast<int>(K.size()), "Iterator count matches size");

  K.pretty_print(true);

  // Erase
  size_t erased = 0;
  for (const auto& t : tests) {
    erased += K.erase(t);
  }
  K.erase("newkey");
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// int key tests - templated on value type
// ============================================================================
template <typename V>
void test_int_key() {
  std::cout << "\n=== Testing ktrie<int, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<int, V> K;

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<int> dist(std::numeric_limits<int>::min(),
                                          std::numeric_limits<int>::max());

  std::vector<int> keys;
  keys.reserve(NUM_KEYS);
  for (int i = 0; i < NUM_KEYS; ++i) {
    keys.push_back(dist(rng));
  }

  // Insert
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.insert(keys[i], make_value<V>(keys[i] % 10000 + 100));
  }
  std::cout << "  Inserted " << NUM_KEYS << " keys, unique count = " << K.size()
            << std::endl;

  // Find
  bool all_found = true;
  for (int i = 0; i < NUM_KEYS; ++i) {
    auto it = K.find(keys[i]);
    if (it == K.end()) {
      all_found = false;
      std::cout << "    Missing: " << keys[i] << std::endl;
      break;
    }
  }
  report(all_found, "Find all inserted keys");

  // Test negative numbers specifically
  ktrie<int, V> K2;
  K2.insert(-100, make_value<V>(1));
  K2.insert(-50, make_value<V>(2));
  K2.insert(0, make_value<V>(3));
  K2.insert(50, make_value<V>(4));
  K2.insert(100, make_value<V>(5));

  bool neg_ok = K2.contains(-100) && K2.contains(-50) && K2.contains(0) &&
                K2.contains(50) && K2.contains(100);
  report(neg_ok, "Negative number handling");

  // Check ordering
  std::vector<int> ord_keys;
  for (const auto& [k, v] : K2) {
    ord_keys.push_back(k);
  }
  bool ordered =
      (ord_keys.size() == 5 && ord_keys[0] == -100 && ord_keys[1] == -50 &&
       ord_keys[2] == 0 && ord_keys[3] == 50 && ord_keys[4] == 100);
  report(ordered, "Sorted order for int keys");

  K.pretty_print(true);

  // Erase
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.erase(keys[i]);
  }
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// unsigned int key tests - templated on value type
// ============================================================================
template <typename V>
void test_unsigned_int_key() {
  std::cout << "\n=== Testing ktrie<unsigned int, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<unsigned int, V> K;

  std::mt19937_64 rng(43);
  std::uniform_int_distribution<unsigned int> dist(
      std::numeric_limits<unsigned int>::min(),
      std::numeric_limits<unsigned int>::max());

  std::vector<unsigned int> keys;
  keys.reserve(NUM_KEYS);
  for (int i = 0; i < NUM_KEYS; ++i) {
    keys.push_back(dist(rng));
  }

  // Insert
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.insert(keys[i], make_value<V>(static_cast<int>(keys[i] % 10000) + 100));
  }
  std::cout << "  Inserted " << NUM_KEYS << " keys, unique count = " << K.size()
            << std::endl;

  // Find
  bool all_found = true;
  for (int i = 0; i < NUM_KEYS; ++i) {
    if (!K.contains(keys[i])) {
      all_found = false;
      std::cout << "    Missing: " << keys[i] << std::endl;
      break;
    }
  }
  report(all_found, "Find all inserted keys");

  // Test specific values
  ktrie<unsigned int, V> K2;
  K2.insert(0u, make_value<V>(1));
  K2.insert(100u, make_value<V>(2));
  K2.insert(1000u, make_value<V>(3));
  K2.insert(0xFFFFFFFFu, make_value<V>(4));

  bool vals_ok = values_equal(K2.at(0u), make_value<V>(1)) &&
                 values_equal(K2.at(100u), make_value<V>(2)) &&
                 values_equal(K2.at(1000u), make_value<V>(3)) &&
                 values_equal(K2.at(0xFFFFFFFFu), make_value<V>(4));
  report(vals_ok, "Specific unsigned values");

  // Check ordering
  std::vector<unsigned int> ord_keys;
  for (const auto& [k, v] : K2) {
    ord_keys.push_back(k);
  }
  bool ordered =
      (ord_keys.size() == 4 && ord_keys[0] == 0u && ord_keys[1] == 100u &&
       ord_keys[2] == 1000u && ord_keys[3] == 0xFFFFFFFFu);
  report(ordered, "Sorted order for unsigned int keys");

  K.pretty_print(true);

  // Erase
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.erase(keys[i]);
  }
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// int64_t key tests - templated on value type
// ============================================================================
template <typename V>
void test_int64_key() {
  std::cout << "\n=== Testing ktrie<int64_t, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<int64_t, V> K;

  std::mt19937_64 rng(44);
  std::uniform_int_distribution<int64_t> dist(
      std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());

  std::vector<int64_t> keys;
  keys.reserve(NUM_KEYS);
  for (int i = 0; i < NUM_KEYS; ++i) {
    keys.push_back(dist(rng));
  }

  // Insert
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.insert(keys[i], make_value<V>(i % 10000 + 100));
  }
  std::cout << "  Inserted " << NUM_KEYS << " keys, unique count = " << K.size()
            << std::endl;

  // Find
  bool all_found = true;
  for (int i = 0; i < NUM_KEYS; ++i) {
    if (!K.contains(keys[i])) {
      all_found = false;
      std::cout << "    Missing: " << keys[i] << std::endl;
      break;
    }
  }
  report(all_found, "Find all inserted keys");

  // Test extreme values
  ktrie<int64_t, V> K2;
  K2.insert(INT64_MIN, make_value<V>(1));
  K2.insert(-1LL, make_value<V>(2));
  K2.insert(0LL, make_value<V>(3));
  K2.insert(1LL, make_value<V>(4));
  K2.insert(INT64_MAX, make_value<V>(5));

  bool vals_ok = values_equal(K2.at(INT64_MIN), make_value<V>(1)) &&
                 values_equal(K2.at(-1LL), make_value<V>(2)) &&
                 values_equal(K2.at(0LL), make_value<V>(3)) &&
                 values_equal(K2.at(1LL), make_value<V>(4)) &&
                 values_equal(K2.at(INT64_MAX), make_value<V>(5));
  report(vals_ok, "Extreme int64_t values");

  // Check ordering
  std::vector<int64_t> ord_keys;
  for (const auto& [k, v] : K2) {
    ord_keys.push_back(k);
  }
  bool ordered = (ord_keys.size() == 5 && ord_keys[0] == INT64_MIN &&
                  ord_keys[1] == -1LL && ord_keys[2] == 0LL &&
                  ord_keys[3] == 1LL && ord_keys[4] == INT64_MAX);
  report(ordered, "Sorted order for int64_t keys");

  K.pretty_print(true);

  // Erase
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.erase(keys[i]);
  }
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// uint64_t key tests - templated on value type
// ============================================================================
template <typename V>
void test_uint64_key() {
  std::cout << "\n=== Testing ktrie<uint64_t, " << type_name<V>()
            << "> ===" << std::endl;
  ktrie<uint64_t, V> K;

  std::mt19937_64 rng(45);
  std::uniform_int_distribution<uint64_t> dist(
      std::numeric_limits<uint64_t>::min(),
      std::numeric_limits<uint64_t>::max());

  std::vector<uint64_t> keys;
  keys.reserve(NUM_KEYS);
  for (int i = 0; i < NUM_KEYS; ++i) {
    keys.push_back(dist(rng));
  }

  // Insert
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.insert(keys[i], make_value<V>(i % 10000 + 100));
  }
  std::cout << "  Inserted " << NUM_KEYS << " keys, unique count = " << K.size()
            << std::endl;

  // Find
  bool all_found = true;
  for (int i = 0; i < NUM_KEYS; ++i) {
    if (!K.contains(keys[i])) {
      all_found = false;
      std::cout << "    Missing: " << keys[i] << std::endl;
      break;
    }
  }
  report(all_found, "Find all inserted keys");

  // Test extreme values
  ktrie<uint64_t, V> K2;
  K2.insert(0ULL, make_value<V>(1));
  K2.insert(1ULL, make_value<V>(2));
  K2.insert(1000000000000ULL, make_value<V>(3));
  K2.insert(UINT64_MAX, make_value<V>(4));

  bool vals_ok = values_equal(K2.at(0ULL), make_value<V>(1)) &&
                 values_equal(K2.at(1ULL), make_value<V>(2)) &&
                 values_equal(K2.at(1000000000000ULL), make_value<V>(3)) &&
                 values_equal(K2.at(UINT64_MAX), make_value<V>(4));
  report(vals_ok, "Extreme uint64_t values");

  // Check ordering
  std::vector<uint64_t> ord_keys;
  for (const auto& [k, v] : K2) {
    ord_keys.push_back(k);
  }
  bool ordered =
      (ord_keys.size() == 4 && ord_keys[0] == 0ULL && ord_keys[1] == 1ULL &&
       ord_keys[2] == 1000000000000ULL && ord_keys[3] == UINT64_MAX);
  report(ordered, "Sorted order for uint64_t keys");

  K.pretty_print(true);

  // Erase
  for (int i = 0; i < NUM_KEYS; ++i) {
    K.erase(keys[i]);
  }
  report(K.empty(), "Empty after erase");
}

// ============================================================================
// Additional edge case tests - templated on value type
// ============================================================================
template <typename V>
void test_edge_cases() {
  std::cout << "\n=== Testing Edge Cases with " << type_name<V>()
            << " values ===" << std::endl;

  // Empty string key
  {
    ktrie<std::string, V> K;
    K.insert({"", make_value<V>(42)});
    report(K.contains(""), "Empty string as key");
    report(values_equal(K.at(""), make_value<V>(42)),
           "Empty string value correct");
  }

  // Single character keys
  {
    ktrie<std::string, V> K;
    for (int i = 0; i < 256; ++i) {
      std::string s(1, static_cast<char>(i));
      K.insert({s, make_value<V>(i)});
    }
    report(K.size() == 256, "256 single-char keys");

    bool all_ok = true;
    for (int i = 0; i < 256; ++i) {
      std::string s(1, static_cast<char>(i));
      if (!K.contains(s) || !values_equal(K.at(s), make_value<V>(i))) {
        all_ok = false;
        break;
      }
    }
    report(all_ok, "All single-char keys found with correct values");
  }

  // Very long key
  {
    ktrie<std::string, V> K;
    std::string long_key(10000, 'x');
    K.insert({long_key, make_value<V>(123)});
    report(K.contains(long_key), "Very long key (10000 chars)");
    report(values_equal(K.at(long_key), make_value<V>(123)),
           "Very long key value correct");
  }

  // Insert same key twice
  {
    ktrie<std::string, V> K;
    K.insert({"key", make_value<V>(1)});
    K.insert({"key", make_value<V>(2)});  // Should not update
    report(K.size() == 1, "Duplicate insert doesn't increase size");
    report(values_equal(K.at("key"), make_value<V>(1)),
           "First value preserved on duplicate insert");

    K.insert_or_assign("key", make_value<V>(3));  // Should update
    report(values_equal(K.at("key"), make_value<V>(3)),
           "insert_or_assign updates value");
  }

  // Clear and reuse
  {
    ktrie<int, V> K;
    for (int i = 0; i < 100; ++i) K.insert(i, make_value<V>(i));
    K.clear();
    report(K.empty(), "Clear makes empty");
    for (int i = 0; i < 100; ++i) K.insert(i, make_value<V>(i * 2));
    report(K.size() == 100, "Reuse after clear");
    report(values_equal(K.at(50), make_value<V>(100)),
           "Values correct after reuse");
  }

  // Lower/upper bound
  {
    ktrie<int, V> K;
    K.insert(10, make_value<V>(1));
    K.insert(20, make_value<V>(2));
    K.insert(30, make_value<V>(3));

    auto lb = K.lower_bound(15);
    report(lb != K.end() && lb->first == 20, "lower_bound(15) == 20");

    auto ub = K.upper_bound(20);
    report(ub != K.end() && ub->first == 30, "upper_bound(20) == 30");

    lb = K.lower_bound(20);
    report(lb != K.end() && lb->first == 20, "lower_bound(20) == 20");
  }
}

// ============================================================================
// Run all tests for a given value type
// ============================================================================
template <typename V>
void run_all_tests() {
  std::cout << "\n########################################################"
            << std::endl;
  std::cout << "     Running all tests with VALUE TYPE: " << type_name<V>()
            << std::endl;
  std::cout << "     sizeof(" << type_name<V>() << ") = " << sizeof(V)
            << std::endl;
  std::cout << "########################################################"
            << std::endl;

  test_char_ptr<V>();
  test_string<V>();
  test_int_key<V>();
  test_unsigned_int_key<V>();
  test_int64_key<V>();
  test_uint64_key<V>();
  test_edge_cases<V>();
}

int main() {
  std::cout << "========================================" << std::endl;
  std::cout << "       KTRIE Comprehensive Tests       " << std::endl;
  std::cout << "========================================" << std::endl;

  // Run all tests with int value type (SmallClass - stored inline)
  run_all_tests<int>();

  // Run all tests with long double value type (BigClass - allocated)
  run_all_tests<long double>();

  std::cout << "\n========================================" << std::endl;
  std::cout << "              RESULTS                  " << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Passed: " << passed << std::endl;
  std::cout << "Failed: " << failed << std::endl;
  std::cout << "========================================" << std::endl;

  return failed > 0 ? 1 : 0;
}
#endif