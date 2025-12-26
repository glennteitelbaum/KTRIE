# KTRIE Implementation Concepts

This document explains the key implementation techniques used in KTRIE. Understanding these concepts will help you appreciate the design decisions and trade-offs that make KTRIE efficient.

## Table of Contents

1. [Traditional Trie Structure](#traditional-trie-structure)
2. [Dirty High Pointer Technique](#dirty-high-pointer-technique)
3. [SWAR - SIMD Within A Register](#swar---simd-within-a-register)
4. [Numeric Key Conversion](#numeric-key-conversion)
5. [Data Pointer Optimization](#data-pointer-optimization)

---

## Traditional Trie Structure

### What is a Trie?

A trie (pronounced "try") is a tree data structure used for storing strings where each node represents a single character. The term comes from "re**trie**val" because tries excel at prefix-based lookups.

### The Classic 256-Pointer Node

The simplest trie implementation uses an array of 256 pointers at each node—one for each possible byte value (0-255):

```cpp
// Traditional trie node
struct TrieNode {
    TrieNode* children[256];  // One pointer per possible character
    bool is_end_of_word;      // Marks complete keys
    Value* value;             // Stored value (if is_end_of_word)
};
```

**Visual representation:**

```
             [Root Node]
              children[256]
    ┌──────────┼──────────┐
    │          │          │
   [0]  ...  ['h'=104]  [255]
    │          │          │
  null        ↓        null
           [Node 'h']
            children[256]
    ┌──────────┼──────────┐
    │          │          │
   [0]  ... ['e'=101]  [255]
    │          │          │
  null        ↓        null
           [Node 'e']
              ...
```

### Memory Problem with 256-Pointer Nodes

Each node requires:
```
256 pointers × 8 bytes = 2,048 bytes per node!
```

For a trie storing 10,000 words averaging 8 characters each:
```
Worst case: 80,000 nodes × 2,048 bytes = 160 MB!
```

This is **extremely wasteful** because most of those 256 pointers are null. In practice, each node typically has only 2-10 non-null children.

### How KTRIE Solves This

KTRIE uses several techniques to compress this:

1. **LIST nodes**: For ≤7 children, store a sorted list of characters (8 bytes total)
2. **POP nodes**: For 8+ children, use a 256-bit bitmap (32 bytes) instead of 256 pointers
3. **HOP/SKIP**: Collapse chains of single-child nodes into one node
4. **Contiguous arrays**: Store related nodes together for cache efficiency

**KTRIE memory for the same 10,000 words:**
```
Typical: ~200-400 KB (400-800× smaller!)
```

---

## Dirty High Pointer Technique

### The Problem: Where to Store Flags?

KTRIE nodes need to know what type of data they contain:
- Is the next node an EOS (end-of-string with value)?
- Is it a HOP (1-6 inline characters)?
- Is it a SKIP (longer string)?
- Is it a LIST (branch with ≤7 children)?
- Is it a POP (branch with 8+ children)?

We need 5 flag bits per pointer. The naive approach would be:

```cpp
// Naive approach - wastes memory
struct NodeWithFlags {
    uint8_t flags;        // 5 bits used, 3 wasted
    uint8_t padding[7];   // Alignment padding
    Node* pointer;        // 8 bytes
};
// Total: 16 bytes per pointer (8 bytes wasted!)
```

### The Solution: Embed Flags in Unused Pointer Bits

Modern 64-bit systems don't actually use all 64 bits for memory addresses:

```
64-bit pointer layout (x86-64, current):

┌─────────────────────────────────────────────────────────────────┐
│ Unused (16 bits) │        Virtual Address (48 bits)            │
└─────────────────────────────────────────────────────────────────┘
  bits 63-48              bits 47-0

We can use 5 of those unused high bits for our flags:

┌───────────┬───────────┬────────────────────────────────────────┐
│ Flags (5) │ Unused(11)│        Virtual Address (48 bits)       │
└───────────┴───────────┴────────────────────────────────────────┘
  bits 63-59  bits 58-48           bits 47-0
```

### Implementation in KTRIE

From `ktrie_dirty_high_pointer.h`:

```cpp
template <class T, class U>
class dirty_high_pointer {
    uint64_t raw_;  // Combined pointer and flags
    
    static constexpr uint64_t ptr_mask = (1ULL << 59) - 1;  // Low 59 bits
    static constexpr int flag_shift = 59;                    // Flags in high 5 bits

public:
    // Extract the pointer (mask off flag bits)
    T* get_ptr() const { 
        return reinterpret_cast<T*>(raw_ & ptr_mask); 
    }
    
    // Extract the flags (shift down from high bits)
    U get_byte() const { 
        return static_cast<U>(raw_ >> flag_shift); 
    }
    
    // Set the pointer (preserve flags)
    void set_ptr(T* p) {
        raw_ = (raw_ & ~ptr_mask) | (reinterpret_cast<uint64_t>(p) & ptr_mask);
    }
    
    // Set the flags (preserve pointer)
    void set_byte(U x) {
        raw_ = (raw_ & ptr_mask) | (static_cast<uint64_t>(x) << flag_shift);
    }
    
    // Get both at once (common operation, avoids redundant masking)
    std::pair<T*, U> get_both() const {
        return {
            reinterpret_cast<T*>(raw_ & ptr_mask),
            static_cast<U>(raw_ >> flag_shift)
        };
    }
};
```

### Why This is Safe

#### Current x86-64 Architecture (48-bit addressing)

On current x86-64 processors:
- Virtual addresses use only 48 bits (bits 0-47)
- Bits 48-63 must be **sign-extended** from bit 47
- This means they're either all 0s (user space) or all 1s (kernel space)
- User space addresses always have bits 48-63 = 0
- We have **16 free bits** to use

```
User space address:    0x00007FFF_FFFFFFFF (max)
                       ↑↑↑↑
                       These high bits are always 0 in user space
```

#### Which Architectures Support This?

| Architecture | Address Bits | Free High Bits | KTRIE Compatible |
|--------------|--------------|----------------|------------------|
| x86-64 (current) | 48 | 16 | ✅ Yes |
| x86-64 (LA57) | 57 | 7 | ✅ Yes (5 bits ok) |
| ARM64 | 48 (configurable) | 16 | ✅ Yes |
| ARM64 (52-bit VA) | 52 | 12 | ✅ Yes |
| RISC-V (Sv48) | 48 | 16 | ✅ Yes |
| RISC-V (Sv57) | 57 | 7 | ✅ Yes (5 bits ok) |

#### Future Proofing: 57-bit Pointers

Intel's **5-Level Paging (LA57)** extends virtual addresses to 57 bits:

```
Future x86-64 with LA57:

┌───────────┬────────────────────────────────────────────────────┐
│ Free (7)  │           Virtual Address (57 bits)                │
└───────────┴────────────────────────────────────────────────────┘
  bits 63-57              bits 56-0
```

**KTRIE uses only 5 bits**, so it remains safe even with 57-bit addressing:
- LA57 leaves 7 free bits
- KTRIE needs 5 bits
- **2 bits to spare** even on future hardware

### Memory Savings

Without dirty pointers (separate flags):
```cpp
struct NodeArray {
    uint8_t flags;           // 1 byte
    uint8_t padding[7];      // 7 bytes (alignment)
    Node* ptr;               // 8 bytes
};
// Total: 16 bytes minimum overhead per node array reference
```

With dirty pointers:
```cpp
// Just the pointer itself, flags embedded
uint64_t dirty_ptr;  // 8 bytes total
// Total: 8 bytes (50% savings!)
```

For a trie with 100,000 internal pointers:
```
Without dirty pointers: 1.6 MB overhead
With dirty pointers:    0.8 MB overhead
Savings: 800 KB (50%)
```

### Trade-offs vs Separate Flag Storage

| Aspect | Dirty High Pointer | Separate Flags |
|--------|-------------------|----------------|
| **Memory** | 8 bytes | 16 bytes (with padding) |
| **Access speed** | Requires masking | Direct access |
| **Portability** | Architecture-dependent | Fully portable |
| **Debugging** | Harder (values look wrong) | Easier |
| **Could store size** | No room | Yes, could add |

**Alternative design considered:**
```cpp
// Could store array size, avoiding calculation
struct NodeArrayHeader {
    uint8_t flags;
    uint8_t reserved;
    uint16_t array_size;  // No need to calculate!
    uint32_t padding;
};
// But this costs 8 extra bytes per array
```

KTRIE chose dirty pointers because memory efficiency was prioritized, and the `node_array_sz()` calculation is infrequent compared to pointer traversal.

### Portability Concerns

**Potential issues:**

1. **32-bit systems**: Not supported (pointers are only 32 bits)
2. **Exotic architectures**: May use different pointer layouts
3. **Future hardware**: Could theoretically use more than 59 bits (unlikely)
4. **Pointer provenance**: Some compilers/sanitizers may flag this

**Mitigations in KTRIE:**

```cpp
// ktrie.h includes this check
static_assert((std::endian::native == std::endian::big) ||
              (std::endian::native == std::endian::little),
              "Irregular Endian not supported");

// Could add:
static_assert(sizeof(void*) == 8, "64-bit pointers required");
```

---

## SWAR - SIMD Within A Register

### What is SWAR?

**SWAR** (SIMD Within A Register) is a technique for performing parallel operations on multiple small values packed into a single large register. Instead of processing one byte at a time, we process 8 bytes simultaneously using 64-bit integer operations.

```
Traditional approach (8 operations):
  byte[0] == target?  → check
  byte[1] == target?  → check
  byte[2] == target?  → check
  ...
  byte[7] == target?  → check

SWAR approach (1-2 operations):
  All 8 bytes checked simultaneously using bit manipulation
```

### Why KTRIE Uses SWAR

KTRIE's `t_small_list` stores up to 7 sorted characters in a single 64-bit value:

```cpp
// t_small_list layout (8 bytes):
┌────┬────┬────┬────┬────┬────┬────┬───────┐
│ c0 │ c1 │ c2 │ c3 │ c4 │ c5 │ c6 │ count │
└────┴────┴────┴────┴────┴────┴────┴───────┘
  Sorted characters (7 bytes)       List size
```

When looking up a character, we need to find its position in this list. A naive loop would be:

```cpp
// Naive approach: 1-7 comparisons
int offset(char c) const {
    for (int i = 0; i < count; i++) {
        if (chars[i] == c) return i + 1;
    }
    return 0;  // Not found
}
```

### SWAR Implementation in offset()

From `ktrie_small_list.h`:

```cpp
int offset(char c) const {
    // Step 1: Replicate search byte to all positions
    constexpr uint64_t rep = 0x01'01'01'01'01'01'01'00ULL;
    uint64_t x = static_cast<uint8_t>(c);
    uint64_t search = rep * x;  // c copied to bytes 0-6, byte 7 stays 0
    
    // Example: searching for 'e' (0x65)
    // search = 0x65656565656565_00
    
    // Step 2: XOR with list - matching bytes become 0x00
    uint64_t diff = n_ ^ search;
    
    // If list is "abcdefg" and we search for 'e':
    // n_   = 0x61626364_65666707  ('a','b','c','d','e','f','g', count=7)
    // diff = 0x04070601_00030200  (only byte 4 is 0x00!)
    
    // Step 3: Find zero bytes using SWAR magic
    constexpr uint64_t low_bits = 0x7F'7F'7F'7F'7F'7F'7F'7FULL;
    uint64_t zeros = ~((((diff & low_bits) + low_bits) | diff) | low_bits);
    
    // Step 4: Count leading zeros to find position
    int pos = std::countl_zero(zeros) / 8;
    
    return (pos + 1 <= get_list_sz()) ? pos + 1 : 0;
}

```

### How the Zero-Finding Magic Works

See [***Bit Twiddling Hacks***](https://graphics.stanford.edu/~seander/bithacks.html) for more details on finding zero bytes.
This Algorythm was derived from '***Determine if a word has a byte equal to n***'
Using '***Determine if a word has a zero byte***'
But it cannot use the more efficient '***Determine if a word has a byte less than n***' since it only works for `x>=0; 0<=n<=128`

The expression `~((((diff & low_bits) + low_bits) | diff) | low_bits)` finds zero bytes. Let's break it down:

```
Goal: Set high bit of each byte that is 0x00, clear for non-zero bytes

For each byte B in diff:

1. (B & 0x7F)         - Clear high bit
2. + 0x7F             - Add 127
3. | B                - OR with original
4. | 0x7F             - OR with 0x7F
5. ~(...)             - Invert

Truth table:
┌──────────┬────────────────────────────────────────┬────────────┐
│ B value  │ Steps                                  │ Result bit │
├──────────┼────────────────────────────────────────┼────────────┤
│ 0x00     │ (0+127)|0|127 = 127 = 0x7F → ~0x7F=0x80│ 1 (found!) │
│ 0x01     │ (1+127)|1|127 = 128|1|127 = 0xFF → 0x00│ 0          │
│ 0x7F     │ (127+127)|127|127 = 0xFE|0x7F = 0xFF   │ 0          │
│ 0x80     │ (0+127)|128|127 = 0xFF                 │ 0          │
│ 0xFF     │ (127+127)|255|127 = 0xFF               │ 0          │
└──────────┴────────────────────────────────────────┴────────────┘
```

Only 0x00 bytes result in a high bit being set!

### SWAR in insert()

The `insert()` function uses SWAR for **unsigned byte comparison** to find the insertion position:

```cpp
int insert(int len, char c) {
    // Need to find: how many existing chars are less than c?
    
    constexpr uint64_t h = 0x80'80'80'80'80'80'80'80ULL;  // High bits
    constexpr uint64_t m = 0x7F'7F'7F'7F'7F'7F'7F'7FULL;  // Low bits
    
    // Replicate c to all positions
    uint64_t rep_x = (REP * static_cast<uint8_t>(c)) & valid_mask;
    
    // SWAR unsigned comparison: find bytes where chars[i] < c
    // This is tricky because bytes are unsigned!
    
    // Case 1: High bits differ - larger value has high bit set
    uint64_t diff_high = (chars ^ rep_x) & h;
    uint64_t B_high_wins = rep_x & diff_high;
    
    // Case 2: High bits same - compare low 7 bits
    uint64_t same_high = ~diff_high & h;
    uint64_t low_cmp = ~((low_chars | h) - low_x) & h;
    
    // Combine results
    uint64_t lt = (B_high_wins | (same_high & low_cmp)) & valid_mask;
    
    // Count bytes where existing < new
    int pos = std::popcount(lt);
    
    // ... shift bytes and insert at pos ...
}
```

### Performance Implications

**Without SWAR:**
```cpp
// 7 iterations worst case
for (int i = 0; i < 7; i++) {
    if (list[i] >= c) return i;
}
// ~7 comparisons, 7 branches
// Branch mispredictions cost 10-20 cycles each
```

**With SWAR:**
```cpp
// Fixed ~5-6 operations regardless of list size
uint64_t diff = n_ ^ search;
uint64_t zeros = ~((((diff & low_bits) + low_bits) | diff) | low_bits);
int pos = std::countl_zero(zeros) / 8;
// No branches, fully predictable
// ~5-10 cycles total
```

**Benchmark comparison:**

| List Size | Naive Loop | SWAR |
|-----------|------------|------|
| 1 char | ~3 cycles | ~6 cycles |
| 4 chars | ~12 cycles | ~6 cycles |
| 7 chars | ~21 cycles | ~6 cycles |

SWAR is **constant time** regardless of list size, and eliminates branch mispredictions.

---

## Numeric Key Conversion

### The Problem: Byte Order and Signed Numbers

KTRIE stores all keys as byte sequences and compares them lexicographically (byte-by-byte from left to right). This works naturally for strings, but numeric keys have two issues:

1. **Endianness**: Different CPUs store multi-byte numbers differently
2. **Signed numbers**: Two's complement doesn't sort correctly as bytes

### Endianness Explained

**Little-endian** (x86, ARM default): Least significant byte first
```
int32_t x = 0x12345678;

Memory layout (little-endian):
Address:  [0]  [1]  [2]  [3]
Value:    78   56   34   12   ← LSB first!
```

**Big-endian** (network order, some ARM modes): Most significant byte first
```
int32_t x = 0x12345678;

Memory layout (big-endian):
Address:  [0]  [1]  [2]  [3]
Value:    12   34   56   78   ← MSB first!
```

### Why This Matters for Tries

For correct lexicographic ordering, we need the **most significant byte first**:

```
Comparing 256 vs 1:
  256 = 0x00000100
  1   = 0x00000001
  
Little-endian comparison (WRONG):
  256: [00][01][00][00]
  1:   [01][00][00][00]
  First byte: 0x00 < 0x01 → 256 < 1  ← WRONG!
  
Big-endian comparison (CORRECT):
  256: [00][00][01][00]
  1:   [00][00][00][01]
  First bytes equal, then: 0x01 > 0x00 → 256 > 1  ← CORRECT!
```

### Solution: Always Convert to Big-Endian

From `ktrie_num_cvt.h`:

```cpp
template <typename T>
T do_byteswap(T inp) {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(inp);  // Swap byte order
    }
    return inp;  // Already big-endian
}
```

### The Two's Complement Problem

For **signed integers**, there's another issue. Two's complement representation puts negative numbers at the "top" of the bit range:

```
Signed int8_t values and their bit patterns:

Value    Bits      Unsigned equivalent
-----    ----      -------------------
 -128    10000000  128
 -1      11111111  255
  0      00000000  0
  1      00000001  1
  127    01111111  127
```

If we compare bytes directly:
```
-1 = 0xFF = 255
 1 = 0x01 = 1

Byte comparison: 255 > 1 → -1 > 1  ← WRONG!
```

### Solution: Flip the Sign Bit

By XORing with the sign bit, we convert two's complement to "offset binary" encoding where the byte values sort correctly:

```cpp
// For signed types
static unsign make_sortable(T inp) {
    unsign uns = std::numeric_limits<T>::max();
    uns++;  // Now equals the sign bit position (e.g., 0x80 for int8_t)
    uns += inp;  // Add value (equivalent to XOR with sign bit for offset)
    return uns;
}
```

**Transformation for int8_t:**

| Original | Binary | Add 0x80 | Result | Sorts as |
|----------|--------|----------|--------|----------|
| -128 | 10000000 | +10000000 | 00000000 | 0 (first!) |
| -1 | 11111111 | +10000000 | 01111111 | 127 |
| 0 | 00000000 | +10000000 | 10000000 | 128 |
| 1 | 00000001 | +10000000 | 10000001 | 129 |
| 127 | 01111111 | +10000000 | 11111111 | 255 (last!) |

Now the transformed values sort in the correct numeric order!

### Complete Transformation Pipeline

```cpp
// Converting int32_t key to trie bytes:
int32_t key = -12345;

// Step 1: Handle sign (XOR with 0x80000000)
uint32_t unsigned_val = key + 0x80000000u;
// -12345 + 2147483648 = 2147471303 = 0x7FFFCFC7

// Step 2: Convert to big-endian
uint32_t big_endian = byteswap_if_le(unsigned_val);
// Little-endian machine: 0x7FFFCFC7 → 0xC7CF_FF7F

// Step 3: Reinterpret as bytes
char bytes[4] = {0x7F, 0xFF, 0xCF, 0xC7};  // Big-endian order

// These bytes now sort correctly in the trie!
```

### Reversing the Transformation

When iterating, we need to convert back:

```cpp
// From bytes back to int32_t:
char bytes[4] = {0x7F, 0xFF, 0xCF, 0xC7};

// Step 1: Reinterpret as uint32_t (big-endian)
uint32_t big_endian = 0x7FFFCFC7;

// Step 2: Convert to native endian
uint32_t unsigned_val = byteswap_if_le(big_endian);

// Step 3: Undo sign transformation
int32_t key = unsigned_val - 0x80000000u;
// 2147471303 - 2147483648 = -12345  ← Original value!
```

---

## Data Pointer Optimization

### The Problem: Value Storage Overhead

Storing values through pointers has overhead:

```cpp
// Traditional approach
struct Node {
    Value* value_ptr;  // Always a pointer, even for small values
};

// For Value = int:
// - Node stores 8-byte pointer
// - Pointer points to 4-byte int (plus allocation overhead)
// - Total: ~24 bytes for a 4-byte value!
```

### The Solution: Inline Small Values

If a value fits in 8 bytes (the size of a pointer), store it directly instead of allocating:

```cpp
// KTRIE approach
union NodeData {
    uint64_t raw;           // Raw bits
    T* pointer;             // For large types: pointer to heap
    T inline_value;         // For small types: value stored directly
};
```

### Implementation in KTRIE

From `ktrie_data_ptr.h`:

```cpp
// Concept to detect small types (≤ pointer size)
template <class T>
concept small_class = sizeof(T) <= sizeof(T*);

template <class T>
concept big_class = sizeof(T) > sizeof(T*);
```

**For small types (≤8 bytes):**

```cpp
template <small_class T, class A>
class data_ptr<T, A> {
    T val_;  // Value stored directly!
    
public:
    // Store value - just copy it
    static uint64_t make_val(const T* data) {
        if constexpr (std::is_integral_v<T>) {
            return static_cast<uint64_t>(*data);
        } else {
            // For float/double, preserve bit pattern
            return std::bit_cast<uint64_t>(*data);
        }
    }
    
    // Retrieve value
    static data_ptr make_data(uint64_t v) {
        data_ptr d;
        if constexpr (std::is_integral_v<T>) {
            d.val_ = static_cast<T>(v);
        } else {
            d.val_ = std::bit_cast<T>(v);
        }
        return d;
    }
    
    // No cleanup needed!
    static void destroy(uint64_t, A&) { }
};
```

**For large types (>8 bytes):**

```cpp
template <big_class T, class A>
class data_ptr<T, A> {
    T* ptr_;  // Must use pointer
    
public:
    // Allocate and copy
    static uint64_t make_val(const T* data, A& alloc) {
        T* np = alloc.allocate(1);
        std::construct_at(np, *data);
        return reinterpret_cast<uint64_t>(np);
    }
    
    // Must destroy when removing!
    static void destroy(uint64_t val, A& alloc) {
        T* p = reinterpret_cast<T*>(val);
        if (p) {
            std::destroy_at(p);
            alloc.deallocate(p, 1);
        }
    }
};
```

### Which Types Are "Small"?

| Type | sizeof | Storage Method |
|------|--------|----------------|
| `int8_t` | 1 | Inline |
| `int16_t` | 2 | Inline |
| `int32_t` | 4 | Inline |
| `int64_t` | 8 | Inline |
| `float` | 4 | Inline |
| `double` | 8 | Inline |
| `void*` | 8 | Inline |
| `std::array<char, 8>` | 8 | Inline |
| `std::string` | 32 | **Pointer** (SSO buffer) |
| `std::vector<int>` | 24 | **Pointer** |
| `long double` | 16 | **Pointer** (on most systems) |

### Why This Matters

**Without inline storage (100K int values):**
```
Node storage:     100,000 × 8 bytes = 800 KB
Value allocations: 100,000 × (4 + ~20 overhead) = 2.4 MB
Total: ~3.2 MB
Plus: 100,000 allocation calls (slow!)
```

**With inline storage (100K int values):**
```
Node storage with inline values: 100,000 × 8 bytes = 800 KB
Value allocations: 0
Total: 800 KB (4× less memory!)
Plus: 0 allocation calls (fast!)
```

### The bit_cast Detail for Floating Point

For floating-point values, we use `std::bit_cast` instead of `static_cast`:

```cpp
// Why not static_cast for double?
double pi = 3.14159;
uint64_t wrong = static_cast<uint64_t>(pi);  // wrong = 3 (truncated!)

uint64_t right = std::bit_cast<uint64_t>(pi);  // Preserves all bits!
// right = 0x400921F9F01B866E (IEEE 754 representation)

// Restore original:
double restored = std::bit_cast<double>(right);  // restored = 3.14159
```

`bit_cast` preserves the exact bit pattern, which is essential for round-trip storage.

---

## Summary

| Technique | What It Does | Memory Savings | Performance Impact |
|-----------|--------------|----------------|-------------------|
| **Dirty High Pointer** | Embeds 5 flag bits in pointer | 50% per pointer | Tiny (masking) |
| **SWAR** | Parallel byte operations | None | 3-4× faster lookups |
| **Numeric Key Conversion** | Enables sorted numeric keys | None | Small (conversion) |
| **Data Pointer (inline)** | Stores small values directly | 4× for small types | Faster (no allocation) |

These techniques combine to make KTRIE significantly more memory-efficient and often faster than traditional data structures, especially for string-heavy workloads with shared prefixes.

---

## See Also

- [README.md](./README.md) - Main documentation, API reference, quick start
- [Architecture Guide](./architecture.md) - Node layouts, algorithms, state machines
- [Comparison Guide](./comparisons.md) - vs std::map/unordered_map, performance, migration
- [Internal API Reference](./internal_api.md) - Full class and function documentation

---

*Document Version: 1.0*  
*Last Updated: December 2025*
