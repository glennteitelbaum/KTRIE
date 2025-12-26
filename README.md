# KTRIE - High-Performance Trie-Based Associative Container

A header-only C++20/C++23 implementation of an ordered associative container using a compact trie data structure. KTRIE provides a `std::map`-like interface with optimized memory layout and cache-friendly traversal.

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
- [Key Types](#key-types)
- [Header Files and Include Hierarchy](#header-files-and-include-hierarchy)
- [Macros and Symbols](#macros-and-symbols)
- [Documentation](#documentation)
  - [Architecture Guide](./architecture.md) - Node layouts, algorithms, state machines
  - [Implementation Concepts](./concepts.md) - Dirty pointers, SWAR, numeric conversion
  - [Comparison Guide](./comparisons.md) - vs std::map/unordered_map, migration
  - [Internal API Reference](./internal_api.md) - Full class/function documentation
- [Architecture](#architecture)
- [Performance Characteristics](#performance-characteristics)
- [Memory Layout](#memory-layout)
- [Thread Safety](#thread-safety)
- [License](#license)

---

## Features

- **Header-only**: No compilation required, just include and use
- **std::map-compatible API**: Familiar interface with iterators, `insert`, `erase`, `find`, etc.
- **Multiple key types**: Supports `std::string`, `char*`, and all numeric types
- **Ordered traversal**: Keys are always iterated in sorted order
- **Memory efficient**: Compact node representation (8 bytes per node)
- **Cache-friendly**: Contiguous node arrays minimize cache misses
- **No external dependencies**: Uses only C++20 standard library

---

## Requirements

- **C++20 compiler** (minimum) with support for:
  - Concepts
  - `std::bit_cast`
  - `std::endian`
  - Structured bindings
  - `[[nodiscard]]` attribute

- **C++23 compiler** (recommended) for native feature support

- **Tested compilers**:
  - GCC 10+ (C++20), GCC 13+ (C++23)
  - Clang 12+ (C++20), Clang 16+ (C++23)
  - MSVC 2019 16.10+ (C++20), MSVC 2022 17.4+ (C++23)

### C++20 vs C++23 Feature Usage

KTRIE is fully **C++23 compliant** and will automatically use native C++23 features when available, falling back to compiler intrinsics or polyfills for C++20:

| Feature | C++20 (Fallback) | C++23 (Native) |
|---------|------------------|----------------|
| **Byte swap** | `__builtin_bswap64()` (GCC/Clang)<br>`_byteswap_uint64()` (MSVC) | `std::byteswap()` |
| **Assume hint** | `__builtin_assume()` (GCC/Clang)<br>`__assume()` (MSVC) | `[[assume(expr)]]` |
| **Unreachable** | `__builtin_unreachable()` (GCC/Clang)<br>`__assume(false)` (MSVC) | `std::unreachable()` |

#### Detection Mechanism

```cpp
// Byte swap (ktrie_num_cvt.h, ktrie_defines.h)
#if __cpp_lib_byteswap >= 202110L
    return std::byteswap(inp);        // C++23 native
#elif defined(_MSC_VER)
    return _byteswap_uint64(inp);     // MSVC intrinsic
#else
    return __builtin_bswap64(inp);    // GCC/Clang intrinsic
#endif

// Assume hint (ktrie_defines.h)
#if __has_cpp_attribute(assume)
    #define KTRIE_ASSUME(cond) [[assume((cond))]]    // C++23 native
#elif defined(_MSC_VER)
    #define KTRIE_ASSUME(cond) __assume((cond))      // MSVC intrinsic
#else
    #define KTRIE_ASSUME(cond) __builtin_assume((cond))  // GCC/Clang
#endif
```

#### Compile-Time Feature Macros Used

| Macro | Standard | Purpose |
|-------|----------|---------|
| `__cpp_lib_byteswap` | C++23 | Detect `std::byteswap` availability |
| `__has_cpp_attribute(assume)` | C++23 | Detect `[[assume]]` attribute |
| `__cpp_lib_unreachable` | C++23 | Detect `std::unreachable` availability |

#### Building for C++23

```bash
# GCC 13+
g++ -std=c++23 -O2 -o myapp myapp.cpp

# Clang 16+
clang++ -std=c++23 -O2 -o myapp myapp.cpp

# MSVC 2022
cl /std:c++latest /O2 /EHsc myapp.cpp
```

---

## Installation

KTRIE is header-only. Simply copy all `.h` files to your project's include path.

```bash
# Clone or download, then copy headers
cp ktrie*.h /your/project/include/
```

**Minimum required file:** `ktrie.h` (includes all dependencies automatically)

---

## Quick Start

### Basic Usage with String Keys

```cpp
#include "ktrie.h"
using namespace gteitelbaum;

int main() {
    // Create a trie mapping strings to integers
    ktrie<std::string, int> phonebook;
    
    // Insert entries
    phonebook.insert({"Alice", 1234});
    phonebook.insert({"Bob", 5678});
    phonebook["Charlie"] = 9999;
    
    // Lookup
    if (phonebook.contains("Alice")) {
        std::cout << "Alice: " << phonebook.at("Alice") << "\n";
    }
    
    // Iterate in sorted order
    for (const auto& [name, number] : phonebook) {
        std::cout << name << ": " << number << "\n";
    }
    
    // Erase
    phonebook.erase("Bob");
    
    return 0;
}
```

### Numeric Keys

```cpp
#include "ktrie.h"
using namespace gteitelbaum;

int main() {
    // Integer keys are automatically sorted
    ktrie<int, std::string> numbers;
    
    numbers.insert({42, "forty-two"});
    numbers.insert({-10, "minus ten"});
    numbers.insert({100, "hundred"});
    
    // Iterate in numeric order
    for (const auto& [num, name] : numbers) {
        std::cout << num << ": " << name << "\n";
    }
    // Output:
    // -10: minus ten
    // 42: forty-two
    // 100: hundred
    
    return 0;
}
```

### Value Types

All Value types are valid

Values with size less than or equal to eight are stored by value

Values of size greater than eight are stored with a pointer

Values that are pointers fall into the category of 'less than or equal to eight'.
This means the lements beng pointed to are stored by reference since the pointer is copied not the value.
KTRIEs of pointer to X, `KTRIE<X *>`, will not take ownership of any value pointer.
If an element of type X is inserted into a `KTRIE<X *>` by pointer and the value is changed outside of KTRIE,
the element value will be changed inside of the KTRIE.
This might be desirable and would take less space if sizeof(X) is greater than eight.
If the value pointed to is deleted, it will be corrupt in the KTRIE. This is undefined behaviour to be avoided.
Instead use a `KTRIE<X>` instead of `KTRIE<X *>` and dereference the `X*` before insertion.

---

## API Reference

### Class Template

```cpp
template <class Key, class T, class Allocator = std::allocator<T>>
class ktrie;
```

### Template Specializations

| Key Type | Description |
|----------|-------------|
| `std::string` | Variable-length string keys with full STL container interface |
| `char*` | Raw pointer interface for performance-critical applications |
| Numeric types | `int`, `long`, `int64_t`, `uint64_t`, etc. with automatic byte-order handling |

### Core Operations

```cpp
// Element access
T& at(const Key& key);
T& operator[](const Key& key);

// Capacity
bool empty() const noexcept;
size_type size() const noexcept;

// Modifiers
void clear() noexcept;
std::pair<iterator, bool> insert(const value_type& value);
template <class... Args>
std::pair<iterator, bool> emplace(Args&&... args);
std::pair<iterator, bool> insert_or_assign(const Key& key, const T& value);
iterator erase(const_iterator pos);
size_type erase(const Key& key);

// Lookup
iterator find(const Key& key);
bool contains(const Key& key) const;
size_type count(const Key& key) const;

// Ordered operations
iterator lower_bound(const Key& key);
iterator upper_bound(const Key& key);
std::pair<iterator, iterator> equal_range(const Key& key);

// Iterators
iterator begin();
iterator end();
reverse_iterator rbegin();
reverse_iterator rend();
```

---

## Key Types

### String Keys (`std::string`)

String keys are stored using path compression:
- Short sequences (1-6 chars) use inline **HOP** nodes
- Longer sequences use **SKIP** nodes with external storage
- Common prefixes are shared between keys

### Numeric Keys

Numeric keys are converted to big-endian byte representation for correct lexicographic ordering. Supported types:
- `int8_t`, `int16_t`, `int32_t`, `int64_t`
- `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`
- `float`, `double`

### Raw Pointer Keys (`char*`)

The `char*` specialization provides a minimal interface for maximum performance:

```cpp
const T* insert(const char* key, size_t len, const T& value);
const T* find(const char* key, size_t len) const;
size_type erase(const char* key, size_t len);
bool contains(const char* key, size_t len) const;
```

---

## Header Files and Include Hierarchy

### File Descriptions

| File | Purpose | Include From |
|------|---------|--------------|
| `ktrie.h` | **Main public header** - include this | User code |
| `ktrie_base.h` | Core trie implementation | ktrie.h |
| `ktrie_node.h` | Node structure and basic operations | Internal |
| `ktrie_defines.h` | Macros, types, utility functions | Internal |
| `ktrie_data_ptr.h` | Type-erased value storage | Internal |
| `ktrie_dirty_high_pointer.h` | Pointer with embedded flags | Internal |
| `ktrie_flags_loc.h` | Flag location abstraction | Internal |
| `ktrie_hop.h` | HOP node (1-6 char inline) | Internal |
| `ktrie_skip.h` | SKIP node (long strings) | Internal |
| `ktrie_small_list.h` | LIST node (≤7 children) | Internal |
| `ktrie_insert_help.h` | Insert operation helpers | Internal |
| `ktrie_remove_help.h` | Remove operation helpers | Internal |
| `ktrie_nav.h` | Navigation (next/prev) helpers | Internal |
| `ktrie_iter.h` | Iterator implementation | Internal |
| `ktrie_num_cvt.h` | Numeric key conversion | Internal |
| `ktrie_pretty.h` | Debug visualization | Internal |

### Include File Dependency Graph

```
ktrie.h (main public header - include this)
├── ktrie_base.h
│   ├── ktrie_insert_help.h
│   │   ├── ktrie_flags_loc.h
│   │   │   ├── ktrie_defines.h
│   │   │   ├── ktrie_hop.h
│   │   │   │   └── ktrie_defines.h
│   │   │   └── ktrie_skip.h
│   │   │       └── ktrie_defines.h
│   │   └── ktrie_node.h
│   │       ├── ktrie_data_ptr.h
│   │       │   └── ktrie_defines.h
│   │       ├── ktrie_defines.h
│   │       ├── ktrie_dirty_high_pointer.h
│   │       │   └── ktrie_defines.h
│   │       ├── ktrie_hop.h
│   │       ├── ktrie_skip.h
│   │       └── ktrie_small_list.h
│   │           └── ktrie_defines.h
│   ├── ktrie_iter.h
│   │   ├── ktrie_nav.h
│   │   │   └── ktrie_node.h
│   │   └── ktrie_node.h
│   ├── ktrie_nav.h
│   ├── ktrie_node.h
│   ├── ktrie_pretty.h
│   │   └── ktrie_node.h
│   └── ktrie_remove_help.h
│       ├── ktrie_flags_loc.h
│       ├── ktrie_insert_help.h
│       └── ktrie_node.h
├── ktrie_iter.h
└── ktrie_num_cvt.h
```

### Include Guard Pattern

All headers use `#pragma once` for include guards.

---

## Macros and Symbols

### Defined Macros

KTRIE defines the following macros. Ensure these don't conflict with your codebase:

| Macro | Defined In | Purpose |
|-------|-----------|---------|
| `KTRIE_FORCE_INLINE` | `ktrie_defines.h` | Forces function inlining |
| `KTRIE_PREFETCH(addr)` | `ktrie_defines.h` | CPU prefetch hint |
| `KTRIE_ASSUME(cond)` | `ktrie_defines.h` | Optimizer hint |
| `KTRIE_DEBUG_ASSERT(x)` | `ktrie_defines.h` | Debug assertion |

### Macro Definitions

```cpp
// ktrie_defines.h

// Force inline (compiler-specific)
#if defined(_MSC_VER)
#define KTRIE_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define KTRIE_FORCE_INLINE inline __attribute__((always_inline))
#else
#define KTRIE_FORCE_INLINE inline
#endif

// Prefetch (compiler-specific)
#if defined(__GNUC__) || defined(__clang__)
#define KTRIE_PREFETCH(addr) __builtin_prefetch(addr, 0, 3)
#elif defined(_MSC_VER)
#include <intrin.h>
#define KTRIE_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
#define KTRIE_PREFETCH(addr) ((void)0)
#endif

// Assume hint (C++23 or compiler-specific)
#if __has_cpp_attribute(assume)
#define KTRIE_ASSUME(cond) [[assume((cond))]]
#elif defined(_MSC_VER)
#define KTRIE_ASSUME(cond) __assume((cond))
#else
#define KTRIE_ASSUME(cond) __builtin_assume((cond))
#endif

// Debug assertion
#ifdef NDEBUG
#define KTRIE_DEBUG_ASSERT(x) KTRIE_ASSUME(x)
#else
#include <cassert>
#define KTRIE_DEBUG_ASSERT(x) assert(x)
#endif
```

### Avoiding Conflicts

If you have existing macros with these names, you can:

1. **Rename KTRIE macros** before including:
```cpp
// If you have your own KTRIE_FORCE_INLINE
#define MY_KTRIE_FORCE_INLINE KTRIE_FORCE_INLINE
#undef KTRIE_FORCE_INLINE
#include "ktrie.h"
#define KTRIE_FORCE_INLINE MY_KTRIE_FORCE_INLINE
```

2. **Wrap in namespace** (macros are still global, but types are namespaced):
```cpp
#include "ktrie.h"
// All KTRIE types are in namespace gteitelbaum
using gteitelbaum::ktrie;
```

### Namespace

All KTRIE types and functions are in the `gteitelbaum` namespace:

```cpp
namespace gteitelbaum {
    template <class Key, class T, class A> class ktrie;
    template <class T, size_t fixed_len, class A> class ktrie_base;
    template <class T, size_t fixed_len, class A> class node;
    // ... etc.
}
```

### Type Aliases and Enums

| Symbol | Type | Defined In |
|--------|------|-----------|
| `t_flag` | `uint8_t` | `ktrie_defines.h` |
| `t_val` | `uint64_t` | `ktrie_node.h` |
| `intptr_type` | `std::uintptr_t` | `ktrie_defines.h` |
| `eos_bit` | `enum flags` | `ktrie_defines.h` |
| `skip_bit` | `enum flags` | `ktrie_defines.h` |
| `hop_bit` | `enum flags` | `ktrie_defines.h` |
| `list_bit` | `enum flags` | `ktrie_defines.h` |
| `pop_bit` | `enum flags` | `ktrie_defines.h` |
| `num_bits` | `enum flag_count` | `ktrie_defines.h` |

### Concepts

| Concept | Purpose | Defined In |
|---------|---------|-----------|
| `numeric` | Any arithmetic type | `ktrie_num_cvt.h` |
| `signed_numeric` | Signed arithmetic types | `ktrie_num_cvt.h` |
| `unsigned_numeric` | Unsigned arithmetic types | `ktrie_num_cvt.h` |
| `small_class` | Types ≤ pointer size | `ktrie_data_ptr.h` |
| `big_class` | Types > pointer size | `ktrie_data_ptr.h` |

---

## Documentation

For detailed explanations of KTRIE internals, see these companion documents:

### [Architecture Guide](./architecture.md)

Comprehensive technical documentation including:
- Visual diagrams of all node layouts (POINTER, HOP, SKIP, LIST, POP, EOS)
- State machine for node interpretation
- Flag bit interaction rules
- Step-by-step example: storing "hello"
- Complete pseudocode for INSERT, DELETE, SEARCH algorithms
- State transition diagrams for all transformations
- Path compression algorithm details
- Numeric key optimization explanation

### [Implementation Concepts](./concepts.md)

Deep dive into the techniques used:
- Traditional trie structure and its memory problems
- **Dirty High Pointer** technique (embedding flags in pointer bits)
- **SWAR** (SIMD Within A Register) for fast byte operations
- **Numeric key conversion** (endianness and signed number handling)
- **Data pointer optimization** (inline storage for small values)

### [Comparison Guide](./comparisons.md)

KTRIE vs `std::map` vs `std::unordered_map`:
- Architecture comparison with diagrams
- Memory overhead analysis
- API feature matrix
- Cache behavior and latency expectations
- Performance complexity (O notation)
- Selection criteria and decision matrix
- Migration guide with code examples
- Performance estimates and benchmarks

### [Internal API Reference](./internal_api.md)

Complete Doxygen-style documentation for all internal classes:
- Core classes (`ktrie`, `ktrie_base`, `node`)
- Node types (`t_hop`, `t_skip`, `t_small_list`)
- Pointer classes (`dirty_high_pointer`, `data_ptr`, `flags_location`)
- Helper classes (`insert_helper`, `remove_helper`, `nav_helper`)
- Iterator classes and numeric conversion
- All methods, parameters, and return types documented

---

## Architecture

### Node Structure

Each node is exactly 8 bytes, interpreted based on context:

```
┌─────────────────────────────────────────────────────────────┐
│                      64-bit Node Value                       │
├─────────────────────────────────────────────────────────────┤
│ • POINTER: [5-bit flags][59-bit pointer address]            │
│ • HOP:     [6 chars][new_flags][length]                     │
│ • SKIP:    [new_flags in high bits][length in low bits]     │
│ • LIST:    [7 sorted chars][count]                          │
│ • POP:     4 consecutive nodes form 256-bit bitmap          │
│ • DATA:    Stored value (inline or pointer based on size)   │
└─────────────────────────────────────────────────────────────┘
```

### Flag Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `EOS` | End-of-string marker; next node contains value |
| 1 | `SKIP` | Long string; next node(s) contain string data |
| 2 | `HOP` | Short string (1-6 chars) inline in node |
| 3 | `LIST` | Small branch point (≤7 children) |
| 4 | `POP` | Large branch point (8+ children) via bitmap |

For detailed architecture information, see [architecture.md](./architecture.md).

---

## Performance Characteristics

### Time Complexity

| Operation | Average | Worst Case |
|-----------|---------|------------|
| `find` | O(k) | O(k) |
| `insert` | O(k) | O(k) |
| `erase` | O(k) | O(k) |
| `lower_bound` | O(k) | O(k) |
| Iteration step | O(1) amortized | O(log n) |

Where **k** = key length, **n** = number of elements.

### When to Use KTRIE

✅ **Good fit**:
- Prefix-heavy string keys (URLs, file paths, words)
- Need for ordered iteration
- Range queries (`lower_bound`, `upper_bound`)
- Memory-constrained environments with many similar keys

❌ **Consider alternatives**:
- Random access patterns with no ordering needs → `std::unordered_map`
- Very short, unique keys → `std::map`
- Need for custom comparators or key types

For detailed comparisons, see [comparisons.md](./comparisons.md).

---

## Memory Layout

### Value Storage

Values are stored based on their size:

| Value Size | Storage Method |
|------------|----------------|
| ≤ 8 bytes | Inline in node |
| > 8 bytes | Heap-allocated, node stores pointer |

### Allocator Support

KTRIE uses the provided allocator for node array and large value allocation:

```cpp
ktrie<std::string, BigStruct, MyAllocator<BigStruct>> trie(my_allocator);
```

---

## Thread Safety

KTRIE provides the same thread-safety guarantees as standard library containers:

- **Multiple readers**: Safe
- **Single writer**: Safe (while no readers)
- **Concurrent modification**: Undefined behavior

For concurrent access, use external synchronization.

---

## Debug Utilities

### Pretty Printing

```cpp
ktrie<std::string, int> trie;
// ... insert data ...

trie.pretty_print();        // Full tree structure
trie.pretty_print(true);    // Summary statistics only
```

---

## Building Tests

```bash
# GCC/Clang
g++ -std=c++20 -O2 -o ktrie_test ktrie.cpp

# With sanitizers
g++ -std=c++20 -O0 -g -fsanitize=address,undefined -o ktrie_test ktrie.cpp

# MSVC
cl /std:c++20 /O2 /EHsc ktrie.cpp
```
---

## AI Usage

Documentation, comments, refactoring and bug detection were assisted by [Claude](https://claude.ai/).
It was helpful in converting recursive functions to iterative versions. It also gave an unbiased view for the comparison document.
Any incorrect or missing documentation or remaining bugs are ultimately the responsibility of the author.

---

## Inspiration

Much of this was inspired directly or indirectly from [Bit Twiddling Hacks](https://graphics.stanford.edu/~seander/bithacks.html)

---

## License

This code is licensed under the Creative Commons Attribution 4.0 International License (CC BY 4.0).
You must give appropriate credit, provide a link to the license, and indicate if changes were made.

Original Author: Glenn Teitelbaum

Source: [GitHub Repository](https://github.com/glennteitelbaum/KTRIE)

---

*Document Version: 2.0*  
*Last Updated: December 2024*
