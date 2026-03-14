# KSTRIE Concepts

This document describes the KSTRIE, the variable-length string key instantiation of the KTRIE. For shared data structure concepts (TRIE, B-tree, KTRIE decomposition), binary search, bitmap representation, sentinel design, and value storage categories, see [KTRIE Concepts](../ktrie_concepts.md).

## Table of Contents

- [1 Optimizations](#1-optimizations)
  - [1.1 Key Representation and Character Maps](#11-key-representation-and-character-maps)
  - [1.2 Value Storage: Implementation Details](#12-value-storage-implementation-details)
  - [1.3 Memory Hysteresis](#13-memory-hysteresis)
- [2 Node Concepts](#2-node-concepts)
  - [2.1 Bitmap Dispatch](#21-bitmap-dispatch)
  - [2.2 Node Header](#22-node-header)
  - [2.3 Compact Node Prefix](#23-compact-node-prefix)
- [3 Nodes](#3-nodes)
  - [3.1 Sentinel](#31-sentinel)
  - [3.2 Compact Nodes](#32-compact-nodes)
  - [3.3 Bitmask Nodes](#33-bitmask-nodes)
  - [3.4 Root](#34-root)
- [4 Operations](#4-operations)
  - [4.1 find](#41-find)
  - [4.2 insert](#42-insert)
  - [4.3 erase](#43-erase)
  - [4.4 find first / find last](#44-find-first--find-last)
  - [4.5 find next / find prev](#45-find-next--find-prev)
  - [4.6 Iterators are Snapshots](#46-iterators-are-snapshots)
- [5 Performance](#5-performance)
  - [5.1 std::map](#51-stdmap)
  - [5.2 std::unordered_map](#52-stdunordered_map)
  - [5.3 KSTRIE](#53-kstrie)
  - [5.4 The Real Complexity: Memory Hierarchy](#54-the-real-complexity-memory-hierarchy)
- [6 Summary](#6-summary)

## 1 Optimizations

### 1.1 Key Representation and Character Maps

String keys in the kstrie are byte sequences. The trie branches on individual bytes — each branch level consumes one byte and fans out to up to 256 children. The raw bytes of the string determine the path through the trie, which means the byte ordering of the key determines the lexicographic ordering of the container.

The kstrie uses a **character map** (`char_map`) to transform key bytes before they enter the trie. The map is a compile-time constant: a 256-element `std::array<uint8_t, 256>` that maps each input byte to an output byte. Every key byte passes through this map before dispatch or storage. The map is applied once on entry and inverted (if needed) on output.

**Identity map.** The default `identity_char_map` is the identity function: byte `i` maps to `i`. Keys are stored and sorted in raw byte order. This is the fastest option and preserves the natural byte-level lexicographic ordering.

```cpp
constexpr auto IDENTITY_MAP = [] {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = static_cast<uint8_t>(i);
    return m;
}();
using identity_char_map = char_map<IDENTITY_MAP>;
```

**Case-insensitive maps.** The `lower_char_map` maps uppercase ASCII letters to lowercase, making lookups case-insensitive while preserving all other bytes. The `upper_char_map` maps lowercase to uppercase. Both are many-to-one mappings: 'A' and 'a' map to the same value, so they sort to the same position and are treated as equal by find.

```cpp
constexpr auto LOWER_MAP = [] {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = static_cast<uint8_t>(i);
    for (int i = 'A'; i <= 'Z'; ++i) m[i] = static_cast<uint8_t>(i + 32);
    return m;
}();
using lower_char_map = char_map<LOWER_MAP>;
```

**Creating a custom map.** A user defines a custom character map by constructing a 256-element array and wrapping it in `char_map`:

```cpp
// Example: map that treats digits 0-9 as equivalent (all map to '0')
constexpr auto MY_MAP = [] {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = static_cast<uint8_t>(i);
    for (int i = '0'; i <= '9'; ++i) m[i] = '0';
    return m;
}();
using my_char_map = char_map<MY_MAP>;

// Use it:
kstrie<int, my_char_map> trie;
```

The character map is a template parameter, resolved entirely at compile time. Each unique map produces a distinct template instantiation with the map baked into every byte-level operation. There is no runtime dispatch cost; the compiler inlines the map lookup into the dispatch and comparison paths.

Many-to-one maps (like case folding) reduce the effective fan-out at branch levels, which makes nodes denser and can improve memory efficiency. The trade-off is that keys that differ only in mapped-away distinctions become equivalent and cannot coexist in the container.

### 1.2 Value Storage: Implementation Details

The three value storage categories (A: trivial inline, B: packed bool, C: heap pointer) are described in [KTRIE Concepts](../ktrie_concepts.md). This section covers KSTRIE-specific implementation details.

**Category A storage.** Values are stored directly in the value region of the compact node via memcpy. For the common case of `kstrie<int>`, each value is 4 bytes (padded to u64 alignment in the value region).

**Category B bit operations.** Bool values are packed into `uint64_t` words with one bit per entry. The `load_value` function returns a pointer to one of two static constants:

```cpp
static constinit bool BOOL_VALUES[2] = {false, true};
return &BOOL_VALUES[(base[index >> LOG2_BPW] >> (index & WORD_BIT_MASK)) & 1];
```

One shift, one mask, one array index. Branchless. The static constants are thread-safe and their pointers are stable forever. Bit-shifting for insert and erase within the bitmap uses u64-level shift-with-carry operations that the compiler auto-vectorizes into AVX2 `vpsllq`/`vpsrlq` instructions on x86.

The compile-time booleans that drive all internal dispatch:

```
IS_TRIVIAL     = trivially_copyable && sizeof <= 8    // A: true    B: false  C: false
IS_BITMAP      = std::is_same_v<VALUE, bool>          // A: false   B: true   C: false
IS_INLINE      = !IS_BITMAP && IS_TRIVIAL             // A: true    B: false  C: false
```

All node code operates on `value_base_t` uniformly, where `value_base_t` is `VALUE` for category A, `uint64_t` for categories B and C.

**Abstraction responsibilities:**

**kstrie** (API boundary): Accepts `std::string_view` keys and `VALUE` values. Converts keys to byte sequences via the character map. Forwards to `kstrie_impl`.

**kstrie_impl** (node ownership): Owns all node allocation and destruction. On clear/destructor: `destroy_tree` walks all nodes, calls `destroy_values` on every compact leaf's value region if the value type requires it, then frees node memory. On erase: `do_leaf_erase` calls `destroy_value` for the erased entry's slot.

**kstrie_compact** (compact node operations): Handles insert, erase, grow, shrink, split, and the keysuffix sharing invariant. Uses `slots::store_raw`, `slots::load_raw`, `slots::copy_values`, and `slots::move_values` for all value manipulation. Never calls `new` or `delete` directly.

**kstrie_bitmask** (bitmask node operations): Handles child dispatch, child insertion and removal, skip prefix management, and EOS child storage.

### 1.3 Memory Hysteresis

Node allocations snap to size classes via `padded_size`. The allocation granularity grows with node size: small nodes round to nearby sizes; large nodes round to power-of-two with midpoints. The worst-case overhead is about 50%.

This padding creates room for in-place operations. A node allocated larger than its current needs can absorb insertions without reallocation. The keysuffix region can grow into the padding between the offset array and the value slots.

**Shrink hysteresis.** A compact node only shrinks when `count < cap / 2`. This prevents oscillation: if a node sits near a boundary, alternating insert/erase won't trigger repeated reallocation cycles.

**In-place keysuffix shuffle.** When the keysuffix region is full but the overall allocation has unused space beyond the value slots, the kstrie can shift the value slots forward within the same allocation to make room for more keysuffix bytes. This avoids a full reallocation for the common case where a few more suffix bytes are needed but the node's total allocation has capacity.

## 2 Node Concepts

### 2.1 Bitmap Dispatch

The shared bitmap representation and popcount mechanics are described in [KTRIE Concepts](../ktrie_concepts.md). The KSTRIE uses branchless popcount dispatch: if the target byte is present, the popcount gives the 0-based index into the dense child array. If the target byte is absent, the dispatch falls through to the sentinel. The sentinel is a static node that always returns "not found," so no conditional branch is needed at the bitmap level.

### 2.2 Node Header

All allocated nodes in the kstrie share a common 8-byte header at `node[0]`:

| Field | Purpose |
|-------|---------|
| `alloc_u64` | Allocation size in u64 units (set by allocator, not modified by node logic) |
| `count` | For compact: entry count. For bitmask: child count in bitmap |
| `flags` | Node type: `0` = compact, `FLAG_BITMASK` = bitmask |
| `skip` | Number of prefix bytes captured at this node (0–254) |
| `slots_off` | Cached u64 offset from node start to the value slots region |

The `flags` field distinguishes the two node types. A single bit test (`h.is_compact()` or `h.is_bitmap()`) determines the node type at any point in the code. There are no tagged pointers; the type is stored in the node itself.

`slots_off` is a cached value that allows the value region to be located in one addition: `node + slots_off`. This avoids recomputing the offset from the node layout on every value access. It is updated when the layout changes (grow, shrink, keysuffix shuffle).

`skip` records how many bytes of shared prefix are stored at this node. For compact nodes, skip bytes are stored in the prefix region of the header. For bitmask nodes, skip bytes are stored after the header. When a lookup reaches a node with skip > 0, it compares the skip bytes against the corresponding key bytes. A mismatch means the key is not in this subtree.

### 2.3 Compact Node Prefix

Compact nodes carry an additional prefix structure beyond the common header, stored in the second u64 (`node[1]`):

| Field | Purpose |
|-------|---------|
| `cap` | Capacity: maximum number of entries before reallocation |
| `keysuffix_used` | Bytes currently used in the keysuffix region |

`cap` determines the size of the parallel arrays (L, F, O) and the value region. It is derived from the allocation size: given the total allocation in u64s, `cap` is the largest entry count that fits within the allocation.

`keysuffix_used` tracks how many bytes of the keysuffix region are occupied. When `keysuffix_used` would exceed the available space (determined by the gap between the offset array and the value slots), the node must either shuffle slots forward, grow, or split.

## 3 Nodes

### 3.1 Sentinel

The sentinel concept is described in [KTRIE Concepts](../ktrie_concepts.md). The KSTRIE sentinel is a static compact node with zero entries:

```cpp
static inline constinit uint64_t sentinel_data_[3] = {};
```

Three u64s of zeros. The header reads as a compact node with `count = 0`, `flags = 0`, `skip = 0`. Any operation that reaches the sentinel finds nothing: `find` returns nullptr, erase returns MISSING.

The sentinel is 3 u64s rather than 2 to ensure that any code reading the prefix region at `node[1]` accesses valid (zero) memory. Since it is static and never modified, it is shared across all kstrie instances of the same type.

The sentinel appears in two roles:

- **Empty root.** When the kstrie has no entries, the root pointer is the sentinel. Find reaches it, sees count = 0, returns nullptr.
- **Bitmask node miss target.** Bitmask nodes store the sentinel as their EOS child when no end-of-string entry exists. Child dispatch for absent bytes also resolves to the sentinel. The find loop follows the dispatch, reaches the sentinel, and returns not-found without a conditional branch.

### 3.2 Compact Nodes

Compact nodes implement the KTRIE's SUFFIX concept: they store key-suffix/value pairs in parallel sorted arrays within a single allocation. They are the only node type that holds entries directly. Bitmask nodes route; compact nodes store.

**Layout:**

```
[Header: 2 u64]
[L: cap bytes]          — suffix lengths
[F: cap bytes]          — suffix first bytes
[O: cap × offset_type]  — keysuffix offsets
[keysuffix: variable]   — suffix tail bytes, contiguous
[values: variable]      — value slots (inline, bitmap, or pointer)
```

The header occupies 2 u64s: the common node header at `node[0]` and the compact prefix (cap, keysuffix_used) at `node[1]`. The remaining space is divided among five regions.

**L (lengths).** `L[i]` is the total suffix length for entry `i`: the first byte plus the tail. `L[i] = 0` means the entry has no suffix (it matched exactly at the dispatch point — an end-of-string entry). `L[i] = 1` means a single byte with no tail. `L[i] = k` means the suffix is `k` bytes: first byte `F[i]` followed by `k-1` tail bytes in the keysuffix region.

**F (firsts).** `F[i]` is the first byte of entry `i`'s suffix, after character map transformation. Entries are sorted by `(F[i], tail bytes)`. For entries with `L[i] = 0`, `F[i]` is undefined and not used in comparisons.

**O (offsets).** `O[i]` is the byte offset into the keysuffix region where entry `i`'s tail bytes begin. The type of O is `ks_offset_type`, a compile-time alias:

```cpp
using ks_offset_type = std::conditional_t<COMPACT_KEYSUFFIX_LIMIT <= 256, uint8_t, uint16_t>;
```

At the default `COMPACT_KEYSUFFIX_LIMIT = 4096`, offset_type is `uint16_t` (2 bytes per entry). At limit ≤ 256, it would be `uint8_t` (1 byte per entry).

**Keysuffix.** A contiguous byte region holding the tail bytes (all suffix bytes after the first) for all entries. The region has `keysuffix_used` live bytes. Entries reference into this region via their `O[i]` offset and read `L[i] - 1` bytes starting there.

**Values.** The value slot region begins at offset `slots_off` (in u64 units from node start). For category A (inline), values are packed directly. For category B (bitmap), values are packed as bits in u64 words. For category C (heap), 8-byte pointers are stored.

#### 3.2.1 Search

Finding an entry in a compact node is a two-phase process. The first phase locates the candidate position using the first-byte array F. The second phase verifies the full suffix against the keysuffix region.

**Phase 1: binary search on F.** Entries are sorted by `(F[i], tail)`. A binary search over the F array finds the range of entries matching the target's first byte. Because F is a dense byte array, this search touches minimal cache lines.

**Phase 2: suffix comparison.** Within the range of entries sharing the same first byte, a linear scan compares each entry's full suffix (first byte + tail bytes from keysuffix) against the lookup key's remaining bytes. The scan is short because entries sharing a first byte are typically few.

For entries with `L[i] = 0` (end-of-string), the match succeeds only if the lookup key is also exhausted at this point.

#### 3.2.2 Keysuffix Sharing

Within each compact node, entries whose suffixes share a common prefix share the same bytes in the keysuffix region. The shorter entry's tail is a prefix of the longer entry's tail, so the shorter entry points into the longer entry's byte region at the same offset.

**Example.** Given entries "absolutist" (tail: "ist"), "absolutistic" (tail: "istic"), and "absolutistically" (tail: "istically"), only the longest tail "istically" is stored in the keysuffix region. The shorter entries reference a prefix of the same bytes:

```
keysuffix: ...i s t i c a l l y...
                                 ^
O["absolutist"]       → offset, L=3  → reads "ist"
O["absolutistic"]     → offset, L=5  → reads "istic"
O["absolutistically"] → offset, L=9  → reads "istically"
```

All three entries share the same `O[i]` value. The length `L[i]` determines how many bytes each reads.

This sharing is not an optional optimization applied after construction. It is an **invariant**: the keysuffix region is always shared regardless of the operation path — insert, erase, grow, shrink, build, or collapse. The physical representation is independent of insertion order.

Sharing reduces the keysuffix region size, which delays node splits and allows more entries per compact node. For key distributions with many shared prefixes (URLs, file paths, domain names), the savings are substantial.

#### 3.2.3 Insert

Insertion into a compact node follows one of four cases, determined by the relationship between the new entry's suffix and existing entries:

**Case 1: Share into next.** The new entry's tail is a prefix of the next entry's tail in sorted order, and both share the same first byte. The new entry shares the next entry's offset. Zero keysuffix bytes are added.

**Case 2: Standalone.** The new entry's tail doesn't share a prefix with any adjacent entry. The full tail is inserted into the keysuffix region at the appropriate position. All offsets at or above the insertion point are adjusted.

**Case 3: New longest in chain.** The new entry extends an existing sharing chain. Its tail is longer than the current chain head. Extra bytes are appended to the chain's region. Offsets are adjusted for the growth.

**Case 4: Chain split.** The new entry has a tail that partially overlaps a sharing chain but diverges. The existing chain head's unique bytes are saved, the new entry's bytes are written, and the saved bytes are appended. Both the new entry and the displaced chain head get correct offsets.

Before insertion, a **pre-split check** computes the exact keysuffix delta for the operation. If `keysuffix_used + delta > COMPACT_KEYSUFFIX_LIMIT`, the node is split into a bitmask subtree before the insert proceeds.

#### 3.2.4 Erase

Erasure from a compact node follows one of three cases:

**Case A: Chain sharer.** The erased entry shares its offset and first byte with the next entry. Removing it doesn't change the keysuffix region — the shared bytes are still needed.

**Case B: Chain head.** The erased entry is the longest in a sharing chain. The reclaimed bytes are the difference between the erased entry's tail length and the next-longest sharer's tail length. The reclaimed bytes are removed from the end of the chain's region.

**Case C: Standalone.** The erased entry's tail bytes are entirely its own. All of them are reclaimed from the keysuffix region.

In all cases, the L, F, O, and value arrays are shifted to close the gap left by the erased entry.

#### 3.2.5 Grow and Shrink

**Grow.** When a compact node needs more capacity (either more entries than `cap` or more keysuffix space than the allocation allows), a new, larger node is allocated. The L, F, O, keysuffix, and value regions are memcpy'd into the new allocation. The caller (insert) then modifies the keysuffix region in the new node. Sharing is preserved because the keysuffix bytes and offsets are copied verbatim.

**Shrink.** After erasure, if `count < cap / 2`, a new, smaller node is allocated and the contents are memcpy'd. Same logic as grow: pure copy, sharing preserved.

Both operations are single-allocation replacements. The old node is freed after the copy.

### 3.3 Bitmask Nodes

Bitmask nodes implement the KTRIE's BRANCH concept: 256-way fan-out dispatching on one byte of the key per node. They use the `bitmap_256_t` to compress the 256-entry child array to only the children that exist.

**Layout:**

```
[Header: 1 u64]
[Skip bytes: 0–254 bytes, padded to u64 alignment]
[Bitmap: 4 u64]
[EOS child: 1 u64]
[Child slots: N u64]
[Total tail: 1 u64]
```

The header is a single u64 `node_header_t` with `flags = FLAG_BITMASK`. Skip bytes (if any) follow the header, storing the prefix bytes shared by all entries in this subtree. The bitmap records which of the 256 possible byte values have children. The EOS (end-of-string) child is a pointer to a compact node holding entries whose keys end at this branch point — keys that have been fully consumed by the trie path and have no remaining bytes to dispatch on. The N child slots are u64 pointers packed densely in bitmap order. The total tail counter at the end holds a byte-budget estimate for collapse decisions.

#### 3.3.1 EOS (End of String)

Variable-length string keys can end at any point in the trie. When a key is exactly as long as the prefix consumed by the path from root to a bitmask node, there is no remaining byte to dispatch on. The entry is stored in the EOS child, a compact node attached to the bitmask node outside the bitmap.

The EOS child is typically a small compact node (often a single entry). It is checked when the lookup key is exhausted at a bitmask node. If the EOS child is the sentinel, the key does not exist.

#### 3.3.2 Skip Prefix

When all children of a bitmask node share a common prefix at a given depth, those bytes are captured as a skip prefix rather than creating a chain of single-child bitmask nodes. The skip byte count is stored in the header's `skip` field, and the actual bytes are stored after the header.

During lookup, the skip bytes are compared against the key. Three outcomes:

**MATCHED.** All skip bytes match. Lookup continues to the bitmap dispatch with the remaining key.

**MISMATCH.** A skip byte differs from the key byte at some position. The key is not in this subtree. For find, this is an immediate not-found. For insert, this triggers a prefix split: a new bitmask node is created at the divergence point.

**KEY_EXHAUSTED.** The lookup key is shorter than the skip prefix. The key ends within the prefix. For find, this is not-found (the key would need more bytes to reach any entry). For insert, a new bitmask node is created with the key's entry as an EOS child and the existing subtree as a byte-dispatched child.

#### 3.3.3 Dispatch

Child lookup uses branchless popcount dispatch as described in [KTRIE Concepts](../ktrie_concepts.md). Given a byte value, the bitmap is checked for the byte's presence and the popcount gives the position in the dense child array:

```cpp
uint64_t* child = dispatch(node, h, byte);
```

If the byte is absent, dispatch returns the sentinel. The find loop follows the sentinel, reaches count = 0, and returns nullptr.

#### 3.3.4 Total Tail

Each bitmask node stores a `total_tail` value: the estimated total byte cost of collapsing the entire subtree back into a single compact node. This estimate accounts for three components per entry in the subtree:

1. **Keysuffix bytes**: the suffix tail bytes stored in compact leaf children.
2. **Dispatch bytes**: one byte per entry for the byte consumed at this bitmask level.
3. **Skip bytes**: for each nested bitmask child with skip = S, every entry passing through it would gain S prefix bytes in the collapsed representation.

The total_tail is maintained incrementally. On insert, the delta propagates upward: the keysuffix delta from the compact leaf, plus one dispatch byte, plus the skip bytes at each bitmask level. On erase, the same values are subtracted.

The collapse check is: `total_tail <= COMPACT_KEYSUFFIX_LIMIT`. If true, the subtree can fit in a single compact node's keysuffix region. This avoids trial collapses — walking an entire subtree only to discover it won't fit — which was a significant performance problem before this heuristic was introduced.

### 3.4 Root

The root of the kstrie is a single `uint64_t*` that can point to any node type: the sentinel (when empty), a compact node (for small collections), or a bitmask node (for large collections that require branch dispatch).

When the kstrie is empty, the root is the sentinel. The first insert creates a compact node. As entries accumulate, the compact node may split into a bitmask node with compact children. On erase, if the total entry count drops low enough, the bitmask subtree may collapse back into a single compact node.

The root has no special prefix mechanism beyond what the root node itself provides. If the root is a bitmask node with a skip prefix, that prefix covers all entries in the trie.

The kstrie object itself stores: the root pointer, the entry count (`size_`), and the memory allocator. The entry count is maintained by increment on insert and decrement on erase; it is not derived from the tree structure.

## 4 Operations

### 4.1 find

Find is the hot path. It descends from the root through bitmask nodes to a compact leaf.

The outer loop tests each node's type. At a bitmask node, find first checks the skip prefix (if any). If the skip matches, it extracts the next byte from the key and dispatches through the bitmap. If the key is exhausted at a bitmask node (no more bytes to dispatch), find checks the EOS child. If the dispatched child is the sentinel, the key is absent.

At a compact node, find performs the two-phase search described in section 3.2.1: binary search on F to find the first-byte range, then suffix comparison within that range.

The entire find path is a tight loop with no heap allocation. The loop body is:

```
while (node is bitmask):
    check skip prefix → mismatch: return nullptr
    key exhausted? → check EOS child
    byte = key[consumed++]
    node = dispatch(node, byte)    // branchless popcount
at compact node:
    binary search + suffix compare
```

### 4.2 insert

Insert begins by descending through the trie, mirroring find. At each bitmask node, the skip prefix is checked. Three outcomes change the control flow:

**MATCHED.** Normal case. If the key is exhausted, insert into the EOS child. Otherwise, dispatch on the next byte. If the byte has no child, create a new compact node with a single entry and attach it. If the byte has a child, recurse into it.

**MISMATCH.** The new key diverges from the skip prefix. A new bitmask node is created at the divergence point with two children: the existing subtree (reskipped past the divergence) and a new node for the inserted key.

**KEY_EXHAUSTED.** The new key is shorter than the skip prefix. A new bitmask node is created at the exhaustion point with the existing subtree as a byte-dispatched child and the new entry as an EOS child.

At a compact node, the entry is inserted according to the four cases described in section 3.2.3. If the insert would overflow `COMPACT_KEYSUFFIX_LIMIT`, the compact node is split into a bitmask subtree: entries are grouped by first byte, each group becomes a compact child, and the bitmask node dispatches among them.

On the unwind path, each bitmask node's `total_tail` is updated with the delta from the operation.

### 4.3 erase

Erase descends through the trie, mirroring find. At a compact node, it locates the entry and returns a PENDING status with the node and position. The actual erasure is deferred to the parent, which calls `erase_in_place` to remove the entry according to the three cases described in section 3.2.4.

On the unwind path after a successful erase:

**Empty node.** If a compact node becomes empty (count = 0), it is freed and the parent removes the child from its bitmap (or clears the EOS child).

**Shrink check.** If `count < cap / 2`, the compact node is shrunk to a smaller allocation.

**Collapse check.** At each bitmask node, the `total_tail` is decremented. If it drops to `COMPACT_KEYSUFFIX_LIMIT` or below, `collapse_to_compact` attempts to collapse the entire subtree into a single compact node. The collapse walks the subtree, collecting all entries into stack-local arrays (L, F, O, keysuffix, values), applies the sharing invariant to the collected entries, and builds a new compact node. If the collapsed keysuffix exceeds the limit (the heuristic was slightly off due to prefix length variations), the collapse is abandoned and the bitmask structure is retained.

**Single-child bitmask.** If removing a child leaves a bitmask node with zero children and no EOS, it is freed.

### 4.4 find first / find last

`iter_first` and `iter_last` find the minimum and maximum entries in the trie. They descend through bitmask nodes following the first or last child at each level. At a bitmask node, the first child is found by scanning the bitmap for the lowest set bit; the last child by scanning for the highest set bit. The EOS child, if present, precedes all byte-dispatched children in key order (a zero-length suffix sorts before any non-empty suffix). At a compact node, the first entry is at position 0 and the last at position `count - 1`.

These operations build the full key as they descend: each skip prefix and dispatch byte is appended to a string that, upon reaching the leaf, represents the complete key of the minimum or maximum entry.

### 4.5 find next / find prev

Iterator advancement is the second-hottest path after find. `iter_next` finds the smallest key strictly greater than the current key.

**Leaf probe.** The descent reaches the compact leaf containing (or nearest to) the current key. Within the leaf, the next entry after the current key's position is checked. If it exists and its full key is greater, the iterator advances within the leaf. This is the common case: most iterator increments resolve within a single leaf.

**Walk-back.** If the current key is the leaf's last entry, the descent unwinds through bitmask nodes. At each level, the next sibling in the bitmap (the next set bit after the current byte) is found. If a sibling exists, `iter_first` descends to its minimum entry. If no sibling exists, the walk-back continues to the parent. The EOS child is considered before byte-dispatched children in the ordering.

`iter_prev` is the mirror image: it checks the previous entry in the leaf, then walks back through the bitmap looking for the previous sibling, and descends to the maximum entry of that sibling's subtree.

The key is reconstructed during descent. Each skip prefix and dispatch byte is tracked so that the full key is available when the target entry is reached.

### 4.6 Iterators are Snapshots

The kstrie iterator is a snapshot: it stores a copy of the current key (as `std::string`) and a copy of the current value, not a pointer into the trie. Each `operator++` and `operator--` re-descends the trie from the root via `iter_next` or `iter_prev` to find the next or previous entry.

This design has two consequences:

**Stability.** Iterators are never invalidated by mutations to other keys. Inserting or erasing a different key does not affect an existing iterator's stored key/value pair. The iterator will find the correct next/previous entry on its next advance, even if the trie's internal structure has been reorganized by splits, collapses, or reallocation.

**No auto-update.** If the value at the iterator's current key is modified (via `insert_or_assign`), the iterator still holds the old value. Dereferencing returns the snapshot, not the live data. To see the updated value, the caller must re-find the key or advance and return.

The cost of this approach is that each iterator increment performs a full root-to-leaf descent rather than following a stored pointer. In practice this cost is low: most increments resolve within a single compact leaf, and the descent through bitmask nodes is the same tight loop as find. The benefit is simplicity: no iterator bookkeeping, no invalidation tracking, and no dangling pointer risk.

## 5 Performance

Performance graphs and detailed benchmark results are maintained separately and updated with each optimization pass. This section describes the conceptual performance characteristics and why they arise.

### 5.1 std::map

`std::map` is a red-black tree, a self-balancing binary search tree where each node contains one key-value pair, two child pointers, a parent pointer, and a color bit. On most implementations, each node is a separate heap allocation. For string-keyed maps, each node carries the overhead of the tree structure plus the overhead of the string itself (typically 32 bytes for the `std::string` object plus any heap-allocated buffer for strings longer than the SSO threshold).

Red-black trees guarantee O(log N) worst-case lookup, insert, and erase through rotation and recoloring rules that keep the tree approximately balanced. Each lookup comparison requires a full key comparison (O(K) per comparison for string keys of length K), giving a total complexity of O(K log N).

The fundamental costs are:

**Memory.** Every entry costs the tree node overhead plus the full key storage. Adjacent entries with similar keys get no benefit from their similarity. The key "https://example.com/users/12345" and "https://example.com/users/12346" each store all 30+ bytes independently.

**Cache behavior.** Each comparison in a tree traversal follows a pointer to a separately-allocated node. These nodes are scattered across the heap in allocation order, not key order. At scale, nearly every level of the tree is a cache miss. Each comparison also accesses the key string, which may be on a separate cache line from the node itself.

**Sorted iteration.** The red-black tree provides naturally sorted in-order traversal, with O(1) amortized iterator increment and decrement. The kstrie provides the same sorted iteration, but resolves most increments within a single compact leaf rather than following parent/child pointers.

### 5.2 std::unordered_map

`std::unordered_map` is a hash table, typically implemented as an array of buckets with separate chaining. Each entry is a heap-allocated node containing the key, value, hash, and a next pointer. Per-entry overhead is roughly the string key size plus 40-64 bytes for structural overhead.

Hash tables offer O(K) expected-case lookup for string keys: compute the hash (O(K)), index into the bucket array, then walk a short chain with O(K) string comparisons per chain element.

The fundamental costs are:

**Memory.** Each entry carries per-node overhead (next pointer, hash, alignment padding) plus the complete key string. At low load factors the bucket array wastes space; at high load factors chains lengthen. Rehashing requires allocating a new bucket array and re-inserting every entry.

**Cache behavior.** The hash function deliberately scatters keys across the bucket array, destroying any locality the input data might have had. Walking a chain follows heap-scattered pointers.

**No ordered iteration.** Hash tables provide no key ordering. Iterating in sorted order requires copying keys to a separate container and sorting.

**No compression.** Every entry stores the complete key. A million URLs sharing the prefix "https://example.com/" store that prefix a million times.

### 5.3 KSTRIE

The kstrie's lookup complexity is O(K) where K is the key length in bytes. Unlike kntrie where K is a compile-time constant, K varies per key. The number of bitmask levels traversed depends on how many bytes of the key are consumed by branch dispatch versus prefix capture versus suffix storage in compact leaves.

In practice, the effective depth is often much less than K because of three mechanisms:

**Skip prefix capture** collapses levels where all keys in a subtree share common bytes. A subtree where 1000 URLs share the prefix "https://example.com/" captures that entire prefix in one comparison rather than 20 bitmask levels.

**Compact leaf absorption** catches entire subtrees in a flat sorted array. When a subtree's total keysuffix byte cost is within `COMPACT_KEYSUFFIX_LIMIT`, it remains a compact leaf rather than being split into bitmask nodes. The search within that compact leaf is a binary search on first bytes followed by suffix comparison.

**Keysuffix sharing** reduces the effective size of compact leaves by storing shared prefixes only once. This allows more entries per leaf, which delays splits and reduces the total number of bitmask levels.

For small collections (hundreds to low thousands of entries), the entire dataset typically fits in a single compact node. For larger collections, one or two levels of bitmask dispatch fan out to compact leaves. The kstrie rarely exceeds 3-4 bitmask levels even for millions of entries, because each level resolves one byte of divergence and compact leaves absorb the remainder.

### 5.4 The Real Complexity: Memory Hierarchy

Textbook complexity treats memory access as uniform cost. In practice, every pointer chase or array lookup pays a cost determined by where the data lives in the memory hierarchy: L1, L2, L3, or DRAM. As N grows and the working set exceeds each cache level, per-operation cost increases for all three structures.

`std::map`'s O(K log N) is well known, but after factoring out the tree depth, each individual hop still gets 2-3x more expensive per 10x growth in N, a pure memory hierarchy effect. `std::unordered_map`, supposedly O(K), degrades similarly: its hash-then-chase-pointer pattern scatters across the same oversized heap, and each string comparison pulls in key data from unpredictable locations.

The kstrie's advantage is twofold. First, much smaller memory footprint means the working set stays in faster cache levels longer. Prefix compression eliminates redundant key storage; sharing reduces suffix storage; compact leaves pack entries contiguously. Second, the trie's dense node layout gives spatial locality. Adjacent keys share compact leaves, so one cache line fetch services multiple lookups.

In short: as N grows, all three structures pay an increasing cost per operation driven by cache pressure. The kstrie's smaller memory footprint keeps the working set in faster cache levels for larger N.

## 6 Summary

The kstrie combines three inheritances: TRIE-style prefix routing for O(K) lookup independent of collection size, B-tree-style wide sorted leaves for cache-friendly storage and iteration, and bitmap compression for memory-efficient branch dispatch. It extends the KTRIE design to variable-length string keys through keysuffix sharing within compact leaves, EOS handling at bitmask nodes, and a byte-budget collapse heuristic that avoids wasted subtree traversals. The result is an ordered associative container that approaches hash-table lookup speed while maintaining full lexicographic ordering, with a memory footprint that falls well below `std::unordered_map` through prefix capture, suffix sharing, and contiguous leaf storage. Benchmark data is maintained separately from this document.
