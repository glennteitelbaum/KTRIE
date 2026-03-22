# kntrie_live.md — Live Iterator Conversion (Code-Level)

Convert kntrie from snapshot iterators to live iterators with parent
pointers. Remove dup-slot machinery. Add bool proxy. Result is a
drop-in std::map replacement.

---

## 1 kntrie_support.hpp

### 1.1 Remove — DONE

```
DUP_SCAN_MAX                    (line 78)
```

### 1.2 node_header_t — replace `total_slots_v` with `parent_byte_v` — DONE

Old:
```cpp
struct node_header_t {
    uint16_t depth_v       = 0;
    uint16_t entries_v     = 0;
    uint16_t alloc_u64_v   = 0;
    uint16_t total_slots_v = 0;   // <- remove
    ...
    unsigned total_slots() const noexcept { return total_slots_v; }      // <- remove
    void set_total_slots(unsigned n) noexcept { ... }                    // <- remove
};
```

New:
```cpp
struct node_header_t {
    uint16_t depth_v       = 0;
    uint16_t entries_v     = 0;
    uint16_t alloc_u64_v   = 0;
    uint16_t parent_byte_v = 0;   // dispatch byte in parent (256 = ROOT)
    ...
    static constexpr uint16_t ROOT_BYTE = 256;
    uint16_t parent_byte() const noexcept { return parent_byte_v; }
    void set_parent_byte(uint16_t b) noexcept { parent_byte_v = b; }
    bool is_root() const noexcept { return parent_byte_v == ROOT_BYTE; }
};
```

### 1.3 Leaf layout — add LEAF_PARENT_PTR, bump LEAF_HEADER_U64 — DONE

Old:
```cpp
inline constexpr size_t LEAF_FIND_FN    = 1;
inline constexpr size_t LEAF_NEXT_FN    = 2;
inline constexpr size_t LEAF_PREV_FN    = 3;
inline constexpr size_t LEAF_FIRST_FN   = 4;
inline constexpr size_t LEAF_LAST_FN    = 5;
inline constexpr size_t LEAF_PREFIX     = 6;
inline constexpr size_t LEAF_HEADER_U64 = 7;
```

New:
```cpp
inline constexpr size_t LEAF_FIND_FN    = 1;
inline constexpr size_t LEAF_NEXT_FN    = 2;
inline constexpr size_t LEAF_PREV_FN    = 3;
inline constexpr size_t LEAF_FIRST_FN   = 4;
inline constexpr size_t LEAF_LAST_FN    = 5;
inline constexpr size_t LEAF_PREFIX     = 6;
inline constexpr size_t LEAF_PARENT_PTR = 7;    // <- new
inline constexpr size_t LEAF_HEADER_U64 = 8;    // <- was 7
```

Add accessors:
```cpp
inline uint64_t* leaf_parent(uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t*>(node[LEAF_PARENT_PTR]);
}
inline void set_leaf_parent(uint64_t* node, uint64_t* parent) noexcept {
    node[LEAF_PARENT_PTR] = reinterpret_cast<uint64_t>(parent);
}
```

### 1.4 bool_slots — SKIPPED (ptr/ptr_at still used by val_ptr_at and bitmask return paths until full iterator switchover)

Remove from `bool_slots`:
```
TRUE_VAL, FALSE_VAL      -- static constexpr bools (snapshot pointer targets)
ptr()                    -- returns &TRUE_VAL or &FALSE_VAL
ptr_at()                 -- returns ptr(get(i))
```

Keep in `bool_slots` (still needed for packed bit insert/erase):
```
get(), set()             -- bit read/write
shift_left_1()           -- bit-level memmove left (for erase)
shift_right_1()          -- bit-level memmove right (for insert)
clear_all()
unpack_to(), pack_from()
bytes_for(), u64_for()
```

`bool_ref` (§1.5) is added alongside `bool_slots`, not replacing it.
`bool_slots` manages the packed storage; `bool_ref` provides the proxy
reference that the live iterator returns.

### 1.5 Add bool_ref proxy — SKIPPED (depends on §1.4)

```cpp
struct bool_ref {
    uint64_t* word;
    uint8_t   bit;

    operator bool() const noexcept {
        return (*word >> bit) & 1;
    }
    bool_ref& operator=(bool v) noexcept {
        if (v) *word |=  (uint64_t{1} << bit);
        else   *word &= ~(uint64_t{1} << bit);
        return *this;
    }
};
```

### 1.6 value_traits — SKIPPED (depends on §1.4)

Old:
```cpp
static const VALUE* as_ptr(const slot_type& s) noexcept {
    if constexpr (IS_BOOL)        return bool_slots::ptr(s);  // <- remove
    else if constexpr (IS_TRIVIAL) return reinterpret_cast<const VALUE*>(&s);
    else                           return s;
}
```

New:
```cpp
static const VALUE* as_ptr(const slot_type& s) noexcept {
    if constexpr (IS_TRIVIAL) return reinterpret_cast<const VALUE*>(&s);
    else                      return s;
}
```

### 1.7 Snapshot iteration types — SKIPPED (iter_fn_t still used by find_next/prev)

Remove:
```
leaf_result_t<VALUE>    (lines 305-310) -- snapshot find result with key+value copy
iter_fn_t<VALUE>        -- function pointer type for find_next/find_prev
get_find_next<VALUE>()  -- accessor for node[LEAF_NEXT_FN]
set_find_next<VALUE>()
get_find_prev<VALUE>()
set_find_prev<VALUE>()
```

Keep:
```
find_fn_t<VALUE>        -- still used by find_loop
get_find_fn<VALUE>() / set_find_fn<VALUE>()
get_find_first<VALUE>() / set_find_first<VALUE>()
get_find_last<VALUE>() / set_find_last<VALUE>()
```

Change return type of find_first/find_last from `leaf_result_t<VALUE>`
to `leaf_pos_t`:

```cpp
struct leaf_pos_t {
    uint64_t* node;
    uint16_t  pos;
    bool      found;
};
using first_fn_t = leaf_pos_t (*)(uint64_t*) noexcept;
using last_fn_t  = leaf_pos_t (*)(uint64_t*) noexcept;
```

---

## 2 kntrie_compact.hpp

### 2.1 adaptive_search — DONE — replace with adaptive version

Old (power-of-two only):
```cpp
static const K* find_base(const K* base, unsigned count, K key) noexcept {
    do {
        count >>= 1;
        base += (base[count] <= key) ? count : 0;
    } while (count > 1);
    return base;
}
static const K* find_base_first(const K* base, unsigned count, K key) noexcept {
    do {
        count >>= 1;
        base += (base[count] < key) ? count : 0;
    } while (count > 1);
    return base;
}
```

New (any count). **Do not condense this code.** The separate variables
and `=` assignments (not `+=`) are deliberate. Eliminating temporaries
or switching to `+=` produces worse codegen:

```cpp
static const K* find_base(const K* base, unsigned count, K key) noexcept {
    int bw=std::bit_width(count);
    unsigned count2=1 << (bw-1);
    unsigned diff=count - count2;
    const K* diff_val=base+diff;
    bool is_diff = key > *diff_val;
    base = is_diff ? diff_val : base;
    count=count2;
    do {
        count >>= 1;
        const K* hi_val=base+count;
        bool is_hi=(*hi_val <= key);
        base = is_hi ? hi_val : base;
    } while (count > 1);
    return base;
}

static const K* find_base_first(const K* base, unsigned count, K key) noexcept {
    int bw=std::bit_width(count);
    unsigned count2=1 << (bw-1);
    unsigned diff=count - count2;
    const K* diff_val=base+diff;
    bool is_diff = key > *diff_val;
    base = is_diff ? diff_val : base;
    count=count2;
    do {
        count >>= 1;
        const K* hi_val=base+count;
        bool is_hi=(*hi_val < key);
        base = is_hi ? hi_val : base;
    } while (count > 1);
    return base;
}
```

Precondition: `count > 0`.

### 2.2 Remove dup machinery — DONE

```
find_dup_pos()           (lines 667-701)
insert_consume_dup()     (lines 703-727)
erase_create_dup()       (lines 729-754)
seed_from_real()         (lines 962-994)
seed_with_insert()       (lines 517-660)
dedup_skip_into()        (lines 482-515)
slots_for()              (lines 72-75)
```

### 2.3 make_leaf — DONE — exact count, no dup seeding

Old:
```cpp
uint16_t ts = slots_for(count);
h->set_total_slots(ts);
seed_from_real(keys(node, hu), ..., count, ts);
```

New:
```cpp
size_t au64 = round_up_u64(size_u64(count, LEAF_HEADER_U64));
h->set_entries(count);
h->set_alloc_u64(au64);
std::memcpy(keys(node, LEAF_HEADER_U64), sorted_keys, count * sizeof(K));
// copy values directly, no seeding
```

### 2.4 insert — DONE — memmove instead of consume-dup

Old:
```cpp
unsigned dups = ts - entries;
if (dups > 0) [[likely]] {
    insert_consume_dup(kd, vd, ts, ins, entries, suffix, value);
    ...
}
// No dups: realloc to next slot count
uint16_t new_ts = slots_for(new_entries);
```

New:
```cpp
unsigned cap = capacity_from_alloc(h->alloc_u64());
if (entries >= cap) {
    // grow: alloc new, memcpy, free old
}
int tail = entries - ins;
if (tail > 0) {
    std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
    std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
}
kd[ins] = suffix;
VT::write_slot(&vd[ins], value);
h->set_entries(entries + 1);
```

### 2.5 erase — DONE — memmove instead of create-dup

Old:
```cpp
erase_create_dup(kd, vd, ts, idx, suffix, bld);
```

New:
```cpp
int tail = entries - idx - 1;
if (tail > 0) {
    std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
    std::memmove(vd + idx, vd + idx + 1, tail * sizeof(VST));
}
h->set_entries(entries - 1);
```

### 2.6 ASSIGN path — DONE — single write, no dup propagation

Old:
```cpp
const K* fb = adaptive_search<K>::find_base_first(kd, ts, suffix);
int first = ...;
for (int i = first; i <= idx; ++i)
    VT::write_slot(&vd[i], value);
```

New:
```cpp
bld.destroy_value(vd[idx]);
VT::init_slot(&vd[idx], value);
```

### 2.7 All `total_slots()` → `entries()` — DONE

Every place that reads `h->total_slots()` or `ts` for search, iteration,
or sizing changes to `h->entries()`.

### 2.8 set_leaf_fns — DONE (DO_SCAN removed, tables 4→2, all 5 fn ptrs kept)

Old:
```cpp
static inline const iter_fn_t<VALUE> NEXT_TABLE[4] = { ... };
static inline const iter_fn_t<VALUE> PREV_TABLE[4] = { ... };

static void set_leaf_fns(uint64_t* node, unsigned ts, bool has_skip) noexcept {
    bool do_scan = (ts <= SCAN_MAX);
    unsigned idx = (has_skip ? 2u : 0u) + (do_scan ? 1u : 0u);
    set_find_fn<VALUE>(node, FIND_TABLE[idx]);
    set_find_next<VALUE>(node, NEXT_TABLE[idx]);
    set_find_prev<VALUE>(node, PREV_TABLE[idx]);
    set_find_first<VALUE>(node, &find_first_fn);
    set_find_last<VALUE>(node, &find_last_fn);
}
```

New:
```cpp
static inline const find_fn_t<VALUE> FIND_TABLE[2] = {
    &find_fn<false>,  &find_fn<true>,
};
static void set_leaf_fns(uint64_t* node, bool has_skip) noexcept {
    unsigned idx = has_skip ? 1u : 0u;
    set_find_fn<VALUE>(node, FIND_TABLE[idx]);
    set_find_first<VALUE>(node, has_skip ? &find_first_fn<true> : &find_first_fn<false>);
    set_find_last<VALUE>(node, has_skip ? &find_last_fn<true> : &find_last_fn<false>);
}
```

DO_SCAN template parameter removed — adaptive search handles any count.

### 2.9 Remove functions — DONE (erase_when, modify_existing, val_ptr_first/last removed; find_next/prev KEPT for narrowing)

```
find_next_fn<DO_SKIP, DO_SCAN>()
find_prev_fn<DO_SKIP, DO_SCAN>()
modify_existing()
val_ptr_at(), val_ptr_first(), val_ptr_last()
```

### 2.10 find_first_fn, find_last_fn — DONE (use entries, inline val_ptr_at)

Old returns `leaf_result_t<VALUE>` (copied key + value pointer).
New returns `leaf_pos_t`:

```cpp
template<bool DO_SKIP>
static leaf_pos_t find_first_fn(uint64_t* node) noexcept {
    return {node, 0, get_header(node)->entries() > 0};
}
template<bool DO_SKIP>
static leaf_pos_t find_last_fn(uint64_t* node) noexcept {
    unsigned e = get_header(node)->entries();
    return {node, static_cast<uint16_t>(e > 0 ? e - 1 : 0), e > 0};
}
```

### 2.11 destroy_and_dealloc — DONE

Also done: `for_each` simplified (no dup skipping, iterates entries linearly). — simplify

Old: iterates keys looking for non-dup entries to destroy.
New: every slot is unique, destroy all `entries` values linearly.

---

## 3 kntrie_bitmask.hpp

### 3.1 Add parent pointer to bitmask node layout — DONE

Old: `[header(1)][bitmap(4)][sentinel(1)][children(n)][desc(1)]`

New: `[header(1)][parent_ptr(1)][bitmap(4)][sentinel(1)][children(n)][desc(1)]`

All child offsets bump by 1:
```cpp
inline constexpr size_t BM_PARENT_U64    = 1;
inline constexpr size_t BM_CHILDREN_START = BM_PARENT_U64 + BITMAP_WORDS + BM_SENTINEL_U64;  // 6, was 5
```

### 3.2 Remove — DONE (iter_next_loop, iter_prev_loop removed from bitmask)

```
iter_next_loop()         (lines 1195-1227)
iter_prev_loop()         (lines 1229-1261)
```

These built a stack-local `path[]` during descent then walked back.
Parent pointers replace this.

### 3.3 Keep (used by live iterator)

```
find_loop()              -- branchless descent for point lookup
descend_first_loop()     -- return type changes to leaf_pos_t
descend_last_loop()      -- return type changes to leaf_pos_t
bm_next_sibling()        -- used by live iterator walk-back
bm_prev_sibling()        -- used by live iterator walk-back
bm_first_child()         -- used by descend loops
bm_last_child()          -- used by descend loops
```

### 3.4 Parent maintenance

Every place that stores a child pointer into a bitmask node must also
set the child's parent pointer and dispatch byte. Apply in:
- `add_child` / `insert_child`
- Realloc paths (children copied to new node, update children's parents)
- `do_coalesce` / `coalesce` (children reassigned)

---

## 4 kntrie_ops.hpp

### 4.1 Remove entirely — DONE (modify_node, modify_or_insert_node, leaf_modify, leaf_modify_or_insert, erase_when_node, leaf_erase_when)

```
modify_node()                (lines 666-722)
modify_or_insert_node()      (lines 727-826)
leaf_modify()                (lines 827-842)
leaf_modify_or_insert()      (lines 844-875)
erase_when_node()            (lines 1019-1075)
leaf_erase_when()            (lines 1139-1160)
```

### 4.2 Leaf construction — PARTIAL (set_leaf_fns calls fixed; parent pointer setting at alloc sites still needed)

All leaf allocation sites set `node[LEAF_PARENT_PTR]` and
`h->set_parent_byte()`.

---

## 5 kntrie_impl.hpp

### 5.1 Remove — DONE (iter_result_t, to_iter_result, iter_first/last/next/prev, impl_insert_result_t, insert_ex, insert_dispatch_ex, erase_when, modify_existing, modify_or_insert)

```
struct iter_result_t             (line 406)
to_iter_result()                 (lines 408-412)
iter_first()                     (lines 414-418)
iter_last()                      (lines 421-425)
iter_next()                      (lines 428-444)
iter_prev()                      (lines 447-463)
modify_existing()                (lines 572-582)
modify_or_insert()               (lines 584-625)
erase_when()                     (lines 321-342)
insert_ex()                      (line 275-276)
insert_dispatch_ex()             (lines 518-563)
struct impl_insert_result_t      (lines 267-270)
```

### 5.2 Add — live position API — DONE (leaf_pos_t, insert_pos_result_t, descend helpers, bitmap_find_pos, compact find_pos, find_pos_for_leaf, reconstruct_ik, first/last/next/prev_pos, find_with_pos, insert_with_pos, upsert_with_pos, erase_at)

```cpp
leaf_pos_t first_pos() const noexcept;
leaf_pos_t last_pos() const noexcept;
leaf_pos_t next_pos(uint64_t* leaf, uint16_t pos) const noexcept;
leaf_pos_t prev_pos(uint64_t* leaf, uint16_t pos) const noexcept;

// Find: returns {leaf, pos, found}. On miss, found=false.
leaf_pos_t find_with_pos(const KEY& key) const noexcept;

struct insert_pos_result_t {
    uint64_t* leaf;
    uint16_t  pos;
    bool      inserted;
};

// Insert-if-absent: returns leaf+pos whether inserted or found existing.
insert_pos_result_t insert_with_pos(const KEY& key, const VALUE& value);

// Insert-or-assign: inserts if absent, overwrites if present.
// Returns {leaf, pos, was_new_insert}.
insert_pos_result_t upsert_with_pos(const KEY& key, const VALUE& value);

// Erase by position: removes entry at leaf[pos], memmoves tail left.
// May trigger shrink. Caller must have advanced the iterator first.
void erase_at(uint64_t* leaf, uint16_t pos);
```

`next_pos` hot path: `pos+1 < entries -> {leaf, pos+1}`.
Cold path: walk parent chain via `bm_next_sibling`, descend via
`descend_first_loop`.

---

## 6 kntrie.hpp

### 6.1 Remove — DONE (snapshot const_iterator, value_proxy, modify, erase_when, at absence comment)

```
class const_iterator     (lines 48-117) -- entire snapshot iterator
class value_proxy        (lines 284-300) -- routed writes through insert_or_assign
modify(key, fn)          (lines 197-201)
modify(key, fn, default) (lines 207-211)
erase_when(key, fn)      (lines 261-265)
at() absence comment     (lines 274-278)
```

### 6.2 New live iterator class — DONE (live iterator with leaf+pos, arrow_proxy, bitmap/compact aware, key_at_pos/value_ref_at_pos added to impl)

```cpp
class iterator {
    friend class kntrie;
    impl_t*   impl_v = nullptr;
    uint64_t* leaf_v = nullptr;
    uint16_t  pos_v  = 0;

    iterator(impl_t* p, uint64_t* leaf, uint16_t pos)
        : impl_v(p), leaf_v(leaf), pos_v(pos) {}

public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type        = std::pair<const KEY, VALUE>;
    using difference_type   = std::ptrdiff_t;
    using reference         = std::pair<const KEY, VALUE&>;

    struct arrow_proxy {
        std::pair<const KEY, VALUE&> p;
        auto* operator->() noexcept { return &p; }
    };
    using pointer = arrow_proxy;

    iterator() = default;

    reference operator*() const noexcept;    // reads key/value& from leaf
    arrow_proxy operator->() const noexcept  // it->second = 5 works
        { return {**this}; }
    iterator& operator++();                  // next_pos
    iterator  operator++(int);
    iterator& operator--();                  // prev_pos, or last_pos for end()--
    iterator  operator--(int);
    bool operator==(const iterator& o) const noexcept;
    bool operator!=(const iterator& o) const noexcept;
};
```

The arrow proxy is a temporary holding `pair<const KEY, VALUE&>`.
`it->second = 5` works because `.second` is a reference into the
leaf's value array — the write goes straight through. The proxy
dies at the semicolon; the reference was already consumed.

Bool specialization: `operator*` returns `pair<const KEY, bool_ref>`
instead of `pair<const KEY, VALUE&>`. The arrow proxy works the same
way — `bool_ref::operator=` writes through to the packed u64.

### 6.3 Full std::map interface — PARTIAL (all methods written; lower_bound/upper_bound use duplicate pos functions that need replacing — §7.2 fn-ptr signature change to leaf_pos_t is the blocker)

Below is the complete public API. Items marked **new** don't exist
today. Items marked **rework** exist but change signature or
implementation. Items marked **keep** are unchanged.

#### Element access

```cpp
// NEW — was absent due to dup corruption risk; no dups now
VALUE& at(const KEY& key) {
    auto r = impl_.find_with_pos(to_unsigned(key));
    if (!r.found) throw std::out_of_range("kntrie::at");
    return vals_at(r.leaf)[r.pos];
}
const VALUE& at(const KEY& key) const {
    auto r = impl_.find_with_pos(to_unsigned(key));
    if (!r.found) throw std::out_of_range("kntrie::at");
    return vals_at(r.leaf)[r.pos];
}

// REWORK — was value_proxy; now returns VALUE& directly
VALUE& operator[](const KEY& key) {
    auto r = impl_.insert_with_pos(to_unsigned(key), VALUE{});
    return vals_at(r.leaf)[r.pos];
}
```

#### Modifiers — insert family

```cpp
// REWORK — returns live iterator instead of snapshot
std::pair<iterator, bool> insert(const value_type& kv) {
    auto r = impl_.insert_with_pos(to_unsigned(kv.first), kv.second);
    return {iterator(&impl_, r.leaf, r.pos), r.inserted};
}

// REWORK — same
std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
    auto r = impl_.insert_with_pos(to_unsigned(key), value);
    return {iterator(&impl_, r.leaf, r.pos), r.inserted};
}

// NEW — move variant
std::pair<iterator, bool> insert(value_type&& kv) {
    auto r = impl_.insert_with_pos(to_unsigned(kv.first), std::move(kv.second));
    return {iterator(&impl_, r.leaf, r.pos), r.inserted};
}

// KEEP — hint ignored, delegates to insert
iterator insert(const_iterator, const value_type& kv) {
    return insert(kv).first;
}

// KEEP — range insert
template<typename InputIt>
void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) insert(*first);
}

// NEW — initializer list
void insert(std::initializer_list<value_type> il) {
    for (auto& kv : il) insert(kv);
}

// REWORK — returns pair<iterator, bool> per std::map contract
//   (currently returns pair<bool, bool>)
std::pair<iterator, bool> insert_or_assign(const KEY& key, const VALUE& value) {
    auto r = impl_.upsert_with_pos(to_unsigned(key), value);
    return {iterator(&impl_, r.leaf, r.pos), r.inserted};
}

// REWORK — uses insert_with_pos instead of insert_ex
template<typename... Args>
std::pair<iterator, bool> emplace(Args&&... args) {
    value_type kv(std::forward<Args>(args)...);
    return insert(std::move(kv));
}

// REWORK — single walk via insert_with_pos (INSERT=true, ASSIGN=false)
template<typename... Args>
std::pair<iterator, bool> try_emplace(const KEY& key, Args&&... args) {
    VALUE v(std::forward<Args>(args)...);
    auto r = impl_.insert_with_pos(to_unsigned(key), std::move(v));
    return {iterator(&impl_, r.leaf, r.pos), r.inserted};
}
```

#### Modifiers — erase

```cpp
// KEEP
size_type erase(const KEY& key) {
    return impl_.erase(to_unsigned(key)) ? 1 : 0;
}

// REWORK — direct, no re-descent by key
iterator erase(iterator pos) {
    auto next = pos; ++next;
    impl_.erase_at(pos.leaf_v, pos.pos_v);
    return next;
}

// REWORK
iterator erase(iterator first, iterator last) {
    while (first != last) first = erase(first);
    return last;
}

// KEEP
void clear() noexcept { impl_.clear(); }
void swap(kntrie& o) noexcept { impl_.swap(o.impl_); }
```

#### Lookup

```cpp
// REWORK — returns live iterator with {leaf, pos}
iterator find(const KEY& key) {
    auto r = impl_.find_with_pos(to_unsigned(key));
    if (!r.found) return end();
    return iterator(&impl_, r.leaf, r.pos);
}
const_iterator find(const KEY& key) const { ... }

// KEEP
bool contains(const KEY& key) const noexcept;
size_type count(const KEY& key) const noexcept;

// REWORK — returns live iterators
iterator lower_bound(const KEY& key);
iterator upper_bound(const KEY& key);
std::pair<iterator, iterator> equal_range(const KEY& key);
// const variants too
```

#### Observers

```cpp
// NEW — returns the comparator that defines key ordering
//   (unsigned byte order via XOR sign-bit flip)
struct key_compare {
    bool operator()(const KEY& a, const KEY& b) const noexcept {
        return to_unsigned(a) < to_unsigned(b);
    }
};
key_compare key_comp() const noexcept { return {}; }

// NEW — compares pairs by key
struct value_compare {
    bool operator()(const value_type& a, const value_type& b) const noexcept {
        return to_unsigned(a.first) < to_unsigned(b.first);
    }
};
value_compare value_comp() const noexcept { return {}; }

// KEEP
allocator_type get_allocator() const noexcept;
```

#### Iterators

```cpp
// REWORK — all return live iterators
iterator begin() noexcept;
iterator end() noexcept;            // {nullptr, 0}
const_iterator cbegin() const noexcept;
const_iterator cend() const noexcept;
reverse_iterator rbegin() noexcept;
reverse_iterator rend() noexcept;
const_reverse_iterator crbegin() const noexcept;
const_reverse_iterator crend() const noexcept;
```

#### Intentionally omitted

```
extract()  -- node handle API; doesn't map to compact leaves
merge()    -- depends on extract
```

These are C++17 node-handle APIs. A compact leaf holds many entries
in one allocation — there's no single-entry node to extract. Could
be emulated with erase+insert but that defeats the purpose.

---

## 7 Design Decisions

### 7.1 Insert pos propagation

The entire insert chain must propagate `{leaf*, pos}` back to the
caller. Current return type:

```cpp
struct insert_result_t {
    uint64_t tagged_ptr;
    bool     inserted;
    bool     needs_split;
    const void* existing_value;  // for insert_ex
};
```

New return type:

```cpp
struct insert_result_t {
    uint64_t  tagged_ptr;
    uint64_t* leaf;       // compact leaf containing the entry
    uint16_t  pos;        // position within the leaf
    bool      inserted;
    bool      needs_split;
};
```

Propagation: `compact_ops::insert` knows `leaf` (it's `node`) and
`pos` (it's `ins` or the found position). It sets them in the result.
The recursive `insert_node` in kntrie_ops passes them through
unchanged — a bitmask insert delegates to a child and the child's
leaf/pos bubble up. On grow/realloc, `leaf` is the new node pointer
and `pos` is the insertion index in the new allocation. On split
(compact → bitmask), the entry lands in one of the new child compacts;
that child's leaf/pos propagate up.

### 7.2 All 5 fn-ptr signatures → leaf_pos_t — PARTIAL (support typedefs+accessors done, compact fns done, bitmask fns+loops done, impl descend refs fixed. Still needed: find_with_pos→find_loop, find_value→find_loop, lower/upper_bound_pos use fn ptrs at leaf, remove 4 dispatch fns from ops, fix bool_slots::ptr ref in bitmask, untemplated setters in ops set_leaf_fns_for)

All 5 leaf function pointers change to return `leaf_pos_t` instead
of their current types. This unifies narrowing dispatch and position
reporting — one set of functions, not two parallel sets.

Current signatures:
```cpp
using find_fn_t  = const VALUE* (*)(const uint64_t*, uint64_t) noexcept;
using iter_fn_t  = leaf_result_t<VALUE> (*)(const uint64_t*, uint64_t) noexcept;
using edge_fn_t  = leaf_result_t<VALUE> (*)(const uint64_t*) noexcept;
```

New: all return `leaf_pos_t`, take `uint64_t*` (mutable):
```cpp
using find_fn_t  = leaf_pos_t (*)(uint64_t*, uint64_t) noexcept;
using iter_fn_t  = leaf_pos_t (*)(uint64_t*, uint64_t) noexcept;
using edge_fn_t  = leaf_pos_t (*)(uint64_t*) noexcept;
```

This eliminates:
- `leaf_result_t<VALUE>` entirely
- `val_ptr_at` (callers read value via `value_ref_at` from pos)
- Duplicate `find_pos`/`find_next_pos`/`find_ge_pos`/`find_prev_pos`
  functions in compact, bitmask, and ops (the fn-ptr versions ARE
  the pos versions)
- `find_pos_for_leaf`, `find_next_pos_for_leaf`, `find_ge_pos_for_leaf`,
  `find_prev_pos_for_leaf` in ops (runtime dispatch eliminated — the
  fn ptr already narrows)

`find_loop` returns `leaf_pos_t`. `contains`/`count` test `.found`.
`find_with_pos` calls `find_loop` directly.
`lower_bound_pos`/`upper_bound_pos` call the fn-ptr `find_next`/
`find_ge` at the leaf — no separate dispatch layer.

### 7.3 `bm_ptr` convention

Adding `parent_ptr` at `node[1]` pushes bitmap to `node[2]`.

Current: bitmask pointers in parent child arrays point to `&node[1]`
(the bitmap start). `bm_to_node(ptr)` subtracts 1 u64.

New: pointers point to `&node[2]` (the bitmap start at new offset).
`bm_to_node(ptr)` subtracts 2 u64s. All BM_CHILDREN_START and related
offsets are relative to the bitmap pointer, so they don't change — only
the node↔ptr conversion functions change:

```cpp
// Old
inline uint64_t* bm_to_node(uint64_t ptr) {
    return reinterpret_cast<uint64_t*>(ptr) - HEADER_U64;  // -1
}

// New
static constexpr size_t BM_PRE_BITMAP = HEADER_U64 + 1;  // header + parent_ptr
inline uint64_t* bm_to_node(uint64_t ptr) {
    return reinterpret_cast<uint64_t*>(ptr) - BM_PRE_BITMAP;  // -2
}
```

Parent pointer accessor:
```cpp
inline uint64_t* bm_parent(uint64_t* node) {
    return reinterpret_cast<uint64_t*>(node[HEADER_U64]);  // node[1]
}
inline void set_bm_parent(uint64_t* node, uint64_t* parent) {
    node[HEADER_U64] = reinterpret_cast<uint64_t>(parent);
}
```

### 7.4 `erase_at` upward walk

Current erase descends by key recursively and propagates size/coalesce
on the unwind. `erase_at(leaf, pos)` starts at the bottom and walks
up:

```
1. Remove entry from compact leaf at pos (memmove left).
2. If leaf is empty, detach from parent bitmask.
3. Walk parent chain upward:
   a. Decrement descendant count at each bitmask level.
   b. Check coalesce: if total descendants <= COMPACT_MAX,
      collapse subtree to compact leaf.
   c. Check single-child: if one child remains, merge into child.
   d. If bitmask is now empty, detach from its parent.
4. If leaf count < capacity / SHRINK_FACTOR, shrink (realloc smaller).
   Update parent's child pointer to new leaf.
5. Decrement size_v.
```

The parent pointer chain provides the walk path. At each bitmask,
the `parent_byte` in the child's header tells which slot the child
occupies — no bitmap search needed to find the child's position.

### 7.5 Capacity and allocation

Compact leaves are no longer power-of-2 sized. They use the same
size-class allocator (`round_up_u64` / `BIN_SIZES`) that bitmask
nodes already use, with the 3/4 midpoints between powers of two.

Capacity is whatever count fits in the allocated u64s:
```cpp
if (size_u64(entries + 1, LEAF_HEADER_U64) > h->alloc_u64())
    grow();  // realloc to round_up_u64(size_u64(entries * GROW_NUMER / GROW_DENOM))
```

One comparison. No stored capacity field. No inversion. The grow
target uses the same `GROW_NUMER / GROW_DENOM` headroom as today,
and `round_up_u64` snaps it to the next size class.

### 7.6 Iterator invalidation

Same contract as `std::unordered_map`: any modification to the
container may invalidate iterators. Insert may realloc a leaf
(invalidating iterators into that leaf). Erase may shrink a leaf
(same). Document as:

> Iterators are invalidated by any operation that modifies the
> container. This matches `std::unordered_map` semantics. References
> and pointers to elements are also invalidated.

This is stricter than std::map (which guarantees insert doesn't
invalidate) but matches the flat-storage reality. Users who need
stable iterators across mutations should use `find()` after each
mutation.

---

## 8 Migration Path

1. Add parent pointer fields to compact leaf and bitmask node.
   Set parent on all construction/realloc paths. Existing snapshot
   iterator still works -- parent pointers are write-only at this stage.

2. Write live iterator class alongside existing const_iterator.
   Test with a parallel `live_begin()` / `live_end()` API. Verify
   advance/retreat match snapshot results for all test cases.

3. Remove dup machinery from compact leaf. Replace with plain
   memmove insert/erase. Switch to adaptive branchless search.
   All tests must still pass with snapshot iterators (dups are
   an internal optimization, not visible to the API).

4. Switch public API from snapshot to live iterator. Remove
   const_iterator, iter_result_t, insert_ex, value_proxy.
   Add operator[], at(), erase(iterator).

5. Add bool_ref proxy. Remove TRUE_VAL/FALSE_VAL/ptr/ptr_at from
   bool_slots. Remove as_ptr IS_BOOL branch. Keep packed bit storage
   and shift operations.

Steps 1-3 are independently testable. Step 4 is the API break.
Step 5 is independent of 1-4.
