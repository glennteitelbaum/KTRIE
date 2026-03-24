# kstrie Live Iterator Conversion — kstrie_live.md

## 1 Problem

kstrie uses snapshot iterators: `operator++` re-descends from root,
`operator*` returns a copy. No live value references, no `it->second += delta`.

## 2 Current Architecture (Unchanged)

Compact node alloc sizing, keysuffix sharing, binary search on F,
insert/erase/grow/shrink/split/coalesce internals, bitmask dispatch,
value storage categories, sentinel design, character map system — all stable.

## 3 Layout Changes

### 3.1 kstrie_support.hpp — Node word indices

```
OLD:
inline constexpr size_t NODE_HEADER     = 0;
inline constexpr size_t NODE_TOTAL_TAIL = 1;

NEW:
inline constexpr size_t NODE_HEADER     = 0;
inline constexpr size_t NODE_PARENT_PTR = 1;   // bitmask: parent pointer
inline constexpr size_t NODE_TOTAL_TAIL = 2;   // bitmask: keysuffix heuristic
```

All 33 `node[NODE_TOTAL_TAIL]` references auto-update.

### 3.2 kstrie_support.hpp — COMPACT_ARRAYS_OFF

```
OLD:
static constexpr size_t COMPACT_ARRAYS_OFF = 2 * U64_BYTES;   // 16

NEW:
static constexpr size_t COMPACT_ARRAYS_OFF = 3 * U64_BYTES;   // 24
// node[0] = header, node[1] = ck_prefix, node[2] = parent_ptr
```

All `lengths()`, `firsts()`, `offsets()` auto-update (they use COMPACT_ARRAYS_OFF).

### 3.3 kstrie_support.hpp — node_header::node_size() bitmask path

```
OLD:
size_t u64s = 2 + BITMAP_WORDS + 1 + count + 1;

NEW:
size_t u64s = 3 + BITMAP_WORDS + 1 + count + 1;
// header(1) + parent_ptr(1) + total_tail(1) + bitmap + sentinel + children + eos
```

### 3.4 kstrie_support.hpp — get_skip() bitmask path

```
OLD:
return reinterpret_cast<uint8_t*>(node + SENTINEL_OFF + 1 + h.count + 1);

NEW:
// auto-correct: SENTINEL_OFF is derived from CHILD_BITMAP_OFF in bitmask.hpp
// which shifts from 2→3, so SENTINEL_OFF shifts by +1 automatically.
// No code change needed here — just verify.
```

### 3.5 kstrie_support.hpp — Parent pointer accessors (new)

```
NEW (add after get_skip):

// --- Compact node parent ---
// parent_ptr at node[2]. parent_byte in ck_prefix.parent_byte.

// --- Bitmask node parent ---
// parent_ptr at node[NODE_PARENT_PTR]. parent_byte in slots_off field.

inline constexpr uint16_t ROOT_PARENT_BYTE = 256;
```

### 3.6 kstrie_compact.hpp — ck_prefix: reserved_ → parent_byte

```
OLD:
struct ck_prefix {
    uint16_t cap;
    uint16_t keysuffix_used;
    uint16_t skip_data_off;
    uint16_t reserved_padding_;
};

NEW:
struct ck_prefix {
    uint16_t cap;
    uint16_t keysuffix_used;
    uint16_t skip_data_off;
    uint16_t parent_byte;       // dispatch byte in parent (ROOT_PARENT_BYTE = root)
};
```

### 3.7 kstrie_compact.hpp — alloc_compact_ks: init parent_byte

```
OLD (line ~215):
p.reserved_padding_ = 0;

NEW:
p.parent_byte = ROOT_PARENT_BYTE;
```

### 3.8 kstrie_compact.hpp — compact parent accessors (new)

```
NEW (add to kstrie_compact):

static constexpr size_t COMPACT_PARENT_PTR = 2;  // node[2]

static uint64_t* get_parent(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t*>(
        static_cast<std::uintptr_t>(node[COMPACT_PARENT_PTR]));
}
static void set_parent(uint64_t* node, uint64_t* parent) noexcept {
    node[COMPACT_PARENT_PTR] = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(parent));
}
static uint16_t get_parent_byte(const uint64_t* node, const hdr_type& h) noexcept {
    return get_prefix(node, h).parent_byte;
}
static void set_parent_byte(uint64_t* node, const hdr_type& h, uint16_t byte) noexcept {
    get_prefix(node, h).parent_byte = byte;
}
```

### 3.9 kstrie_bitmask.hpp — Offset constants

```
OLD:
static constexpr size_t CHILD_BITMAP_OFF = 2;
static constexpr size_t SENTINEL_OFF     = CHILD_BITMAP_OFF + BITMAP_WORDS;
static constexpr size_t CHILD_SLOTS_OFF  = SENTINEL_OFF + 1;

NEW:
static constexpr size_t CHILD_BITMAP_OFF = 3;
// header(1) + parent_ptr(1) + total_tail(1), then bitmap
static constexpr size_t SENTINEL_OFF     = CHILD_BITMAP_OFF + BITMAP_WORDS;
static constexpr size_t CHILD_SLOTS_OFF  = SENTINEL_OFF + 1;
```

SENTINEL_OFF and CHILD_SLOTS_OFF auto-update (derived).

### 3.10 kstrie_bitmask.hpp — BITMAP_BYTE_OFF

```
OLD:
static constexpr size_t BITMAP_BYTE_OFF = 2 * U64_BYTES;

NEW:
static constexpr size_t BITMAP_BYTE_OFF = CHILD_BITMAP_OFF * U64_BYTES;
// Derived from CHILD_BITMAP_OFF — auto-updates.
```

### 3.11 kstrie_bitmask.hpp — needed_u64

```
OLD:
// header(1) + desc(1) + bitmap(BU) + sentinel(1) + children(Nc) + eos(1) + skip
size_t u64s = 2 + BITMAP_WORDS + 1 + child_count + 1;

NEW:
// header(1) + parent_ptr(1) + total_tail(1) + bitmap(BU) + sentinel(1) + children(Nc) + eos(1) + skip
size_t u64s = 3 + BITMAP_WORDS + 1 + child_count + 1;
```

### 3.12 kstrie_bitmask.hpp — create / create_with_children: init parent_ptr

```
OLD (in create):
node[NODE_TOTAL_TAIL] = 0;
slots::store_child(node + SENTINEL_OFF, 0, compact_type::sentinel());

NEW (in create):
node[NODE_PARENT_PTR] = 0;  // null parent until linked
node[NODE_TOTAL_TAIL] = 0;
slots::store_child(node + SENTINEL_OFF, 0, compact_type::sentinel());
```

Same for `create_with_children`.

### 3.13 kstrie_bitmask.hpp — reskip: copy parent_ptr

```
OLD (in reskip):
nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];

NEW (in reskip):
nn[NODE_PARENT_PTR] = node[NODE_PARENT_PTR];
nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];
```

### 3.14 kstrie_bitmask.hpp — insert_child realloc: copy parent_ptr

```
OLD (in insert_child realloc path):
nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];

NEW:
nn[NODE_PARENT_PTR] = node[NODE_PARENT_PTR];
nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];
```

### 3.15 kstrie_bitmask.hpp — bitmask parent accessors (new)

```
NEW (add to kstrie_bitmask):

static uint64_t* get_parent(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t*>(
        static_cast<std::uintptr_t>(node[NODE_PARENT_PTR]));
}
static void set_parent(uint64_t* node, uint64_t* parent) noexcept {
    node[NODE_PARENT_PTR] = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(parent));
}
// parent_byte stored in header.slots_off (unused for bitmask)
static uint16_t get_parent_byte(const uint64_t* node) noexcept {
    return hdr_type::from_node(node).slots_off;
}
static void set_parent_byte(uint64_t* node, uint16_t byte) noexcept {
    hdr_type::from_node(node).slots_off = byte;
}
```

### 3.17 kstrie_compact.hpp — sentinel: 3 → 4 u64s

```
OLD:
static inline constinit uint64_t sentinel_data_[3] = {};

NEW:
static inline constinit uint64_t sentinel_data_[4] = {};
// Bumped: COMPACT_ARRAYS_OFF=24 means code may read node[2] (parent_ptr)
// or peek at node[3] (start of L array) on sentinel.
```

### 3.18 kstrie_compact.hpp — ck_offsets_off comment update

```
OLD (line ~133 comment):
// cap = align_up(count, 8). With COMPACT_ARRAYS_OFF = 16:

NEW:
// cap = align_up(count, 8). With COMPACT_ARRAYS_OFF = 24:
```

### 3.17 kstrie_compact.hpp — index_size comment (line ~190)

```
OLD:
return static_cast<size_t>(h.slots_off) * U64_BYTES - hdr_type::COMPACT_ARRAYS_OFF;

No code change — COMPACT_ARRAYS_OFF constant updated, arithmetic adjusts.
```

## 4 Parent Maintenance (kstrie_impl.hpp)

### 4.1 link_child helper (new)

```
NEW (add to kstrie_impl):

// Set child's parent pointer and dispatch byte after storing in parent's child array.
void link_compact_child(uint64_t* parent, uint64_t* child, uint16_t byte) {
    hdr_type ch = hdr_type::from_node(child);
    if (ch.is_compact()) {
        compact_type::set_parent(child, parent);
        compact_type::set_parent_byte(child, ch, byte);
    } else {
        bitmask_type::set_parent(child, parent);
        bitmask_type::set_parent_byte(child, byte);
    }
}

void mark_root(uint64_t* node) {
    if (!node || node == compact_type::sentinel()) return;
    hdr_type h = hdr_type::from_node(node);
    if (h.is_compact()) {
        compact_type::set_parent(node, nullptr);
        compact_type::set_parent_byte(node, h, ROOT_PARENT_BYTE);
    } else {
        bitmask_type::set_parent(node, nullptr);
        bitmask_type::set_parent_byte(node, ROOT_PARENT_BYTE);
    }
}
```

### 4.2 Wiring into mutation paths

Every place in kstrie_impl that:
- **Creates a new child** (insert_impl bitmask branch) → link_child
- **Replaces a child** (bitmask_type::replace_child) → link_child
- **Creates a new bitmask** (split_node, rebuild_with_new) → link all children
- **Reskips** → parent copied in 3.13; parent's child ptr updated at caller
- **Coalesces** → new compact linked to old bitmask's parent
- **Sets root** → mark_root

### 4.3 clone_tree_into: copy parent structure

After cloning, parent pointers point into the OLD tree. Re-link:
walk the cloned tree, set each child's parent to its new parent node.
This is a one-time recursive pass after clone.

## 5 Iterator Class (kstrie.hpp)

### 5.1 Representation

```cpp
class iterator {
    friend class kstrie;
    uint64_t*   leaf_v  = nullptr;   // compact node (null = end)
    uint16_t    pos_v   = 0;         // slot index
    void*       val_v   = nullptr;   // live value ptr (or impl* for end)
    std::string key_v;               // cached full key

    // End sentinel: leaf_v=nullptr, val_v=impl*
    explicit iterator(impl_t* impl) : val_v(static_cast<void*>(impl)) {}

public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type        = std::pair<const std::string, VALUE>;
    using difference_type   = std::ptrdiff_t;
    using reference         = std::pair<const std::string&, mapped_ref>;

    struct arrow_proxy {
        reference p;
        auto* operator->() noexcept { return &p; }
    };
    using pointer = arrow_proxy;

    reference operator*() const noexcept;
    arrow_proxy operator->() const noexcept { return {**this}; }

    iterator& operator++();   // within-leaf or parent walk
    iterator& operator--();   // within-leaf or parent walk (end→last)
    // post-increment/decrement
    bool operator==(const iterator&) const noexcept;
    bool operator!=(const iterator&) const noexcept;
};
```

### 5.2 Key reconstruction

**find/lower_bound:** Key provided by caller, store in key_v.

**operator++ within leaf:** Truncate to prefix_len, append new suffix:
```
key_v.resize(prefix_len);
if (L[pos] > 0) { key_v += unmap(F[pos]); key_v.append(unmap(ks+O[pos]), L[pos]-1); }
```

**operator++ across leaves:** Parent walk collects divergence point.
Truncate key_v to that depth, append new dispatch byte + skip bytes +
edge entry suffix.

**operator-- on end():** Read impl* from val_v, edge_entry(BWD),
full key reconstruction from parent chain.

### 5.3 Walk from leaf

```
walk_from_leaf(leaf, dir):
    hdr_type lh = hdr_type::from_node(leaf);
    uint64_t* parent = compact_type::get_parent(leaf);
    uint16_t byte = compact_type::get_parent_byte(leaf, lh);
    loop:
        if byte == ROOT_PARENT_BYTE → return end
        hdr_type ph = hdr_type::from_node(parent);
        const bitmap* bm = bitmask_type::get_bitmap(parent, ph);
        if dir == FWD:
            sib = bm->find_next_set(byte + 1)
            if sib >= 0:
                descend to first entry of child at sib
                return entry
            // no more byte children; go up
        else: // BWD
            sib = bm->find_prev_set(byte - 1)
            if sib >= 0:
                descend to last entry of child at sib
                return entry
            // check EOS (sorts before all bytes)
            if bitmask_type::has_eos(parent, ph):
                descend to last entry of EOS child
                return entry
        byte = bitmask_type::get_parent_byte(parent)
        parent = bitmask_type::get_parent(parent)
```

Note: EOS ordering. For FWD, EOS was already visited before byte
children (edge_entry handles this). For BWD past byte 0, check EOS.

### 5.4 Edge entry (min/max of subtree)

```
edge_entry(node, dir):
    while is_bitmap(node):
        if dir == FWD:
            if has_eos(node) → node = eos_child
            else → node = first byte child
        else:
            → node = last byte child
            // (if no byte children, EOS is the only child)
        // append dispatch byte + skip to key_v
    // compact leaf
    pos = (dir == FWD) ? 0 : count - 1
    // append L[pos]/F[pos]/keysuffix to key_v
```

## 6 Value Proxies

```cpp
using mapped_ref       = std::conditional_t<IS_BITMAP, bool_ref, VALUE&>;
using const_mapped_ref = std::conditional_t<IS_BITMAP, bool, const VALUE&>;
```

IS_BOOL: existing `load_value` returns `&BOOL_VALUES[bit]` — a static,
not writable. Replace with `bool_ref{word_ptr, bit_idx}` for mutable
access. This affects `operator*` and `at()`.

IS_INLINE: `load_value` returns `reinterpret_cast<VALUE*>(&base[index])` —
live mutable pointer. Works as `VALUE&`.

Heap (C-type): `load_value` returns heap pointer. `VALUE&` via deref.

## 7 Public API

Full std::map minus node handles. Remove `modify`, `erase_when`,
`assign`, snapshot `find_value`. Add mutable `find()` returning
iterator, `at()`, `operator[]`, `insert()` returning `pair<iterator,bool>`.

## 8 EOS Ordering

- `edge(FWD)` on bitmask: EOS first, then byte children
- `edge(BWD)` on bitmask: last byte child, then EOS (only if no bytes)
- Parent walk FWD from last byte child: up (EOS already visited)
- Parent walk BWD from before first byte child: check EOS

## 9 Scope — What Changes, What Doesn't

### Changes

- **kstrie_support.hpp:** NODE_PARENT_PTR(1), NODE_TOTAL_TAIL(1→2),
  COMPACT_ARRAYS_OFF(16→24), node_size bitmask(2→3), ROOT_PARENT_BYTE,
  EOS_PARENT_BYTE
- **kstrie_compact.hpp:** ck_prefix.reserved_→parent_byte,
  COMPACT_PARENT_PTR, parent accessors, comment updates
- **kstrie_bitmask.hpp:** CHILD_BITMAP_OFF(2→3), BITMAP_BYTE_OFF(derived),
  needed_u64(2→3), parent_ptr init in create/create_with_children/reskip/
  insert_child realloc, parent accessors
- **kstrie_impl.hpp:** link_child/mark_root helpers, parent wiring in
  mutation paths, clone_tree_into re-link
- **kstrie.hpp:** New iterator class, new public API, remove snapshot code

### No changes

- Compact node alloc sizing / keysuffix sharing / chain logic
- Binary search on F / insert / erase / grow / shrink / split / coalesce
- Bitmask dispatch (branchless popcount)
- Value storage categories (bitmap/inline/heap)
- Sentinel design (static **4**-u64 zeros — bumped from 3 for COMPACT_ARRAYS_OFF=24)
- Character map system

## 10 Implementation Order

1. ✅ kstrie_support.hpp: NODE_PARENT_PTR, NODE_TOTAL_TAIL shift, COMPACT_ARRAYS_OFF,
   node_size, ROOT_PARENT_BYTE, EOS_PARENT_BYTE, dir_t enum
2. ✅ kstrie_compact.hpp: ck_prefix.parent_byte, sentinel[4], COMPACT_PARENT_PTR,
   parent accessors, init parent_byte in alloc_compact_ks, comment updates
3. ✅ kstrie_bitmask.hpp: CHILD_BITMAP_OFF, BITMAP_BYTE_OFF, needed_u64, parent_ptr
   init in create/create_with_children/reskip/insert_child, parent accessors,
   link_child_to_parent, link_all_children, parent linking wired into
   replace_child, set_eos_child, insert_child (both paths), reskip,
   build_node_from_entries
4. ✅ Compile check + ASAN — existing tests pass after layout shift
5. ✅ kstrie_impl.hpp: set_root() helper with ROOT_PARENT_BYTE marking.
   All 11 non-sentinel `root_v = X` replaced with `set_root(X)`.
   replace_child and set_eos_child auto-link children (done in step 3).
   Added get_root_mut(), find_for_iter() returning leaf+pos+prefix_len.
6. ✅ kstrie_impl.hpp: clone_tree_into and clone_tree — link_all_children
   after bitmask clone. Copy constructor uses set_root.
7. ✅ kstrie.hpp: Live const_iterator with key_v, parent-walk operator++/--,
   edge_entry (min/max descent), walk_from_leaf, rebuild_suffix.
   Incremental prefix tracking: within-leaf swaps only suffix portion.
   Cross-leaf truncates key_v at divergence point, rebuilds only from
   there down — O(skip+suffix) for the common sibling-in-same-parent case.
   find() uses find_for_iter (single walk).
   lower_bound uses find_ge_iter (single walk).
   upper_bound uses lower_bound + advance (single walk + 1 step).
   prefix() still uses double-walk (snapshot bounds + find_for_iter) — future.
   All 10 tests pass + ASAN clean.
8. ✅ Public API + insert_result leaf+pos propagation
   - insert_result extended with leaf + pos fields (kstrie_support.hpp)
   - compact::insert/modify_or_insert: FOUND/UPDATED/normal-INSERT set
     leaf+pos; split/rebuild leave leaf=nullptr for caller re-find
   - compact::finalize takes pos, propagates on no-split
   - find_leaf_pos static helper (reusable for subtree re-find)
   - insert_node + modify_or_insert_node: all recursive returns propagate
     r.leaf/r.pos through EOS and child branches
   - insert_for_iter: calls insert_node, re-finds via find_leaf_pos
     when leaf==nullptr (split/rebuild/promote paths). Made public.
   - Section reorder: Modifiers + Element access moved below iterator class
   - erase(iterator) UAF fixed: save next key string, erase, re-find
   - Public API complete: insert()→pair, insert_or_assign()→pair,
     emplace, try_emplace, erase(key/iter/range), at(), operator[],
     clear(). Old snapshot-based modifiers removed.
   - operator[] returns mapped_ref (VALUE& or bool_ref)
   - 11 API tests + 10 iterator tests, all ASAN clean.
9. ✅ IS_BOOL bool_ref proxy
   - bool_ref struct: word pointer + bit index, operator bool(), operator=
   - mapped_ref = conditional_t<IS_BITMAP, bool_ref, VALUE&>
   - const_mapped_ref = conditional_t<IS_BITMAP, bool, const VALUE&>
   - operator[] uses bool_ref for bool specialization
   - Bool insert + operator[] tests pass ASAN clean
10. ⬜ Remove modify, erase_when, assign, find_value, iter_next/prev/min/max
11. ⬜ Test: sequential, shuffled, iteration order, reverse, copy, EOS keys
