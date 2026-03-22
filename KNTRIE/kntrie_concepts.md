# KNTRIE Concepts

This document describes the KNTRIE, the integer-key instantiation of the KTRIE. For shared data structure concepts (TRIE, B-tree, KTRIE decomposition), binary search, bitmap representation, sentinel design, and value storage categories, see [KTRIE Concepts](../ktrie_concepts.md).

## Table of Contents

- [1 Optimizations](#1-optimizations)
  - [1.1 Key Representation](#11-key-representation)
  - [1.2 Value Storage: Implementation Details](#12-value-storage-implementation-details)
  - [1.3 Memory Hysteresis](#13-memory-hysteresis)
- [2 Node Concepts](#2-node-concepts)
  - [2.1 Bitmap Dispatch Modes](#21-bitmap-dispatch-modes)
  - [2.2 Node Header](#22-node-header)
  - [2.3 Tagged Pointers](#23-tagged-pointers)
  - [2.4 Descendants](#24-descendants)
  - [2.5 Leaf Contract](#25-leaf-contract)
- [3 Nodes](#3-nodes)
  - [3.1 Sentinel](#31-sentinel)
  - [3.2 Internal Nodes](#32-internal-nodes)
  - [3.3 Compact Leaves](#33-compact-leaves)
  - [3.4 Bitmap256 Leaves](#34-bitmap256-leaves)
  - [3.5 Root](#35-root)
- [4 Operations](#4-operations)
  - [4.1 Depth-Based Dispatch](#41-depth-based-dispatch)
  - [4.2 find](#42-find)
  - [4.3 find first / find last](#43-find-first--find-last)
  - [4.4 find next / find prev](#44-find-next--find-prev)
  - [4.5 insert](#45-insert)
  - [4.6 erase](#46-erase)
  - [4.7 modify](#47-modify)
  - [4.8 erase_when](#48-erase_when)
  - [4.9 Iterators are Snapshots](#49-iterators-are-snapshots)
- [5 Performance](#5-performance)
  - [5.1 std::map](#51-stdmap)
  - [5.2 std::unordered_map](#52-stdunordered_map)
  - [5.3 KNTRIE](#53-kntrie)
  - [5.4 The Real Complexity: Memory Hierarchy](#54-the-real-complexity-memory-hierarchy)
- [6 Summary](#6-summary)

## 1 Optimizations

### 1.1 Key Representation

All key types (`uint16_t`, `int32_t`, `uint64_t`, signed or unsigned) are transformed into a canonical 64-bit internal representation before any kntrie operation. This transformation is the foundation that makes the entire structure work uniformly.

**Signed key handling.** Signed integers have a problem: their binary representation doesn't sort in the same order as their numeric value. Negative numbers have the high bit set but should sort before positives. The fix is an XOR flip of the sign bit:

```
r ^= 1ULL << (key_bits - 1);
```

For `int32_t`, this maps `INT_MIN → 0`, `0 → 0x80000000`, `INT_MAX → 0xFFFFFFFF`, a monotonically increasing sequence that matches numeric order. The flip is its own inverse, so converting back is the same XOR.

**Left-alignment into 64 bits.** After the sign flip (if signed), the key is shifted left so its most significant bit sits at bit 63:

```
r <<= (64 - key_bits);
```

A `uint16_t` key `0xABCD` becomes `0xABCD000000000000`. A `uint32_t` key `0x12345678` becomes `0x1234567800000000`. A `uint64_t` stays as-is.

This left-alignment is what makes the 8-bit byte-wise dispatch work uniformly. The root byte is always extracted from the same position, and each subsequent level shifts to the next byte. Regardless of whether the original key was 16, 32, or 64 bits wide, the same byte extraction logic applies. Shorter keys simply have fewer meaningful bytes before they bottom out.

Without this normalization, every node operation would need key-width-specific logic. With it, the byte at any depth is always at the same bit position in the 64-bit internal key.

### 1.2 Value Storage: Implementation Details

The three value storage categories (A: trivial inline, B: packed bool, C: heap pointer) are described in [KTRIE Concepts](../ktrie_concepts.md). This section covers KNTRIE-specific implementation details.

**Normalization to fixed-width unsigned.** Category A values are normalized to a fixed-width unsigned integer type to reduce template instantiations: 1 byte → `uint8_t`, 2 → `uint16_t`, 3–4 → `uint32_t`, 5–8 → `uint64_t`. This means `kntrie<uint64_t, int>` and `kntrie<uint64_t, float>` share the same internal implementation. The `slot_type` is the normalized unsigned type.

**Bitmap256 bool specialization.** When a leaf covers exactly 256 possible key values — the case where the remaining unresolved key portion is a single byte — bool values are packed into a dedicated 256-bit bitmap with one bit per key and no per-value slot overhead.

The compile-time booleans that drive all internal dispatch:

```
IS_TRIVIAL     = trivially_copyable && sizeof <= 8            // A: true    B: true   C: false
HAS_DESTRUCTOR = !IS_TRIVIAL                                  // A: false   B: false  C: true
IS_BOOL        = std::is_same_v<VALUE, bool>                  // A: false   B: true   C: false
```

**Abstraction responsibilities:**

**kntrie** (API boundary): Normalizes both key and value before forwarding to `kntrie_impl`. Calls `store(val, alloc)` to get a `slot_type` on insert. Calls `as_ptr(slot)` to get `const VALUE*` on find/iterator deref. Never destroys directly.

**_impl** (node ownership): Owns all destruction. On erase: the ops layer handles slot destruction within the node. On clear/destructor: walks all nodes via `remove_subtree`, calls `destroy` on every occupied slot if `HAS_DESTRUCTOR`, then frees node memory.

**_ops** (node operations): Handles insert/erase logic, split/coalesce decisions, and skip prefix management. Uses `sizeof(slot_type)` for node size calculations.

**_compact / _bitmask** (node internals): No-overlap operations (realloc, new node builds) use `std::copy`. In-place shifts (insert gap, erase compaction) use `std::move` / `std::move_backward`. Insert and destroy dispatch on `if constexpr (IS_TRIVIAL)` (A/B) vs `if constexpr (!IS_TRIVIAL)` (C).

### 1.3 Memory Hysteresis

Node allocations snap to size classes. For allocations up to 128 u64s, a fixed bin table provides 12 size classes: {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128} u64s, growing by approximately 1.25–1.5× per step. Beyond 128 u64s, sizes snap to power-of-two with midpoints. The worst-case overhead is about 50%.

This padding creates room for in-place insert and erase operations. A node allocated at 48 u64s when it only needs 34 has extra slots that can absorb mutations without reallocation.

**Shrink hysteresis.** A node only shrinks when its allocated size exceeds the size class for `2× the needed size`. This prevents oscillation: if a node sits near a boundary, alternating insert/erase won't trigger repeated realloc cycles.

**Compact leaves** use power-of-two slot counts via `std::bit_ceil`, with a minimum of 2 slots. Compact leaves track the physical slot count and the entry count as separate values. The physical count may exceed the entry count due to dup padding (extra slots filled with copies of adjacent entries to enable in-place mutation).

## 2 Node Concepts

### 2.1 Bitmap Dispatch Modes

The shared bitmap representation and popcount mechanics are described in [KTRIE Concepts](../ktrie_concepts.md). The KNTRIE uses three dispatch modes, selected at compile time via `slot_mode`:

**FAST_EXIT** (insert, erase, iteration): Check whether the target bit is set. If not, return -1 immediately. If set, subtract 1 from the popcount to get the 0-based index into the dense array.

**BRANCHLESS** (find): Don't check the bit with a branch. If the target bit is set, the popcount gives a 1-based index that directly addresses the correct child — slot 0 being the reserved miss-fallback means hits always land at index ≥ 1. If the target bit is not set, the slot is masked to 0:

```cpp
slot &= -int(bool(before & (1ULL << 63)));  // 0 if miss, 1-based if hit
```

A miss produces slot index 0. Slot 0 in any child array holds a sentinel tag — a pure tag with no backing allocation. The find loop sees the leaf bit, exits the descent, then tests the not-found bit to detect the miss — no pointer dereference needed.

**UNFILTERED** (insertion position): Returns the count of set bits strictly before the target, giving the correct insertion point in the dense array.

### 2.2 Node Header

All allocated nodes in the kntrie share a common 8-byte header (`node_header_t`) at position `node[0]`:

| Field | Bits | Purpose |
|-------|------|---------|
| `depth_v` | 16 | Key position encoding: bytes consumed, skip count, suffix width |
| `entries_v` | 16 | Entry/child count |
| `alloc_u64_v` | 16 | Allocation size in u64s |
| `total_slots_v` | 16 | Physical slot count; power-of-two allocation means this may exceed entries_v. Unused by internal nodes |

`entries_v` answers "how many items are directly stored here": for leaf nodes, it is the count of stored key/value pairs; for internal nodes, it is the count of child pointers.

`alloc_u64_v` records how many u64s were allocated. This may exceed what the entry count requires due to size-class rounding.

Internal nodes use only `node_header_t` (1 u64). Leaf nodes additionally carry 5 function pointers and a u64 prefix field beyond the common header, occupying 7 u64s total.

### 2.3 Tagged Pointers

Every child pointer in the kntrie is a tagged `uint64_t` encoding both address and type:

- **Leaf tag:** The sign bit (bit 63) is set. `LEAF_BIT` = `1ULL << 63`. The node pointer is recovered by XORing with `LEAF_BIT`. Tagged leaf pointers point to `node[0]` (the header).
- **Internal tag:** No sign bit. The pointer targets `node[1]`, one u64 past the header, allowing the pointer to be used directly without adjustment, saving one addition on every descent.
- **Not-found tag:** Bit 62 is set alongside `LEAF_BIT`. `NOT_FOUND_BIT` = `1ULL << 62`. The sentinel value `LEAF_BIT | NOT_FOUND_BIT` is a pure bit pattern with no backing allocation. User-space pointers never have bit 62 set (addresses are < 2^47 on x86-64), so the tag is unambiguous.

The `while (!(ptr & LEAF_BIT))` loop tests the sign bit with a single instruction to distinguish internal nodes from leaves. After the loop exits, `ptr & NOT_FOUND_BIT` distinguishes a miss (sentinel) from a hit (real leaf) with a single bit test — no pointer dereference, no indirect call.

### 2.4 Descendants

Descendants are not stored in the node header. Internal nodes store a `uint64_t` descendant count at the end of their allocation, holding the exact total entry count for the subtree. This count drives coalesce decisions on erase: when the subtree total drops to 4096 entries or below, the entire subtree is collapsed back into a single compact leaf. Leaves don't need descendant tracking; their `entries` field is exact.

### 2.5 Leaf Contract

**Leaf header (7 u64 = 56 bytes).** All leaf node types share an extended header layout:

| Position | Content |
|----------|---------|
| `node[0]` | `node_header_t` (8 bytes) |
| `node[1]` | `find_fn` — exact match function pointer |
| `node[2]` | `find_next_fn` — iterator next function pointer |
| `node[3]` | `find_prev_fn` — iterator prev function pointer |
| `node[4]` | `find_first_fn` — descend to minimum entry |
| `node[5]` | `find_last_fn` — descend to maximum entry |
| `node[6]` | `prefix` — left-aligned key prefix (u64) |

**`depth_t`** is a 16-bit packed bitfield that encodes the leaf's position within the trie:

```
depth_t {
    is_skip  : 1    // whether this leaf has prefix bytes to check;
                    // redundant (is_skip ≡ skip ≠ 0) but deliberate —
                    // testing one bit avoids reading the skip count on the hot read path
    skip     : 3    // number of prefix bytes to compare (0-6)
    consumed : 6    // total bits resolved above this leaf (0-56, always a multiple of 8)
    shift    : 6    // = 64 - suffix_width_bits, only {0, 32, 48, 56} for suffix types u64/u32/u16/u8
}
```

The `suffix()` function extracts the leaf's suffix from a root-level internal key in two instructions: `(ik << consumed) >> shift`. This compiles to `shlx` + `shrx`, two single-cycle operations with no data-dependent branches.

For internal nodes, only `skip` from `depth_t` is meaningful: it records how many single-child dispatch levels were folded into this node's own allocation rather than given their own separate nodes.

The 5 function pointers are set at leaf construction time based on the leaf's suffix type (u8/u16/u32/u64) and whether the leaf has a skip prefix. This enables type-erased dispatch: the find loop doesn't need to know the leaf type. It loads the function pointer and calls it. The cost is 40 bytes of per-leaf overhead for the function pointers, but the benefit is a single indirect call instead of template recursion on the hot path.

**Skip prefix** is how the kntrie implements PREFIX compression. When a subtree's keys all share common bytes at a given depth, those bytes are captured in the node rather than creating separate BRANCH levels.

Leaves store skip information in two places: the `skip` count in `depth_t`, and the `prefix` u64 at `node[6]`. The prefix holds the left-aligned key prefix, the common bytes shared by all entries in this leaf. During lookup, the prefix is checked using `skip_eq(prefix, depth, ik)`, which XORs the key against the prefix within the skip mask region. If any byte differs, the key isn't in this subtree, an early exit. For iteration, `skip_cmp()` provides directional comparison (-1, 0, +1).

The skip mechanism described above applies to leaves. Internal node skip operates on the same `depth_t.skip` field but serves a different structural purpose.

## 3 Nodes

### 3.1 Sentinel

The sentinel is a tagged pointer value, not a physical node. `SENTINEL_TAGGED` is the compile-time constant `LEAF_BIT | NOT_FOUND_BIT` — two bits set, no address. Internal nodes store this value at child position 0, enabling the BRANCHLESS dispatch mode where a bitmap miss resolves to slot 0.

The find loop's descent treats the sentinel like any leaf: `LEAF_BIT` is set, so the `while (!(ptr & LEAF_BIT))` loop exits. The immediately following `NOT_FOUND_BIT` test distinguishes a miss from a real leaf:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_child(ptr, byte);
if (ptr & NOT_FOUND_BIT) [[unlikely]]
    return nullptr;
// real leaf — untag and dispatch
```

The miss path is a bit test and a conditional return. No pointer dereference, no indirect call, no function dispatch. This matters for workloads with high miss rates — BPE pair lookups, for example, where most candidate pairs are not in the vocabulary.

### 3.2 Internal Nodes

Internal nodes implement the KTRIE's BRANCH concept: 256-way fan-out dispatching on one byte of the key per node. They use the `bitmap_256_t` to compress the 256-entry child array to only the children that exist.

**Layout:**

```
[Header: 1 u64] [Bitmap: 4 u64] [Miss ptr: 1 u64] [Children: N u64] [Descendants: 1 u64]
```

The header is a single u64 `node_header_t`. The bitmap records which of the 256 possible byte values have children. The miss pointer at position 0 of the children area holds `SENTINEL_TAGGED` (`LEAF_BIT | NOT_FOUND_BIT`) for BRANCHLESS miss fallback: when a bitmap lookup misses (the target byte isn't present), the popcount returns index 0, which loads the sentinel tag value. The find loop sees `LEAF_BIT`, exits the descent, tests `NOT_FOUND_BIT`, and returns nullptr. No pointer dereference, no indirect call at the bitmap level. The N children are tagged u64 pointers, packed densely in bitmap order. The descendants counter at the end holds the exact total entry count for the subtree.

**Child lookup** uses the bitmap modes described in section 2.1. On the find path, BRANCHLESS mode returns a 1-based index (hitting the sentinel on miss). On insert/erase, FAST_EXIT mode returns -1 on miss.

The child at each position is a tagged pointer that can reference another internal node (for deeper BRANCH levels), a compact leaf (a flat sorted array of suffix/value pairs), a leaf that uses a 256-bit bitmap as key storage rather than a sorted array, or the sentinel. This is how the kntrie forms chains of variable depth. Each internal node links to the next level, and the chain terminates at a leaf.

**Chaining.** A lookup descends through a chain of internal nodes, consuming one byte of the key at each level:

```
internal[byte0] → internal[byte1] → internal[byte2] → leaf
```

The find loop is:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_child(ptr, byte_at_depth(shifted, depth++));
if (ptr & NOT_FOUND_BIT) return nullptr;
return get_find_fn<VALUE>(untag_leaf(ptr))(node, ik);
```

Each iteration tests the sign bit (leaf vs internal), extracts the byte at the current depth, does a BRANCHLESS bitmap lookup, and follows the child pointer. When the sign bit indicates a leaf, the loop exits. A `NOT_FOUND_BIT` test catches misses without a pointer dereference. Hits dispatch through the leaf's function pointer. The entire descent is a tight loop with no type switches or template recursion.

**Embed chains (skip compression).** When a subtree has a single-child internal node (meaning only one byte value exists at that level), the kntrie can inline that level into the parent's allocation rather than creating a separate node. Each inlined level is an **embed** block of 6 u64s:

```
[bitmap(4 u64)][miss_ptr(1 u64)][child_ptr(1 u64)]
```

The bitmap has exactly one bit set, encoding which byte value this level dispatches on. The miss pointer provides the BRANCHLESS miss fallback. The child pointer links to the next embed's bitmap (or to the final multi-child bitmap).

An internal node with skip=2 and N real children looks like:

```
[Header: 1 u64]
[Embed_0: bitmap(4) miss_ptr(1) child_ptr(1)]
[Embed_1: bitmap(4) miss_ptr(1) child_ptr(1)]
[Final bitmap: 4 u64] [Miss ptr: 1 u64] [Children: N u64] [Descendants: 1 u64]
```

The find loop traverses embeds transparently. `bm_child()` works on any bitmap pointer, whether it points to an embed's bitmap or the final bitmap. The difference is that embeds are inline in the same allocation, eliminating the pointer chase between the skip levels and the real dispatch level. This adds 6 u64s per skip level to the node allocation but avoids a separate heap allocation for each single-child intermediate node.

### 3.3 Compact Leaves

Compact leaf nodes implement the KTRIE's SUFFIX concept: they store key/value pairs in sorted arrays within a single allocation. They handle the case where a subtree's keys are few enough that a flat sorted structure outperforms further BRANCH subdivision.

The threshold is COMPACT_MAX = 4096. This value is intentionally large. The kntrie is designed with wide nodes. When a compact leaf overflows and splits into an internal node with up to 256 children, the average child gets 4096/256 ≈ 16 entries. This means even the smallest post-split children are reasonably sized, avoiding the pathology of many tiny nodes each carrying full header overhead. The wide-node design keeps the tree shallow and the node count low. The threshold is a tunable trade-off: a lower value creates more internal nodes and deeper bitmap traversal; a higher one enlarges the binary search within each compact leaf.

**Layout:**

```
[Header: 7 u64] [Sorted keys (aligned to 8)] [Values (aligned to 8)]
```

The 7-u64 leaf header carries the `node_header_t`, 5 function pointers, and the prefix. The keys and values arrays have `total_slots` physical entries, where `total_slots` is a power of 2 ≥ `entries`. The extra slots are filled with duplicates of adjacent entries, padding that enables in-place mutation and guarantees power-of-2 array sizes for the branchless binary search (see [KTRIE Concepts](../ktrie_concepts.md), section 1.1).

**Search** uses the branchless binary search described in [KTRIE Concepts](../ktrie_concepts.md). The power-of-2 slot count guarantees the required input constraint. Every compact leaf has `total_slots = bit_ceil(entries)` physical slots, padded with dup entries to fill the boundary. The minimum of 2 slots ensures count enters the loop as ≥ 2, so at least one comparison always executes.

**Dup-slot padding strategy.** The power-of-two allocation means a compact leaf almost always has more physical slots than entries. Rather than leaving the extra space unused, the leaf fills it with **dup slots**, copies of adjacent real entries. These dups serve two purposes:

1. **In-place insert.** When a new key arrives and dups exist, one dup is consumed: the nearest dup to the insertion point is found, the intervening entries are shifted by one position to close the gap, and the new key is written into the freed slot. No reallocation.

2. **In-place erase.** When a key is erased, its slot is overwritten with a copy of the neighboring key's value. This converts a real entry into a dup in O(1). No memmove of the remaining array.

The seeding pass distributes dups evenly among real entries. When dups are available, the nearest dup to any insertion point is at most about `entries / dups` positions away, bounding the memmove cost. When no dups remain (`entries = total_slots`), the leaf must grow before inserting.

The dup count is never stored; it's always derived: `total_slots - entries`. This keeps the header clean and avoids any possibility of the stored count drifting from reality.

**Dup scan strategy.** Finding the nearest dup depends on the total slot count. For small leaves (≤ `DUP_SCAN_MAX` = 64 total slots), a simple linear scan from the insertion point finds the nearest dup. For larger leaves, a banded search expands outward from the insertion point in alternating right-then-left bands, sized to match the expected dup spacing.

For heap-allocated values (`VALUE*`), all dup slots in a run share the same pointer as their neighbor. Destroy must be called exactly once per unique key, not once per physical slot. The `destroy_and_dealloc` operation skips adjacent duplicate keys to avoid double-free.

### 3.4 Bitmap256 Leaves

When the remaining suffix at a given depth is exactly 8 bits (the suffix type is `uint8_t`), a compact leaf would hold entries whose keys span at most 256 possible values. A bitmap256 leaf is a more compact representation for this case: since all 256 possibilities can be encoded in a 256-bit bitmap, there is no need for a sorted key array. The bitmap itself *is* the key storage.

**Layout:**

```
[Header: 7 u64] [Presence bitmap: 4 u64] [Values: N slots]
```

The header is the same 7-u64 leaf header as compact leaves. The presence bitmap records which of the 256 suffix values exist. Values are packed densely in bitmap order.

For `VALUE=bool` (category B), the values are stored as a second 256-bit bitmap rather than individual slots:

```
[Header: 7 u64] [Presence bitmap: 4 u64] [Value bitmap: 4 u64]
```

This gives the tightest possible representation: 15 u64s total for up to 256 boolean key/value pairs.

Lookup is a single `find_slot<FAST_EXIT>`: check the bit in the presence bitmap, compute the popcount for the slot index, return the value. This compiles to roughly 10 instructions with no data-dependent branches.

Bitmap256 leaves can appear at any depth where the remaining suffix is 8 bits, which depends on the original key width and how many skip prefix bytes have been captured.

### 3.5 Root

The root of the kntrie ties all node types together. It is a single tagged pointer (`root_ptr_v`) that can reference any node type: the sentinel (when empty), a compact leaf (for small collections), or an internal node (for large collections that require BRANCH dispatch).

Two additional fields manage the root's prefix:

- `root_prefix_v` (u64): The shared prefix bytes of all entries, left-aligned.
- `root_skip_bits_v` (u8): Number of leading key bits captured as prefix (0, 8, 16, ..., 48). Stored in bits to eliminate a multiply on every find and iteration call.

Every find begins with a prefix check: `(ik ^ root_prefix_v) & root_prefix_mask()`, where the mask is computed branchlessly as `~(~0ULL >> root_skip_bits_v)`. If any prefix byte differs, the key is absent. No descent needed.

When the kntrie is empty, `root_ptr_v` is `SENTINEL_TAGGED`. The first insert creates a compact leaf and sets `root_skip_bits_v` to the maximum possible prefix (KEY_BYTES - 2 for u64 = 6 bytes, for u32 = 2 bytes, for u16 = 0 bytes since both bytes are needed for dispatch). As more entries arrive with different prefixes, the root skip shrinks and new internal nodes are created to fan out on the diverging bytes.

**Upward coalescing.** The `normalize_root()` function handles structural simplification after erase. If an internal root has only one child, that child can be absorbed into the root by increasing `root_skip_bits_v` and updating the prefix. Conversely, if the total entry count drops below `COMPACT_MAX`, the entire subtree can be collapsed back into a single compact leaf via `coalesce_bm_to_leaf()`.

This design costs 25 bytes of fixed state (root_ptr 8 + prefix 8 + size 8 + skip_bits 1), padded to 32 bytes with the default stateless allocator. Stateful allocators increase the builder's size accordingly. For small collections (which may be just a single compact leaf), there is no wasted root structure. For large collections, the single root pointer leads to an internal node that provides 256-way fan-out.

## 4 Operations

### 4.1 Depth-Based Dispatch

The kntrie tracks position within the key using a **byte depth**, the number of key bytes consumed from the root to the current node. Each internal node dispatches on one byte, incrementing depth by 1. Skip levels increment depth by the skip count.

**depth_switch** converts a runtime byte depth to a compile-time `BITS` parameter via a switch statement over all valid depths. For `uint64_t` (KEY_BITS=64), the valid depths are 0 through 7, mapping to BITS values 64, 56, 48, 40, 32, 24, 16, 8. The switch generates all template instantiations upfront, and the runtime depth selects the correct one.

**Function pointer dispatch.** The find loop uses no template recursion. It descends through internal nodes in a tight loop, and when it reaches a leaf, it calls through the function pointer that the leaf stored at construction time:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_child(ptr, byte_at_depth(shifted, depth++));
if (ptr & NOT_FOUND_BIT) return nullptr;
return get_find_fn<VALUE>(untag_leaf(ptr))(node, ik);
```

Misses resolve at the `NOT_FOUND_BIT` test — a single bit check, no pointer dereference. Hits dispatch through the leaf's function pointer. Because there are only a few possible function pointer targets (one per suffix type × skip), modern CPUs predict indirect calls well. The cost is 5 u64s of storage per leaf for the function pointers, but the benefit is a single indirect call instead of template recursion on the hot path.

For write operations (insert, erase), `depth_switch` dispatches at the point where template-specialized code is needed: node splitting, coalescing, and suffix extraction. These operations are not on the hot read path, so the switch overhead is negligible compared to allocation and memmove costs.

### 4.2 find

Find is the hot path. It begins with a root prefix check: `(ik ^ root_prefix_v) & root_prefix_mask()`. If any prefix byte differs, the key is absent immediately, no descent needed.

Otherwise, `find_loop` executes a tight iteration through internal nodes. Each iteration tests the sign bit (leaf vs internal), extracts the byte at the current depth from the shifted key, performs a BRANCHLESS bitmap lookup, and follows the child pointer. No type switches, no template recursion, no conditional branches at the bitmap level.

When the loop exits (sign bit set), a `NOT_FOUND_BIT` test resolves misses immediately — a single bit check returning nullptr with no pointer dereference and no indirect call. For hits, the pointer is untagged, the leaf's `find_fn` is loaded, and called. The function pointer dispatches to the correct suffix type and skip variant. For compact leaves, this performs a branchless binary search over the sorted suffix array. For bitmap256 leaves, it checks a single bit in the presence bitmap and computes a popcount for the slot index.

The entire find path from root to result is typically 2-4 pointer dereferences plus one branchless search for collections below ~1M entries, with no heap allocation and no locking.

### 4.3 find first / find last

`descend_first_loop` and `descend_last_loop` find the minimum and maximum entries in the trie. They follow the same internal node loop as find, but instead of extracting a key byte, they follow the first or last child at each level:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_first_child(ptr);   // or bm_last_child
return get_find_first<VALUE>(untag_leaf(ptr))(node);
```

`bm_first_child` returns the child at the lowest set bit in the bitmap; `bm_last_child` returns the highest. At the leaf, the `find_first_fn` / `find_last_fn` function pointer returns the minimum or maximum entry within that leaf. These are used by `begin()`, `rbegin()`, and the walk-back phase of iteration.

### 4.4 find next / find prev

Iterator advancement is the second-hottest path after find. `iter_next_loop` finds the smallest key strictly greater than the current key. It operates in three phases:

**Hot path: descent with path recording.** The loop descends through internal nodes, saving each node pointer in a fixed-size stack (`path[8]`, matching the maximum K=8 byte depth for u64 keys). At each level it uses `bm_child_exact` (FAST_EXIT mode) to find the exact child for the current key byte. If the child doesn't exist, it jumps directly to walk-back.

**Leaf probe.** At the leaf, it calls `find_next_fn`. For compact leaves, this performs a branchless binary search for the next key after the current one. For bitmap256 leaves, it scans the presence bitmap for the next set bit. The function pointer handles both cases transparently. In the common case (many entries in the same leaf), iteration resolves here without any walk-back. This is where the wide-node design pays off: most iterator increments never leave the leaf.

**Cold path: walk-back.** If the leaf is exhausted (the current key is the leaf's maximum), the loop walks backward through the saved path. At each level it calls `bm_next_sibling` to find the next child after the current byte in the bitmap. When a sibling is found, `descend_first_loop` walks to its minimum entry.

`iter_prev_loop` is the mirror image: it calls `find_prev_fn` at the leaf, and walks back using `bm_prev_sibling` followed by `descend_last_loop`.

The root prefix is checked before entering the loop. If the iterator's key falls outside the root prefix, the prefix comparison determines whether to descend to the first entry (key is below the prefix) or return end (key is above).

### 4.5 insert

Insert begins with a root prefix check. If the new key diverges from the current root prefix, `reduce_root_skip` shortens the prefix to the point of divergence, creating internal nodes to fan out on the differing byte.

The core insertion is `insert_node`, a runtime-recursive function that descends through three node types. Unlike find, which uses a tight iterative loop, insert accepts recursion because its cost is dominated by allocation and memmove; the function-call overhead per level is negligible by comparison.

**Sentinel.** An empty child slot. Creates a new single-entry compact leaf via `make_single_leaf` and returns it as the new child pointer.

**Leaf.** Walks any skip prefix bytes, comparing against the key. If a prefix byte diverges, `split_on_prefix` creates a new internal node at the divergence point with two children: the existing leaf and a new leaf for the inserted key. If the prefix matches, `leaf_insert` handles the body: for compact leaves, `compact_ops::insert` inserts into the sorted array (consuming a dup slot if available, or reallocating). For bitmap256 leaves, `bitmap_insert` sets the presence bit. If the compact leaf exceeds COMPACT_MAX, `convert_to_bitmask_tagged` splits it into an internal node with up to 256 children.

**Internal node.** Walks any embed chain skip bytes. If a skip byte diverges, `split_skip_at` creates a new internal node at the divergence point. Otherwise, performs a bitmap lookup for the key byte. If the child doesn't exist, creates a new leaf and adds it to the bitmap via `add_child`. If it exists, recurses into the child. On unwind, updates the child pointer if it changed (due to reallocation or splitting) and increments the descendant count.

After insert returns to the root level, `normalize_root` checks whether the root can absorb additional prefix bytes (if the root is an internal node with uniform first-byte children).

### 4.6 erase

Erase follows the same descent structure as insert: root prefix check, then `erase_node` recurses through sentinel (return immediately), leaf (erase from sorted array or bitmap), and internal node (recurse into child).

The interesting logic is on the unwind path:

**Descendant check.** After a successful erase, the parent's descendant count is decremented. If it drops to COMPACT_MAX or below, `do_coalesce` collects all entries from the subtree into a flat array and rebuilds a single compact leaf, replacing the entire internal node subtree.

**Single-child collapse.** If removing a child leaves an internal node with exactly one remaining child, the node is collapsed: its skip bytes are prepended to the child (either extending a leaf's skip prefix or wrapping a bitmask in a longer embed chain), and the single-child node is deallocated.

**Root normalization.** After erase returns to the root level, `normalize_root` absorbs single-child internal roots and `coalesce_bm_to_leaf` handles the case where the total size has dropped below COMPACT_MAX.

For compact leaves, erase converts the removed entry into a dup by overwriting it with a copy of its neighbor, an O(1) operation with no memmove. For bitmap256 leaves, erase clears the presence bit and shifts subsequent values down by one slot, an O(N) operation bounded by 256. If the leaf becomes empty, it is deallocated and the parent removes the child from its bitmap.

### 4.7 modify

`modify(key, fn)` applies a user function to the value at `key` in place, without find+erase+insert overhead. The function signature is `void fn(VALUE&)` — it receives a mutable reference to the stored value.

The implementation walks the trie once to locate the key. At the compact leaf, it calls `fn` directly on the slot. For inline types (Category A), this modifies the u64 slot in place. For heap types (Category C), all dup slots share the same pointer, so the modification is visible through all of them. No reallocation, no value copy, no destroy/reconstruct cycle.

A two-argument overload `modify(key, fn, default_val)` handles the miss case: if the key exists, apply `fn`; if not, insert `default_val` as-is (fn is not called on the default). This enables atomic accumulator patterns:

```cpp
trie.modify(key, [](int64_t& v) { v++; }, 1);  // increment or insert 1
```

For inline types, the public API handles the VALUE/slot_type mismatch (e.g. `int64_t` stored as `uint64_t`) via `reinterpret_cast<VALUE&>(slot)` in a wrapper lambda. This is transparent to the caller.

### 4.8 erase_when

`erase_when(key, fn)` is a single-walk conditional erase. The function signature is `bool fn(const VALUE&)`. The trie descends to the key, tests the predicate on the value, and erases only if the predicate returns true. Returns `true` if the entry was erased, `false` if not found or predicate failed.

The implementation reuses the full erase unwind path (descendant check, single-child collapse, root normalization) but gates entry into that path on the predicate result at the leaf level. If the predicate fails, the trie is untouched — no wasted unwind work.

This avoids the find-then-erase two-walk pattern and the risk of the key being modified or erased between the two walks.

### 4.9 Iterators are Snapshots

The kntrie iterator is a snapshot: it stores a copy of the current key and value, not a pointer into the trie. Each `operator++` and `operator--` re-descends the trie from the root via `iter_next` or `iter_prev` to find the next or previous entry.

This design has two consequences:

**Stability.** Iterators are never invalidated by mutations to other keys. Inserting or erasing a different key does not affect an existing iterator's stored key/value pair. The iterator will find the correct next/previous entry on its next advance, even if the trie's internal structure has been reorganized by splits, coalesces, or reallocation.

**No auto-update.** If the value at the iterator's current key is modified (via `insert_or_assign`, `assign`, or `modify`), the iterator still holds the old value. Dereferencing returns the snapshot, not the live data. To see the updated value, the caller must re-find the key or advance and return. This is a deliberate trade-off: `modify` changes the live data in place, but existing iterators see the value as it was when they were created or last advanced. The snapshot design means `modify` never invalidates iterators — it just doesn't update them.

The cost of this approach is that each iterator increment performs a full root-to-leaf descent rather than following a stored pointer. In practice this cost is low: most increments resolve within a single compact leaf (the hot path in `find_next_fn`), and the descent through internal nodes is the same tight loop as find. The benefit is simplicity: no iterator bookkeeping, no invalidation tracking, and no dangling pointer risk.

## 5 Performance

Performance graphs and detailed benchmark results are maintained separately and updated with each optimization pass. This section describes the conceptual performance characteristics and why they arise.

### 5.1 std::map

`std::map` is a red-black tree, a self-balancing binary search tree where each node contains one key-value pair, two child pointers, a parent pointer, and a color bit. On most implementations, each node is a separate heap allocation of about 72 bytes (key + value + 3 pointers + color + alignment padding).

Red-black trees guarantee O(log N) worst-case lookup, insert, and erase through rotation and recoloring rules that keep the tree approximately balanced. The maximum height is 2 × log₂(N), so a million-entry map requires at most ~40 comparisons per lookup.

The fundamental costs are:

**Memory.** Every entry costs ~72 bytes regardless of key or value size. A million uint64_t→uint64_t entries consume ~72 MB in `std::map` versus ~16 MB in kntrie, a ~4.5× difference.

**Cache behavior.** Each comparison in a tree traversal follows a pointer to a separately-allocated node. These nodes are scattered across the heap in allocation order, not key order. At scale, nearly every level of the tree is a cache miss. A 20-level traversal in a million-entry map touches 20 random cache lines.

**Sorted iteration.** The red-black tree provides naturally sorted in-order traversal, with O(1) amortized iterator increment and decrement. kntrie provides the same sorted iteration, but resolves most increments within a single compact leaf via branchless binary search rather than following parent/child pointers.

### 5.2 std::unordered_map

`std::unordered_map` is a hash table, typically implemented as an array of buckets with separate chaining. Each entry is a heap-allocated node containing the key, value, hash, and a next pointer. On most implementations, per-entry overhead is roughly 40-64 bytes depending on key/value size and alignment.

Hash tables offer O(1) expected-case lookup: compute the hash, index into the bucket array, then walk a short chain. The constant factor is low when the load factor is moderate and the hash function distributes well.

The fundamental costs are:

**Memory.** Each entry carries per-node overhead (next pointer, hash, alignment padding) plus the bucket array itself. At low load factors the bucket array wastes space; at high load factors chains lengthen. Rehashing requires allocating a new bucket array and re-inserting every entry, causing periodic latency spikes.

**Cache behavior.** The hash function deliberately scatters keys across the bucket array, destroying any locality the input data might have had. Walking a chain follows heap-scattered pointers, similar to `std::map`. For large tables, nearly every lookup is a cache miss on the bucket access and another on the node access.

**No ordered iteration.** Hash tables provide no key ordering. Iterating in sorted order requires copying keys to a separate container and sorting, which is O(N log N) and destroys the O(1) lookup advantage.

**No compression.** Every entry stores the complete key, the full hash value, and structural pointers. Adjacent entries with similar keys get no benefit from their similarity.

### 5.3 KNTRIE

The kntrie's lookup complexity is O(K) where K is the key width in bytes, a constant determined at compile time. For `uint64_t`, K=8 means at most 8 levels of internal nodes (one per byte). For `int32_t`, K=4 means at most 4 levels. For `uint16_t`, K=2 means at most 2 levels. This is independent of N, the number of entries.

In practice, the effective depth is often less than the maximum because of two mechanisms:

**Skip prefix capture** collapses levels where all keys in a subtree share a common byte. Root-level skip is particularly effective: for `uint64_t` keys, up to 6 prefix bytes can be captured at the root, leaving only 2 bytes of real dispatch.

**Compact leaf absorption** catches entire subtrees in a flat sorted array. When a subtree has ≤ 4096 entries, it remains a compact leaf rather than being split into internal nodes. The search within that compact leaf is a branchless binary search: fast, cache-friendly, and often faster than descending further into the trie.

These two mechanisms create a dependency on N for the effective depth:

**N ≤ 4096:** The entire dataset fits in a single compact leaf at the root. Zero internal node descents, just a branchless binary search.

**N ≤ ~1M:** One level of internal dispatch fans out to compact leaves, each holding up to 4096 entries.

**N ≤ ~268M:** Two levels of internal dispatch (256 × 256 × 4096 = 268M). This covers datasets well beyond what most in-memory use cases require.

**Full depth:** Up to K levels of internal dispatch, reached only when subtrees overflow their 4096-entry compact leaves at every level. In practice, even a billion-entry dataset with random uint64_t keys rarely reaches full depth because the keys distribute across the 256 possible byte values at each level.

The key insight: O(K) is the theoretical bound, but the compact leaf mechanism makes the practical behavior closer to O(1) with a binary search whose size grows slowly with N. The kntrie's depth only increases when a key range is dense enough to overflow compact leaves.

### 5.4 The Real Complexity: Memory Hierarchy

Textbook complexity treats memory access as uniform cost. In practice, every pointer chase or array lookup pays a cost determined by where the data lives in the memory hierarchy: L1, L2, L3, or DRAM. As N grows and the working set exceeds each cache level, per-operation cost increases for all three structures.

`std::map`'s O(log N) is well known, but after factoring out the log N tree depth, each individual hop still gets 2-3x more expensive per 10x growth in N, a pure memory hierarchy effect. `std::unordered_map`, supposedly O(1), degrades just as badly: its hash-then-chase-pointer pattern scatters across the same oversized heap.

The kntrie's advantage is twofold. First, much smaller memory footprint means the working set stays in faster cache levels longer. Second, the trie's dense node layout gives spatial locality. Adjacent keys share cache lines, so one fetch services multiple lookups.

In short: as N grows, all three structures pay an increasing cost per operation driven by cache pressure. The kntrie's smaller memory footprint keeps the working set in faster cache levels for larger N.

## 6 Summary

The kntrie combines three inheritances: TRIE-style prefix routing for O(K) lookup independent of collection size, B-tree-style wide sorted leaves for cache-friendly storage and iteration, and bitmap compression for memory-efficient internal dispatch. The result is an ordered associative container that approaches hash-table lookup speed while maintaining full key ordering, with a memory footprint that can fall below the raw key-value data size through prefix and suffix compression. Benchmark data is maintained separately from this document.
