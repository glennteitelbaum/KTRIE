# KTRIE vs std::map vs std::unordered_map: Comprehensive Comparison

This document provides a detailed comparison of KTRIE against the C++ Standard Library associative containers `std::map` and `std::unordered_map`. Use this guide to understand the trade-offs and select the optimal container for your use case.

## Table of Contents

1. [Architecture Comparison](#architecture-comparison)
2. [Memory Overhead Per Entry](#memory-overhead-per-entry)
3. [API Comparison](#api-comparison)
4. [Cache Behavior](#cache-behavior)
5. [Performance Comparison](#performance-comparison)
6. [Selection Criteria](#selection-criteria)
7. [Migration Guide](#migration-guide)
8. [Performance Estimates](#performance-estimates)

---

## Architecture Comparison

### std::map - Red-Black Tree

```
                    [Root Node]
                   /          \
            [Left]              [Right]
           /      \            /       \
        [LL]      [LR]      [RL]       [RR]
```

**Node Structure (typical implementation):**
```cpp
struct map_node {
    Key key;                    // Full key stored
    Value value;                // Full value stored
    map_node* parent;           // 8 bytes
    map_node* left;             // 8 bytes
    map_node* right;            // 8 bytes
    int8_t color;               // 1 byte (+ padding)
};
// Minimum overhead: 32 bytes + key + value + alignment padding
```

**Characteristics:**
- Self-balancing binary search tree
- Each node stores complete key-value pair
- O(log n) guaranteed for all operations
- Nodes scattered in heap memory
- Pointer-heavy traversal

### std::unordered_map - Hash Table

```
Bucket Array:
[0] -> Entry -> Entry -> nullptr
[1] -> nullptr
[2] -> Entry -> nullptr
[3] -> Entry -> Entry -> Entry -> nullptr
...
[n] -> nullptr
```

**Node Structure (typical implementation):**
```cpp
struct hash_node {
    Key key;                    // Full key stored
    Value value;                // Full value stored
    hash_node* next;            // 8 bytes (chain pointer)
    size_t hash_cache;          // 8 bytes (cached hash, optional)
};
// Plus bucket array: sizeof(pointer) * bucket_count
// Minimum overhead: 16 bytes + key + value per entry
// Plus: 8 bytes * bucket_count for bucket array
```

**Characteristics:**
- Array of buckets with collision chains
- Each node stores complete key-value pair
- O(1) average, O(n) worst case
- Rehashing causes periodic O(n) operations
- No ordering guarantees

### KTRIE - Compressed Trie

```
Storing 'pre', 'prefix' and 'suffix' with a depth of 2
[EOS] Represents the End of String and pointer to the value

Level 0: [Head]
Level 1:  +-> [LIST 'p', 's'] [PTR: 'p'][PTR: 's']
Level2a                       |         +-> [HOP "uffix"][EOS: suffix]                                  
Level2b:                      +-> [HOP "pre"] [EOS:'pre'] [HOP "fix"] [EOS:'prefix']
                                             ^                     
                                             (Note: shared prefix "pre")
```

**Node Structure:**
```cpp
// Every node is exactly 8 bytes
It acts like a union but each type is stored in a uint64_t regardless of interpretation
It is taken out and put back using std::bit_cast

uint64_t raw;                             // Raw 64-bit value
// Interpreted as one of:
struct { uint64_t flags:5, ptr:59; };     // Dirty pointer
struct { char chars[6], flags, len; };    // HOP (1-6 chars inline)
struct { uint64_t flags:5, length:59; };  // SKIP header
struct { char chars[7], count; };         // LIST (≤7 sorted chars)
uint64_t pop_bitmap;                      // POP (first part of 256-bit bitmap)
T value;                                  // EOS Inline value (sizeof(T) ≤ 8) otherwise a pointer

// Stored in the uint64_t regardless of interpretation
```

**Characteristics:**
- Compressed radix trie with path compression
- Keys share common prefixes (no duplication)
- Fixed 8-byte nodes in contiguous arrays
- Ordered traversal guaranteed
- Key-length dependent operations

---

## Memory Overhead Per Entry

### Theoretical Overhead

| Container | Fixed Overhead | Variable Overhead | Total for int→int |
|-----------|---------------|-------------------|-------------------|
| **std::map** | 32-40 bytes | +key +value +padding | ~48-56 bytes |
| **std::unordered_map** | 16-24 bytes | +key +value +bucket share | ~32-40 bytes + bucket |
| **KTRIE** | 8-24 bytes | Shared prefixes reduce | ~16-32 bytes typical |

### Detailed Breakdown

#### std::map<std::string, int>
```
Per entry:
  - Key (std::string): 32 bytes (SSO) or 32 + heap
  - Value (int): 4 bytes + 4 padding
  - Parent pointer: 8 bytes
  - Left pointer: 8 bytes
  - Right pointer: 8 bytes
  - Color + padding: 8 bytes
  ─────────────────────────────
  Total: 72+ bytes per entry
```

#### std::unordered_map<std::string, int>
```
Per entry:
  - Key (std::string): 32 bytes (SSO) or 32 + heap
  - Value (int): 4 bytes + 4 padding
  - Next pointer: 8 bytes
  - Hash cache: 8 bytes (implementation dependent)
  ─────────────────────────────
  Entry: 56+ bytes
  Plus bucket array: ~1 pointer per entry (load factor)
  Total: ~64+ bytes per entry
```

#### ktrie<std::string, int>
```
Per entry (varies with prefix sharing):
  - Shared prefix nodes: amortized across entries
  - Branch node contribution: 8-16 bytes
  - Value node: 8 bytes
  - Unique suffix: 8-16 bytes typical
  ─────────────────────────────
  Total: 16-40 bytes typical (highly dependent on key patterns)
  
With heavy prefix sharing (URLs, paths):
  Total: 8-20 bytes per entry
```

### Memory Comparison Chart

```
Entries: 100,000 string keys (average 20 chars, 50% prefix sharing)

std::map:           ~7.2 MB  (72 bytes × 100K)
std::unordered_map: ~6.4 MB  (64 bytes × 100K)  
KTRIE:              ~2.4 MB  (24 bytes × 100K, with sharing)

Memory Savings with KTRIE: 63-67%
```

---

## API Comparison

### Core Operations Availability

| Operation | std::map | std::unordered_map | KTRIE |
|-----------|----------|-------------------|-------|
| `insert` | ✅ | ✅ | ✅ |
| `emplace` | ✅ | ✅ | ✅ |
| `try_emplace` | ✅ | ✅ | ✅ |
| `insert_or_assign` | ✅ | ✅ | ✅ |
| `erase(key)` | ✅ | ✅ | ✅ |
| `erase(iterator)` | ✅ | ✅ | ✅ |
| `find` | ✅ | ✅ | ✅ |
| `contains` | ✅ | ✅ | ✅ |
| `count` | ✅ | ✅ | ✅ |
| `at` | ✅ | ✅ | ✅ |
| `operator[]` | ✅ | ✅ | ✅ |
| `clear` | ✅ | ✅ | ✅ |
| `swap` | ✅ | ✅ | ✅ |
| `size` | ✅ | ✅ | ✅ |
| `empty` | ✅ | ✅ | ✅ |

### Ordering & Range Operations

| Operation | std::map | std::unordered_map | KTRIE |
|-----------|----------|-------------------|-------|
| Ordered iteration | ✅ | ❌ | ✅ |
| `lower_bound` | ✅ | ❌ | ✅ |
| `upper_bound` | ✅ | ❌ | ✅ |
| `equal_range` | ✅ | ✅ | ✅ |
| Reverse iteration | ✅ | ❌ | ✅ |

### Advanced Features

| Feature | std::map | std::unordered_map | KTRIE |
|---------|----------|-------------------|-------|
| Custom comparator | ✅ | ❌ | ❌ |
| Custom hash function | 🔶 N/A | ✅ | 🔶 N/A |
| Custom key equality | ❌ | ✅ | ❌ |
| Node handle (extract) | ✅ | ✅ | ❌ |
| Heterogeneous lookup | ✅ (C++14) | ✅ (C++20) | ❌ |
| Prefix search | ❌ | ❌ | ✅ |
| Thread-safe iteration | ❌ | ❌ | ❌ |

### Key Type Support

| Key Type | std::map | std::unordered_map | KTRIE |
|----------|----------|-------------------|-------|
| `std::string` | ✅ | ✅ | ✅ |
| `const char*` | ❌ | ❌ | ✅ |
| `int`, `long`, etc. | ✅ | ✅ | ✅ |
| `int64_t`, `uint64_t` | ✅ | ✅ | ✅ |
| `float`, `double` | ✅ | ✅ | ✅ |
| Custom class | ✅ (with `<`) | 🔶 (with hash/=) | ❌ |
| `std::pair` | ✅ (with `<`) | 🔶 (with hash/=) | ❌ |
| Composite keys | ✅ (with `<`) | 🔶 (with hash/=) | ❌ |

*It is easier ✅ to provide a custom* `operator<`
*than 🔶 a hash and* `operator=`

### Missing from KTRIE
s
2. **Custom Comparators** - Ordering is lexicographic only
3. **Heterogeneous Lookup** - Keys must match exact type
4. **Node Handles** - No extract operations
5. **Custom Key Types** - Limited to string, char*, and numeric types
6. **Multi-key variants** - No multimap equivalent

### Unique to KTRIE

1. **Prefix Sharing** - Memory efficiency for similar keys
2. **Prefix Find** - find all elements starting with
3. **Fixed Node Size** - Predictable memory layout
4. **Cache-Optimized Arrays** - Contiguous node storage
5. **Implicit Ordering** - No rebalancing needed
 
---

## Cache Behavior

### Memory Access Patterns

#### std::map
```
Find("hello"):
  1. Access root node         → Likely L3 miss (random heap location)
  2. Compare key, go left     → Likely L3 miss (different heap location)
  3. Compare key, go right    → Likely L3 miss
  4. Compare key, found       → L1/L2 if lucky
  
Expected cache misses: O(log n), each likely L3
```

#### std::unordered_map
```
Find("hello"):
  1. Hash computation         → CPU only
  2. Access bucket array      → L1/L2 hit (array is contiguous)
  3. Access first chain node  → L3 miss (heap allocated)
  4. Compare key (if collision) → L3 miss
  
Expected cache misses: 1-3 typical, mostly L3
```

#### KTRIE
```
Find("hello"):
  1. Access root array        → L2/L3 miss first time
  2. Match "hel" (HOP)        → Same cache line as step 1
  3. Check LIST ['l','o']     → Same array, likely L1
  4. Follow pointer           → L2/L3 miss (new array)
  5. Match "lo" + EOS         → Same cache line as step 4
  
Expected cache misses: 1-2 for short keys, arrays stay hot
```

### Cache Miss Analysis

| Scenario | std::map | std::unordered_map | KTRIE |
|----------|----------|-------------------|-------|
| **L1 Hit Rate** | 5-10% | 20-40% | 40-60% |
| **L2 Hit Rate** | 10-20% | 30-50% | 50-70% |
| **L3 Miss Rate** | 60-80% | 30-50% | 20-40% |

### Latency Expectations (approximate cycles)

| Access Type | Latency |
|-------------|---------|
| L1 Cache Hit | 4-5 cycles |
| L2 Cache Hit | 12-15 cycles |
| L3 Cache Hit | 40-50 cycles |
| Main Memory | 200-300 cycles |

**Expected Total Latency per Lookup:**

| Container | Best Case | Typical Case | Worst Case |
|-----------|-----------|--------------|------------|
| **std::map** (n=10K) | ~150 cycles | ~500 cycles | ~1000 cycles |
| **std::unordered_map** | ~50 cycles | ~150 cycles | ~1000 cycles |
| **KTRIE** (20-char key) | ~80 cycles | ~200 cycles | ~600 cycles |

### Cache Behavior Summary

```
                    Cache Friendliness Score (higher = better)
                    
std::map:           ████░░░░░░  (scattered nodes, pointer chasing)
std::unordered_map: ██████░░░░  (bucket array hot, chains cold)
KTRIE:              ████████░░  (contiguous arrays, good locality)
```

---

## Performance Comparison

### Time Complexity

| Operation | std::map | std::unordered_map | KTRIE |
|-----------|----------|-------------------|-------|
| **Find** | | | |
| Best | O(1)* | O(1) | O(1)* |
| Average | O(log n) | O(1) | O(k) |
| Worst | O(log n) | O(n) | O(k) |
| **Insert** | | | |
| Best | O(1)* | O(1) | O(1)* |
| Average | O(log n) | O(1) | O(k) |
| Worst | O(log n) | O(n)† | O(k) |
| **Delete** | | | |
| Best | O(1)* | O(1) | O(k) |
| Average | O(log n) | O(1) | O(k) |
| Worst | O(log n) | O(n) | O(k) |
| **Iteration (full)** | | | |
| Total | O(n) | O(n + buckets) | O(n) |
| Per-step | O(1) amortized | O(1) amortized | O(1) amortized |

*With hint  
†Rehashing  
k = key length, n = number of elements

### Key Observations

1. **KTRIE is key-length dependent, not element-count dependent**
   - 1 million entries with 10-char keys: same speed as 100 entries
   - This is a major advantage for large datasets

2. **std::unordered_map has O(n) worst case**
   - Hash collisions can cause linear probe
   - Rehashing is O(n) and can cause latency spikes

3. **std::map has consistent O(log n)**
   - No worst-case surprises
   - But log n grows with size

### Comparison by Key Length

For `n = 1,000,000` entries:

| Key Length | std::map | std::unordered_map | KTRIE |
|------------|----------|-------------------|-------|
| 4 bytes | ~20 comparisons | 1 + collisions | ~4 byte checks |
| 16 bytes | ~20 comparisons | 1 + collisions | ~6-10 byte checks |
| 64 bytes | ~20 comparisons | 1 + collisions | ~8-16 byte checks |
| 256 bytes | ~20 comparisons | 1 + collisions | ~16-32 byte checks |

**Winner by key length:**
- Short keys (≤16 bytes): `unordered_map` (hash is fast)
- Medium keys (16-64 bytes): Tie or `KTRIE` (prefix sharing helps)
- Long keys (>64 bytes): `KTRIE` (path compression wins)

---

## Selection Criteria

### Decision Matrix

| Criterion | Choose std::map | Choose unordered_map | Choose KTRIE |
|-----------|-----------------|---------------------|--------------|
| **In Standard Library** | ✅ | ✅ | ❌ |
| **Custom key types** | ✅ | 🔶 | ❌ |
| **Need ordered iteration** | ✅ | ❌ | ✅ |
| **Need range queries** | ✅ | ❌ | ✅ |
| **Predictable latency** | ✅ | ❌ | ✅ |
| **Need O(1) average lookup** | ❌ | ✅ | 🔶 |
| **Need Large Dataset** | ❌ | ✅ | ✅ |
| **Memory constrained** | ❌ | ❌ | ✅ |
| **Keys share prefixes** | ❌ | ❌ | ✅ |
| **Long string keys** | ❌ | ❌ | ✅ |

### When to Choose Each

#### Choose std::map when:
- You need custom comparison functions
- You need node extraction/merging
- You need copy semantics
- Key type is custom class
- Dataset is small (< 1000 entries)
- Predictable O(log n) is acceptable

#### Choose std::unordered_map when:
- O(1) average lookup is critical
- Ordering doesn't matter
- Memory is not constrained
- Keys are short and hashable
- You can tolerate occasional latency spikes
- You need heterogeneous lookup (C++20)

#### Choose KTRIE when:
- Keys are strings with common prefixes (URLs, paths, words)
- Memory efficiency is important
- You need ordered iteration AND good performance
- Dataset is large (> 10K entries)
- Keys are long strings
- You need range queries (lower_bound, upper_bound)
- Predictable, key-length-based performance is desired

### Use Case Examples

| Use Case | Recommended | Why |
|----------|-------------|-----|
| URL routing table | **KTRIE** | URLs share prefixes, need prefix matching |
| User session cache | unordered_map | Random IDs, need O(1), no ordering |
| Dictionary/spell check | **KTRIE** | Words share prefixes, ordered iteration |
| Configuration map | std::map | Small size, custom types, copy needed |
| IP address lookup | **KTRIE** | Numeric keys, prefix sharing |
| Database index | **KTRIE** or map | Range queries needed |
| In-memory cache | unordered_map | Random keys, O(1) critical |
| File system paths | **KTRIE** | Heavy prefix sharing |
| Symbol table | **KTRIE** | Identifiers share prefixes |
| Event handlers | std::map | Small, custom comparison |

### Size-Based Recommendations

| Entry Count | Short Keys (≤16B) | Long Keys (>64B) | Prefix-Heavy |
|-------------|-------------------|------------------|--------------|
| < 100 | Any (map simplest) | Any | Any |
| 100 - 1K | unordered_map | map or KTRIE | KTRIE |
| 1K - 10K | unordered_map | KTRIE | KTRIE |
| 10K - 100K | unordered_map | KTRIE | KTRIE |
| 100K - 1M | unordered_map | KTRIE | **KTRIE** |
| > 1M | unordered_map | **KTRIE** | **KTRIE** |

---

## Migration Guide

### From std::map to KTRIE

#### Compatible Patterns (No Changes Needed)

```cpp
// These work identically:
container[key] = value;
container.insert({key, value});
container.find(key);
container.erase(key);
container.contains(key);
container.at(key);
container.size();
container.empty();
container.clear();

// Iteration works the same:
for (const auto& [k, v] : container) { ... }

// Range operations work the same:
auto it = container.lower_bound(key);
auto it = container.upper_bound(key);
```

#### Required Changes

```cpp
// BEFORE: std::map with custom comparator
std::map<std::string, int, std::greater<>> m;

// AFTER: KTRIE doesn't support custom comparators
// Either: reverse your iteration, or stay with std::map
ktrie<std::string, int> k;  // Always ascending order


// BEFORE: Copy construction
std::map<std::string, int> m2 = m1;

// AFTER: Move only
ktrie<std::string, int> k2 = std::move(k1);
// Or manually copy:
ktrie<std::string, int> k2;
for (const auto& [key, val] : k1) k2.insert({key, val});


// BEFORE: Custom key type
struct MyKey { ... };
std::map<MyKey, int> m;

// AFTER: Not supported - use string or numeric keys only
// Option 1: Serialize to string
ktrie<std::string, int> k;
k.insert({mykey.serialize(), value});

// Option 2: Stay with std::map
```

#### Migration Checklist

- [ ] Key type is `std::string`, `char*`, or numeric
- [ ] No custom comparator needed
- [ ] No copy construction/assignment needed
- [ ] No node handle operations (extract/merge)
- [ ] No heterogeneous lookup needed
- [ ] Replace copy with move where needed
- [ ] Test ordered iteration still correct

### From std::unordered_map to KTRIE

#### New Capabilities After Migration

#### Required Changes

```cpp
// BEFORE: Custom hash function
struct MyHash { size_t operator()(const Key& k) const; };
std::unordered_map<Key, Value, MyHash> um;

// AFTER: No hash needed (uses trie structure)
ktrie<std::string, Value> k;


// BEFORE: Bucket operations
um.bucket_count();
um.load_factor();
um.rehash(n);

// AFTER: Not applicable - remove these calls


// BEFORE: Reserve
um.reserve(10000);

// AFTER: Not needed - KTRIE grows incrementally
// (Can remove, but no direct equivalent)

```

#### Migration Checklist

- [ ] Key type is `std::string`, `char*`, or numeric
- [ ] Remove bucket-related code
- [ ] Remove reserve() calls (optional)
- [ ] Update iteration order expectations (now sorted!)
- [ ] Test performance with real data

### Migration Code Example

```cpp
// BEFORE
#include <map>
std::map<std::string, int> wordCount;
wordCount.insert({"hello", 1});
wordCount["world"] = 2;
auto it = wordCount.find("hello");
for (const auto& [word, count] : wordCount) {
    std::cout << word << ": " << count << "\n";
}

// AFTER - Minimal changes
#include "ktrie.h"
using namespace gteitelbaum;
ktrie<std::string, int> wordCount;  // Changed type only
wordCount.insert({"hello", 1});     // Same
wordCount["world"] = 2;             // Same
auto it = wordCount.find("hello"); // Same
for (const auto& [word, count] : wordCount) {  // Same
    std::cout << word << ": " << count << "\n";
}
```

---

## Performance Estimates

### Lookup Performance (nanoseconds)

Estimates for modern CPU (3.5 GHz), n = 100,000 entries:

| Key Type | Key Length | std::map | unordered_map | KTRIE |
|----------|------------|----------|---------------|-------|
| string | 8 chars | 250 ns | 45 ns | 80 ns |
| string | 16 chars | 280 ns | 55 ns | 100 ns |
| string | 32 chars | 320 ns | 70 ns | 130 ns |
| string | 64 chars | 380 ns | 100 ns | 160 ns |
| string | 128 chars | 450 ns | 150 ns | 200 ns |
| int32 | 4 bytes | 200 ns | 35 ns | 60 ns |
| int64 | 8 bytes | 220 ns | 40 ns | 70 ns |

### Insert Performance (nanoseconds)

| Key Type | Key Length | std::map | unordered_map | KTRIE |
|----------|------------|----------|---------------|-------|
| string | 8 chars | 350 ns | 80 ns | 150 ns |
| string | 32 chars | 450 ns | 120 ns | 200 ns |
| string | 128 chars | 650 ns | 250 ns | 300 ns |

### Memory Usage (MB for 100K entries)

| Key Type | Avg Key Len | std::map | unordered_map | KTRIE |
|----------|-------------|----------|---------------|-------|
| string | 8 chars | 6.4 MB | 5.2 MB | 2.8 MB |
| string | 32 chars | 8.8 MB | 7.6 MB | 3.2 MB |
| string (prefix-heavy) | 32 chars | 8.8 MB | 7.6 MB | 1.8 MB |
| int64 | 8 bytes | 5.6 MB | 4.4 MB | 2.0 MB |

### Scaling Characteristics

```
Lookup time vs. Entry Count (32-char string keys):

Time (ns)
    │
500 │                              ╭─── std::map (O(log n))
    │                         ╭────╯
400 │                    ╭────╯
    │               ╭────╯
300 │          ╭────╯
    │     ╭────╯
200 │╭────╯─────────────────────────────── KTRIE (O(k), flat)
    │
100 │════════════════════════════════════ unordered_map (O(1), flat)
    │
  0 └────────────────────────────────────────────────────
    1K    10K    100K    1M    10M    Entry Count
```

### Break-Even Analysis

**KTRIE beats std::map when:**
- Entry count > ~500 (memory)
- Entry count > ~1000 (lookup speed)
- Keys share >30% common prefixes

**KTRIE beats unordered_map when:**
- Keys share >50% common prefixes (memory)
- Need ordered iteration
- Need range queries
- Key length > 64 bytes

### Real-World Benchmark Estimates

| Workload | std::map | unordered_map | KTRIE | Winner |
|----------|----------|---------------|-------|--------|
| URL routing (1M URLs) | 450 ns | 80 ns | 120 ns | unordered* |
| URL routing (1M URLs) + memory | 89 MB | 72 MB | 28 MB | **KTRIE** |
| Dictionary (500K words) | 380 ns | 50 ns | 90 ns | unordered* |
| Dictionary + prefix search | N/A | N/A | 90 ns | **KTRIE** |
| File paths (100K paths) | 320 ns | 70 ns | 85 ns | unordered* |
| File paths + memory | 12 MB | 10 MB | 3.5 MB | **KTRIE** |
| Config keys (1K entries) | 150 ns | 35 ns | 60 ns | unordered_map |
| Numeric IDs (1M int64) | 280 ns | 40 ns | 70 ns | unordered_map |

*unordered_map wins raw speed but loses on memory and ordering

---

## Summary

### Quick Reference Card

| Aspect | std::map | std::unordered_map | KTRIE |
|--------|----------|-------------------|-------|
| **Lookup** | O(log n) | O(1) avg | O(k) |
| **Memory** | High | Medium | Low |
| **Ordered** | ✅ | ❌ | ✅ |
| **Range Queries** | ✅ | ❌ | ✅ |
| **Best For** | Small sets, custom types | Speed critical | Large string sets |

*n is number of entries*
*k is length of keys*

### One-Line Recommendations

- **Need fastest lookup, don't care about order?** → `std::unordered_map`
- **Need ordering and custom comparators?** → `std::map`
- **Have string keys with shared prefixes and need efficiency?** → `KTRIE`
- **Memory constrained with large dataset?** → `KTRIE`
- **Need both ordering AND efficiency at scale?** → `KTRIE`

---

## See Also

- [README.md](./README.md) - Main documentation, API reference, quick start
- [Architecture Guide](./architecture.md) - Node layouts, algorithms, state machines
- [Implementation Concepts](./concepts.md) - Dirty pointers, SWAR, numeric conversion details
- [Internal API Reference](./internal_api.md) - Full class and function documentation

---

*Document Version: 1.0*  
*Last Updated: December 2025*
