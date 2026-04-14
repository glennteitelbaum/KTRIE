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
  - [4.1 Shift-Based Dispatch](#41-shift-based-dispatch)
  - [4.2 find](#42-find)
  - [4.3 find first / find last](#43-find-first--find-last)
  - [4.4 find next / find prev](#44-find-next--find-prev)
  - [4.5 insert](#45-insert)
  - [4.6 erase](#46-erase)
  - [4.7 Iterators are Live](#47-iterators-are-live)
- [5 Performance](#5-performance)
  - [5.1 std::map](#51-stdmap)
  - [5.2 std::unordered_map](#52-stdunordered_map)
  - [5.3 KNTRIE](#53-kntrie)
  - [5.4 The Real Complexity: Memory Hierarchy](#54-the-real-complexity-memory-hierarchy)
- [6 Summary](#6-summary)

## 1 Optimizations

### 1.1 Key Representation

All key types (`uint16_t`, `int32_t`, `uint64_t`, signed or unsigned) are transformed into a stored representation of the same width before any kntrie operation. The stored type is `std::make_unsigned_t<KEY>`: a `uint64_t` stores as `uint64_t`, an `int32_t` stores as `uint32_t`, a `uint16_t` stores as `uint16_t`.

**Signed key handling.** Signed integers have a problem: their binary representation doesn't sort in the same order as their numeric value. Negative numbers have the high bit set but should sort before positives. The fix is an XOR flip of the sign bit:

```
stored ^= UK(1) << (key_bits - 1);
```

For `int32_t`, this maps `INT_MIN → 0`, `0 → 0x80000000`, `INT_MAX → 0xFFFFFFFF`, a monotonically increasing sequence that matches numeric order. The flip is its own inverse, so converting back is the same XOR. Unsigned types store as-is (the flip is a no-op that folds away at compile time).

**Byte extraction via shift counter.** During descent, byte extraction uses a running shift counter: at each branch level, the dispatch byte is `(stored >> shift) & 0xFF`, where shift starts at the most significant byte and decrements by 8. For `uint64_t`, shift starts at 56 (byte 0 at bits 63–56). For `uint32_t`, shift starts at 24. For `uint16_t`, shift starts at 8. The maximum depth is bounded by key width: 2 levels for `uint16_t`, 4 for `uint32_t`, 8 for `uint64_t`.

**Full key storage.** Compact nodes store the complete stored key at every depth. A `uint64_t` entry always stores 8 bytes, regardless of how many prefix bytes the branch structure has consumed. All operations use a single key type (`std::make_unsigned_t<KEY>`) at all depths, and iteration reads the next key in a single array load.

### 1.2 Value Storage: Implementation Details

The three value storage categories (A: trivial inline, B: packed bool, C: heap pointer) are described in [KTRIE Concepts](../ktrie_concepts.md). This section covers KNTRIE-specific implementation details.

**Normalization to fixed-width unsigned.** Category A values are normalized to a fixed-width unsigned integer type to reduce template instantiations: 1 byte → `uint8_t`, 2 → `uint16_t`, 3–4 → `uint32_t`, 5–8 → `uint64_t`. This means `kntrie<uint64_t, int>` and `kntrie<uint64_t, float>` share the same internal implementation. The `slot_type` is the normalized unsigned type.

**Bitmap256 bool specialization.** When a leaf covers exactly 256 possible key values — the case where the remaining unresolved key portion is a single byte — bool values are packed into a dedicated 256-bit bitmap with one bit per key and no per-value slot overhead.

The compile-time booleans that drive all internal dispatch:

```
IS_TRIVIAL     = trivially_copyable && sizeof <= 8            // A: true    B: true   C: false
IS_INLINE      = IS_TRIVIAL                                   // A: true    B: true   C: false
HAS_DESTRUCTOR = !IS_TRIVIAL                                  // A: false   B: false  C: true
IS_BOOL        = std::is_same_v<VALUE, bool>                  // A: false   B: true   C: false
slot_type      = IS_INLINE ? VALUE : VALUE*                   // A: VALUE   B: bool   C: VALUE*
```

`IS_INLINE` determines whether the slot stores the value directly or a heap pointer. All value-access code dispatches on `if constexpr (VT::IS_INLINE)` to handle the indirection correctly — including iterators, `operator[]`, `at()`, and `value_ref_at`.

**Abstraction responsibilities:**

**kntrie** (API boundary): Normalizes both key and value before forwarding to `kntrie_impl`. For iterator dereference, `deref_val(val_v)` handles the inline/pointer indirection: for trivial types, `val_v` points directly at the value; for non-trivial types (Category C), `val_v` points at a `VALUE*` slot, and `deref_val` dereferences through it. Never destroys directly.

**_impl** (node ownership): Owns all destruction. On erase: the ops layer handles slot destruction within the node. On clear/destructor: walks all nodes via `remove_subtree`, calls `destroy` on every occupied slot if `HAS_DESTRUCTOR`, then frees node memory. Maintains parent pointer linkage on all structural changes.

**_ops** (node operations): Handles insert/erase logic, split/coalesce decisions, skip prefix management, and parent pointer relinking. Uses `sizeof(slot_type)` for node size calculations. `convert_to_bitmask_tagged` partitions entries by dispatch byte when creating children — children at the final byte depth become bitmap leaves, others become compact leaves storing the full key.

**_compact / _bitmask** (node internals): Use `std::memcpy` / `std::memmove` for slot movement (valid for both trivial types and pointer slots). Insert and destroy dispatch on `if constexpr (VT::IS_INLINE)` vs `if constexpr (VT::HAS_DESTRUCTOR)` for value indirection.

### 1.3 Memory Hysteresis

Node allocations snap to size classes. A fixed table provides 26 size classes from 4 to 16,384 u64s, growing by approximately 1.5× per step: {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128, 194, 256, 386, 512, ...16384}. Beyond the table, exact sizes are used. The worst-case overhead is about 50%.

This padding creates room for in-place insert and erase operations. A node allocated at 48 u64s when it only needs 34 has extra slots that can absorb mutations without reallocation.

**Shrink hysteresis.** A node only shrinks when its allocated size exceeds the size class for `2× the needed size`. This prevents oscillation: if a node sits near a boundary, alternating insert/erase won't trigger repeated realloc cycles.

**Compact leaves** use size-class allocation. The entry count is exact — no padding slots. Size-class rounding provides headroom for in-place inserts when the allocation has spare capacity.

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
| `flags_v` | 8 | Bit 0: `is_bitmap` (leaf type); bits 1–3: `skip` (branch chain only) |
| `reserved_v` | 8 | Reserved |
| `entries_v` | 16 | Entry/child count |
| `alloc_u64_v` | 16 | Allocation size in u64s (capacity for compact leaves) |
| `parent_byte_v` | 16 | Dispatch byte in parent bitmask node (ROOT_BYTE = 256 for root) |

`entries_v` answers "how many items are directly stored here": for leaf nodes, it is the count of stored key/value pairs; for internal nodes, it is the count of child pointers.

`alloc_u64_v` records how many u64s were allocated. This may exceed what the entry count requires due to size-class rounding.

`parent_byte_v` records which byte value this node was dispatched on in its parent bitmask. This enables upward traversal for live iterators: given any node, the parent byte tells the parent which child slot this node occupies, allowing sibling search without re-descending from the root. The root node uses the sentinel value `ROOT_BYTE = 256` (out of the 0–255 byte range) to terminate the upward walk.

Internal nodes use a 2-u64 header: `node[0]` is `node_header_t`, `node[1]` is the parent bitmask node pointer. Compact leaf nodes also use a 2-u64 header. Bitmap leaf nodes use a 3-u64 header: the node header, the parent pointer, and a base key for iteration.

### 2.3 Tagged Pointers

Every child pointer in the kntrie is a tagged `uint64_t` encoding both address and type:

- **Leaf tag:** The sign bit (bit 63) is set. `LEAF_BIT` = `1ULL << 63`. The node pointer is recovered by XORing with `LEAF_BIT`. Tagged leaf pointers point to `node[0]` (the header).
- **Internal tag:** No sign bit. The pointer targets `node[HEADER_U64]` (= `node[2]`, two u64s past the start — past the header and parent pointer), pointing directly at the bitmap. This allows the bitmap to be used immediately on every descent without adjustment, saving one addition per level.
- **Not-found tag:** Bit 62 is set alongside `LEAF_BIT`. `NOT_FOUND_BIT` = `1ULL << 62`. The sentinel value `LEAF_BIT | NOT_FOUND_BIT` is a pure bit pattern with no backing allocation. User-space pointers never have bits 62–63 set: on 4-level paging (48-bit VA), canonical addresses are < 2^47; on 5-level paging (57-bit VA, LA57), canonical addresses are < 2^56. In both cases bits 62–63 are zero for user-space, so the tags are unambiguous. The tag is cast through `std::uintptr_t` (not `reinterpret_cast`) to avoid pointer provenance UB, with a `static_assert` that the low tag bits don't collide with alignment.

The `while (!(ptr & LEAF_BIT))` loop tests the sign bit with a single instruction to distinguish internal nodes from leaves. After the loop exits, `ptr & NOT_FOUND_BIT` distinguishes a miss (sentinel) from a hit (real leaf) with a single bit test — no pointer dereference, no indirect call.

### 2.4 Descendants

Descendants are not stored in the node header. Internal nodes store a `uint64_t` descendant count at the end of their allocation, holding the exact total entry count for the subtree. This count drives coalesce decisions on erase: when the subtree total drops to COMPACT_MAX (1024) entries or below, the entire subtree is collapsed back into a single compact leaf. Leaves don't need descendant tracking; their `entries` field is exact.

### 2.5 Leaf Contract

**Compact leaf header (2 u64 = 16 bytes).**

| Position | Content |
|----------|---------|
| `node[0]` | `node_header_t` (8 bytes, `is_bitmap`=false) |
| `node[1]` | `parent_ptr` — pointer to parent bitmask node (for upward iteration) |

Compact leaves store the full stored key for every entry; binary search on `operator<` handles lookup directly. The `alloc_u64_v` field stores the capacity (maximum entry count that fits in the allocation).

**Bitmap leaf header (3 u64 = 24 bytes).**

| Position | Content |
|----------|---------|
| `node[0]` | `node_header_t` (8 bytes, `is_bitmap`=true) |
| `node[1]` | `parent_ptr` — pointer to parent bitmask node |
| `node[2]` | `base_key` — stored key with the bitmap byte zeroed (for key reconstruction) |

Bitmap leaves exist only at the final byte depth, where all prefix bytes have been consumed by the branch structure above. The `base_key` field is used by iteration to reconstruct the full stored key from a bit position: `base_key | UK(bit_index)`.

**Leaf type dispatch** uses the `is_bitmap` flag in the header. The header is loaded once after the find loop exits; this load also prefetches the key array (which begins immediately after the header). A single predictable branch dispatches to compact or bitmap search.

**Parent pointer** at `node[1]` (both compact and bitmap) stores a raw pointer to the parent bitmask node. Combined with `parent_byte_v` in the header, this enables upward traversal: the iterator walks from leaf to parent, finds the next sibling in the parent's bitmap, and descends to the sibling's edge entry.

**Skip scope.** Skip compression in KNTRIE operates at two levels: the root prefix (capturing shared leading bytes of all entries) and branch node embed chains (inlining single-child dispatch levels). Leaves do not participate in skip — compact leaves store full keys, and bitmap leaves are always at the final byte depth where all prefix bytes have already been consumed.

## 3 Nodes

### 3.1 Sentinel

The sentinel is a tagged pointer value, not a physical node. `SENTINEL_TAGGED` is the compile-time constant `LEAF_BIT | NOT_FOUND_BIT` — two bits set, no address. Internal nodes store this value at child position 0, enabling the BRANCHLESS dispatch mode where a bitmap miss resolves to slot 0.

The find loop's descent treats the sentinel like any leaf: `LEAF_BIT` is set, so the `while (!(ptr & LEAF_BIT))` loop exits. The immediately following `NOT_FOUND_BIT` test distinguishes a miss from a real leaf:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_child(ptr, byte);
if (ptr & NOT_FOUND_BIT) [[unlikely]]
    return {};  // not-found entry
// real leaf — untag and dispatch
```

The miss path is a bit test and a conditional return. No pointer dereference, no indirect call, no function dispatch. This matters for workloads with high miss rates — BPE pair lookups, for example, where most candidate pairs are not in the vocabulary.

### 3.2 Internal Nodes

Internal nodes implement the KTRIE's BRANCH concept: 256-way fan-out dispatching on one byte of the key per node. They use the `bitmap_256_t` to compress the 256-entry child array to only the children that exist.

**Layout:**

```
[Header: 1 u64] [Parent ptr: 1 u64] [Bitmap: 4 u64] [Miss ptr: 1 u64] [Children: N u64] [Descendants: 1 u64]
```

The header is a single u64 `node_header_t`. The parent pointer at `node[1]` stores a raw pointer to the parent bitmask node (or null for the root), enabling upward iteration. The bitmap records which of the 256 possible byte values have children. The miss pointer at position 0 of the children area holds `SENTINEL_TAGGED` (`LEAF_BIT | NOT_FOUND_BIT`) for BRANCHLESS miss fallback: when a bitmap lookup misses (the target byte isn't present), the popcount returns index 0, which loads the sentinel tag value. The find loop sees `LEAF_BIT`, exits the descent, tests `NOT_FOUND_BIT`, and returns a not-found entry. No pointer dereference, no indirect call at the bitmap level. The N children are tagged u64 pointers, packed densely in bitmap order. The descendants counter at the end holds the exact total entry count for the subtree.

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
if (ptr & NOT_FOUND_BIT) return {};  // not-found entry
auto* leaf = untag_leaf(ptr);
auto* hdr = get_header(leaf);
if (hdr->is_bitmap())
    return bitmap_find_byte(leaf, stored, (stored >> shift) & 0xFF);
return compact_find(leaf, hdr, stored);
```

Each iteration tests the sign bit (leaf vs internal), extracts the byte at the current shift position, does a BRANCHLESS bitmap lookup, and follows the child pointer. When the sign bit indicates a leaf, the loop exits. A `NOT_FOUND_BIT` test catches misses without a pointer dereference. Hits load the header and dispatch via `is_bitmap` to the appropriate leaf search. The entire descent is a tight loop with one bitmap operation and one pointer follow per level.

**Embed chains (skip compression).** When a subtree has a single-child internal node (meaning only one byte value exists at that level), the kntrie can inline that level into the parent's allocation rather than creating a separate node. Each inlined level is an **embed** block of 6 u64s:

```
[bitmap(4 u64)][miss_ptr(1 u64)][child_ptr(1 u64)]
```

The bitmap has exactly one bit set, encoding which byte value this level dispatches on. The miss pointer provides the BRANCHLESS miss fallback. The child pointer links to the next embed's bitmap (or to the final multi-child bitmap).

An internal node with skip=2 and N real children looks like:

```
[Header: 1 u64] [Parent ptr: 1 u64]
[Embed_0: bitmap(4) miss_ptr(1) child_ptr(1)]
[Embed_1: bitmap(4) miss_ptr(1) child_ptr(1)]
[Final bitmap: 4 u64] [Miss ptr: 1 u64] [Children: N u64] [Descendants: 1 u64]
```

The find loop traverses embeds transparently. `bm_child()` works on any bitmap pointer, whether it points to an embed's bitmap or the final bitmap. The difference is that embeds are inline in the same allocation, eliminating the pointer chase between the skip levels and the real dispatch level. This adds 6 u64s per skip level to the node allocation but avoids a separate heap allocation for each single-child intermediate node.

### 3.3 Compact Leaves

Compact leaf nodes implement the KTRIE's SUFFIX concept: they store key/value pairs in sorted arrays within a single allocation. They handle the case where a subtree's keys are few enough that a flat sorted structure outperforms further BRANCH subdivision.

The threshold is COMPACT_MAX = 1024. This balances write performance (memmove cost is O(C) per insert/erase), header overhead at split (16 bytes per child ÷ ~4 entries per child = 4 bytes/entry), and binary search depth (log₂(1024) ≈ 10 comparisons). On erase, when a branch subtree's descendant count drops to COMPACT_MAX or below, the entire subtree is collapsed back into a single compact leaf.

**Layout:**

```
[Header: 2 u64] [Sorted keys (aligned to 8)] [Values (aligned to 8)]
```

The 2-u64 header carries the `node_header_t` and the parent pointer. The keys are the full stored representation (`std::make_unsigned_t<KEY>`) regardless of depth. The keys and values arrays have exactly `entries` elements. Allocation uses size-class rounding, so the physical allocation may exceed the logical size, providing room for in-place inserts when the next size class has spare capacity.

**Search** uses branchless binary search via `adaptive_search`. Non-power-of-two entry counts are handled by an initial adjustment step that conditionally advances the base pointer, reducing to a power-of-two count for the main loop. The main loop compiles to a tight sequence of conditional moves with no data-dependent branches.

For heap-allocated values (Category C), each slot stores a `VALUE*` pointer. The `value_traits` abstraction handles the indirection: `store()` allocates and constructs the value, `destroy()` destructs and deallocates, and all value-access code dereferences through the pointer via `if constexpr (VT::IS_INLINE)` dispatch.

### 3.4 Bitmap256 Leaves

When the branch structure has consumed all bytes of the key except the last, a bitmap256 leaf is used instead of a compact sorted array. Since all 256 possible byte values can be encoded in a 256-bit bitmap, there is no need for a sorted key array. The bitmap itself *is* the key storage.

Bitmap leaves are created during split when the child shift reaches zero — the final byte depth, where all prefix bytes have been consumed by the branch path above.

**Layout:**

```
[Header: 3 u64] [Presence bitmap: 4 u64] [Values: N slots]
```

The 3-u64 header carries the `node_header_t` (with `is_bitmap`=true), the parent pointer, and a base key (`stored_key & ~0xFF`) used for key reconstruction during iteration. The presence bitmap records which of the 256 byte values exist. Values are packed densely in bitmap order.

For `VALUE=bool` (category B), the values are stored as a second 256-bit bitmap rather than individual slots:

```
[Header: 3 u64] [Presence bitmap: 4 u64] [Value bitmap: 4 u64]
```

This gives a compact representation: 11 u64s total for up to 256 boolean key/value pairs.

Lookup is a single bit test in the presence bitmap plus a popcount for the slot index. On the find hot path, the byte is already extracted by the caller, so no additional shift is needed.

### 3.5 Root

The root of the kntrie ties all node types together. It is a single tagged pointer (`root_ptr_v`) that can reference any node type: the sentinel (when empty), a compact leaf (for small collections), or an internal node (for large collections that require BRANCH dispatch).

Two additional fields manage the root's prefix:

- `root_prefix_v` (UK): The shared prefix bytes of all entries, stored as the unsigned key type.
- `root_skip_bytes_v` (unsigned): Number of leading key bytes captured as prefix (0 to KEY_BYTES − 2).

Every find begins with a prefix check: `(stored ^ root_prefix_v) & root_prefix_mask()`. If any prefix byte differs, the key is absent. No descent needed.

When the kntrie is empty, `root_ptr_v` is `SENTINEL_TAGGED`. The first insert creates a compact leaf and sets `root_skip_bytes_v` to the maximum possible prefix (6 for u64, 2 for u32, 0 for u16). As more entries arrive with different prefixes, the root skip shrinks and new internal nodes are created to fan out on the diverging bytes.

**Upward coalescing.** The `normalize_root()` function handles structural simplification after erase. If an internal root has a skip chain, the chain is absorbed into `root_skip_bytes_v` and flattened. If the total entry count drops below `COMPACT_MAX`, `coalesce_bm_to_leaf()` collapses the entire subtree into a single compact leaf. When the root points to a compact leaf, root skip is retained as a fast-reject filter — the leaf stores full keys and the binary search handles correctness, but the prefix check avoids the descent entirely for keys outside the common prefix range.

## 4 Operations

### 4.1 Shift-Based Dispatch

The kntrie tracks position within the key using a **shift counter** that starts at the most significant byte and decrements by 8 at each level. For `uint64_t`, the initial shift is 56; after consuming one byte, shift becomes 48, then 40, and so on. When shift reaches 0, the final byte is being examined.

All operations use this single shift counter. Compact nodes store the full stored key at every depth, so the shift counter is used only during branch descent, not at the leaf level.

**Leaf type dispatch** uses the `is_bitmap` flag in the header:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_child(ptr, (stored >> shift) & 0xFF);
    shift -= 8;
if (ptr & NOT_FOUND_BIT) return {};
auto* leaf = untag_leaf(ptr);
auto* hdr = get_header(leaf);  // prefetches key array cache line
if (hdr->is_bitmap())
    return bitmap_find_byte(leaf, stored, (stored >> shift) & 0xFF);
return compact_find(leaf, hdr, stored);
```

The header load serves double duty: it provides the `is_bitmap` flag and prefetches the compact node's key array. The `is_bitmap` branch is highly predictable — for random keys it's almost always compact.

### 4.2 find

Find is the hot path. It begins with a root prefix check: `(stored ^ root_prefix_v) & root_prefix_mask()`. If any prefix byte differs, the key is absent immediately, no descent needed.

Otherwise, `find_loop` executes a tight iteration through internal nodes. Each iteration tests the sign bit (leaf vs internal), extracts the byte at the current shift position via `(stored >> shift) & 0xFF`, performs a BRANCHLESS bitmap lookup, and follows the child pointer.

When the loop exits (sign bit set), a `NOT_FOUND_BIT` test resolves misses immediately — a single bit check returning a not-found entry. For hits, the header is loaded (prefetching the key array), and `is_bitmap` dispatches to the correct search. For compact leaves, a branchless binary search runs over the full stored key array. For bitmap256 leaves, a single bit test in the presence bitmap resolves the lookup.

The entire find path from root to result is typically 2-4 pointer dereferences plus one branchless search for collections below ~1M entries, with no heap allocation and no locking.

### 4.3 find first / find last

`descend_edge_loop` finds the minimum or maximum entry in the trie. It follows the same internal node loop as find, but instead of extracting a key byte, it follows the first or last child at each level:

```cpp
while (!(ptr & LEAF_BIT))
    ptr = bm_first_child(ptr);   // or bm_last_child
auto* node = untag_leaf(ptr);
auto* hdr = get_header(node);
if (hdr->is_bitmap())
    return bitmap_edge(node, dir);
return compact_edge(node, dir);
```

`bm_first_child` returns the child at the lowest set bit in the bitmap; `bm_last_child` returns the highest. At the leaf, the `is_bitmap` flag dispatches to the correct edge function, returning the minimum or maximum entry. These are used by `begin()`, `rbegin()`, and the walk-back phase of iteration.

### 4.4 find next / find prev

Iterator advancement is the second-hottest path after find. The iterator stores a leaf pointer, position within the leaf, and cached key/value data. Advancement operates in two phases:

**Hot path: within-leaf advance.** For compact leaves, the advance is inlined directly into `operator++`: increment position, bounds-check, read `kd[pos]` (a single array load — the key is the full stored representation), and bump the value pointer. For bitmap256 leaves, the advance is also inlined: `next_bit_after` scans the presence bitmap for the next set bit (a word-level shift and `tzcnt`), then the last byte of the cached key is replaced via mask and OR. In the common case (many entries in the same leaf), iteration resolves here without leaving the leaf. This is where the wide-node design pays off: most iterator increments are a few instructions with no function call.

**Cold path: parent-pointer walk.** If the leaf is exhausted (the current position is at the leaf's boundary), the iterator walks upward via parent pointers. Each leaf stores a parent pointer (`node[1]`) and a parent byte (`parent_byte_v` in the header). The walk reads the parent's bitmap and searches for the next sibling after the current byte:

```cpp
while (byte != ROOT_BYTE) {
    auto [sib, found] = bm_next_sibling(parent_bm, byte);
    if (found) return descend_edge_loop(sib, dir);
    byte = parent->parent_byte();
    parent = bm_parent(parent);
}
return end();
```

When a sibling is found, `descend_edge_loop` follows the first (or last) child at each level down to a leaf and returns the edge entry.

The cost of the cold path is one upward step per exhausted level, plus one downward descent to the sibling's edge. In practice, most walks go up one level — the average leaf has many entries, and exhaust events are rare relative to within-leaf advances.

### 4.5 insert

Insert begins with a root prefix check. If the new key diverges from the current root prefix, `reduce_root_skip` shortens the prefix to the point of divergence, creating internal nodes to fan out on the differing byte.

The core insertion is `insert_node`, a runtime-recursive function that descends through three node types. Unlike find, which uses a tight iterative loop, insert accepts recursion because its cost is dominated by allocation and memmove; the function-call overhead per level is negligible by comparison.

**Sentinel.** An empty child slot. Creates a new single-entry compact leaf via `make_single_leaf` and returns it as the new child pointer.

**Leaf.** `leaf_insert` dispatches on `is_bitmap`. For compact leaves, `compact_ops::insert` inserts into the sorted array (shifting entries via memmove, or reallocating if the allocation is full). No skip check — compact leaves store full keys. If the compact leaf exceeds COMPACT_MAX, `convert_to_bitmask_tagged` splits it into an internal node with up to 256 children. Children at the final byte depth become bitmap leaves; others become compact leaves storing the full key. For bitmap256 leaves, `bitmap_insert` sets the presence bit.

**Internal node.** Walks any embed chain skip bytes. If a skip byte diverges, `split_skip_at` creates a new internal node at the divergence point. Otherwise, performs a bitmap lookup for the key byte. If the child doesn't exist, creates a new leaf and adds it to the bitmap via `add_child`. If it exists, recurses into the child. On unwind, updates the child pointer if it changed (due to reallocation or splitting) and increments the descendant count.

After insert returns to the root level, `normalize_root` checks whether the root can absorb additional prefix bytes (if the root is an internal node with uniform first-byte children).

All child creation and child pointer updates maintain parent linkage: when a new leaf or bitmask node is created as a child of a bitmask node, the child's parent pointer and parent byte are set to reference the parent. When a child pointer changes (due to reallocation or splitting), the new child's parent linkage is updated. This invariant ensures that every node in the tree can walk upward to the root at any time, enabling live iterator advancement.

### 4.6 erase

Erase follows the same descent structure as insert: root prefix check, then `erase_node` recurses through sentinel (return immediately), leaf (erase from sorted array or bitmap), and internal node (recurse into child).

The interesting logic is on the unwind path:

**Descendant check.** After a successful erase, the parent's descendant count is decremented. If it drops to COMPACT_MAX or below, `do_coalesce` collects all entries from the subtree into a flat array and rebuilds a single compact leaf, replacing the entire internal node subtree.

**Single-child collapse.** If removing a child leaves an internal node with exactly one remaining child, the node is collapsed according to the child type: a compact child is returned directly (it stores full keys, no skip needed); a bitmap child keeps the one-child bitmask (bitmap needs a dispatch path to reach it); a bitmask child is wrapped in a chain (the dispatch byte is captured as a skip byte).

**Root normalization.** After erase returns to the root level, `normalize_root` absorbs single-child internal roots and `coalesce_bm_to_leaf` handles the case where the total size has dropped below COMPACT_MAX.

For compact leaves, erase removes the entry and shifts subsequent entries down via memmove, an O(N) operation bounded by the leaf's entry count. For bitmap256 leaves, erase clears the presence bit and shifts subsequent values down by one slot, an O(N) operation bounded by 256. If the leaf becomes empty, it is deallocated and the parent removes the child from its bitmap.

### 4.7 Iterators are Live

The kntrie iterator is a live view: it stores a pointer to the current leaf node, a position within that leaf, a cached internal key, and a cached value pointer. Dereferencing returns a `pair<const KEY, VALUE&>` where the value reference points directly into the node's storage. Modifications to the value through the reference are immediately visible.

**Iterator state:**

```cpp
uint64_t* leaf_v;   // current leaf node
uint16_t  pos_v;    // slot index within leaf
uint16_t  bit_v;    // byte value (bitmap leaves), unused for compact
UK        key_v;    // stored key (cached)
void*     val_v;    // pointer into leaf's value array (cached)
```

The cached `key_v` is the complete stored representation read directly from the leaf's key array (compact) or reconstructed from `base_key | byte` (bitmap). `to_user(key_v)` in `operator*()` converts back to the user's key type. For non-trivial value types (Category C), the slot stores a `VALUE*` pointer; `operator*` dereferences through the pointer via `if constexpr (VT::IS_INLINE)` dispatch.

**Advancement** is O(1) amortized. The hot path is inlined directly into `operator++` / `operator--`: for compact leaves, increment position and read `kd[pos]` — a single array load. For bitmap leaves, `next_bit_after` + mask/OR on the cached key. The cold path walks upward via parent pointers and descends to the sibling's edge entry (see section 4.4).

**End sentinel.** The `end()` method constructs a lightweight sentinel iterator with `leaf_v = nullptr`. The sentinel's `val_v` field stashes a pointer to the impl object, enabling `--end()` to find the last entry via `descend_last_loop`.

**Invalidation.** Iterators are invalidated by any mutation to the trie (insert, erase, insert_or_assign). This is more restrictive than `std::map` (which preserves iterators on insert and erase of other keys) or `std::unordered_map` (which preserves iterators on erase of other elements and on non-rehashing inserts). The reason: kntrie mutations can structurally reorganize distant parts of the tree — compact leaf splits, bitmask node coalescing, and node reallocation can move or deallocate the node an iterator points to, even when the mutated key is in a different subtree. Callers must not hold iterators across any mutation.

**API.** The iterator API matches `std::map`: `(*it).second` returns a mutable value reference; `it->first` / `it->second` work via arrow proxy.

## 5 Performance

Performance graphs and detailed benchmark results are maintained separately and updated with each optimization pass. This section describes the conceptual performance characteristics and why they arise.

### 5.1 std::map

`std::map` is a red-black tree, a self-balancing binary search tree where each node contains one key-value pair, two child pointers, a parent pointer, and a color bit. On most implementations (libstdc++/gcc), each node is a separate heap allocation of 64 bytes (key + value + 3 pointers + color + alignment padding).

Red-black trees guarantee O(log N) worst-case lookup, insert, and erase through rotation and recoloring rules that keep the tree approximately balanced. The maximum height is 2 × log₂(N), so a million-entry map requires at most ~40 comparisons per lookup.

The fundamental costs are:

**Memory.** Every entry costs 64 bytes regardless of key or value size. A million uint64_t→uint64_t entries consume 64 MB in `std::map` versus 8–12 MB in kntrie (structural, depending on key distribution), a 5–8× difference.

**Cache behavior.** Each comparison in a tree traversal follows a pointer to a separately-allocated node. These nodes are scattered across the heap in allocation order, not key order. At scale, nearly every level of the tree is a cache miss. A 20-level traversal in a million-entry map touches 20 random cache lines.

**Sorted iteration.** The red-black tree provides naturally sorted in-order traversal, with O(1) amortized iterator increment and decrement. kntrie provides the same sorted iteration with O(1) amortized cost: most increments resolve within the current leaf via a position increment (no search), with parent-pointer walk-back only when a leaf is exhausted.

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

**Compact leaf absorption** catches entire subtrees in a flat sorted array. When a subtree has ≤ 1024 entries, it remains a compact leaf rather than being split into internal nodes. The search within that compact leaf is a branchless binary search: fast, cache-friendly, and often faster than descending further into the trie.

These two mechanisms create a dependency on N for the effective depth:

**N ≤ 1024:** The entire dataset fits in a single compact leaf at the root. Zero internal node descents, just a branchless binary search.

**N ≤ ~262K:** One level of internal dispatch fans out to compact leaves, each holding up to 1024 entries.

**N ≤ ~67M:** Two levels of internal dispatch (256 × 256 × 1024 ≈ 262K). This covers datasets well beyond what most in-memory use cases require.

**Full depth:** Up to K levels of internal dispatch, reached only when subtrees overflow their 1024-entry compact leaves at every level. In practice, even a billion-entry dataset with random uint64_t keys rarely reaches full depth because the keys distribute across the 256 possible byte values at each level.

The key insight: O(K) is the theoretical bound, but the compact leaf mechanism makes the practical behavior closer to O(1) with a binary search whose size grows slowly with N. The kntrie's depth only increases when a key range is dense enough to overflow compact leaves.

### 5.4 The Real Complexity: Memory Hierarchy

Textbook complexity treats memory access as uniform cost. In practice, every pointer chase or array lookup pays a cost determined by where the data lives in the memory hierarchy: L1, L2, L3, or DRAM. As N grows and the working set exceeds each cache level, per-operation cost increases for all three structures.

`std::map`'s O(log N) is well known, but after factoring out the log N tree depth, each individual hop still gets 2-3x more expensive per 10x growth in N, a pure memory hierarchy effect. `std::unordered_map`, supposedly O(1), degrades just as badly: its hash-then-chase-pointer pattern scatters across the same oversized heap.

The kntrie's advantage is twofold. First, much smaller memory footprint means the working set stays in faster cache levels longer. Second, the trie's dense node layout gives spatial locality. Adjacent keys share cache lines, so one fetch services multiple lookups.

In short: as N grows, all three structures pay an increasing cost per operation driven by cache pressure. The kntrie's smaller memory footprint keeps the working set in faster cache levels for larger N.

## 6 Summary

The kntrie combines three inheritances: TRIE-style prefix routing for O(K) lookup independent of collection size, B-tree-style wide sorted leaves for cache-friendly storage and iteration, and bitmap compression for memory-efficient internal dispatch. The result is an ordered associative container that approaches hash-table lookup speed while maintaining full key ordering, with a memory footprint typically one-third that of standard alternatives through prefix compression and contiguous storage. Compact leaves store the full key at every depth, enabling 2–4× faster iteration than `std::map` via single-load key reads. Benchmark data is maintained separately from this document.
