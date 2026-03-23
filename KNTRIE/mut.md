# Mutation Path Fixes — mut.md

All changes are cut-and-paste ready. OLD = remove, NEW = insert at same location.

---

## 1+4. Value offset in alloc_u64 (HOT PATH)

For compact leaves, `alloc_u64` = value offset (u64s from node base).
Hot read: `vals = node + alloc_u64`. In-place ops never change it.
Two memmoves always, never three.

Bitmask nodes: `alloc_u64` = total allocation. Unchanged.

---

### A. kntrie_support.hpp

#### A1. Allocation table + round_up_u64

OLD:
```cpp
inline constexpr size_t FREE_MAX  = 128;
inline constexpr size_t NUM_BINS  = 12;
inline constexpr size_t BIN_SIZES[NUM_BINS] = {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128};

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= FREE_MAX) {
        for (size_t i = 0; i < NUM_BINS; ++i)
            if (n <= BIN_SIZES[i]) return BIN_SIZES[i];
    }
    int bit  = static_cast<int>(std::bit_width(n - 1));
    size_t pow2 = size_t{1} << bit;
    static constexpr size_t MIDPOINT_MIN_BIAS = 2;  // ensures mid > pow2/2 for smallest sizes
    size_t mid  = pow2 / 2 + pow2 / 4 + MIDPOINT_MIN_BIAS;
    return (n <= mid) ? mid : pow2;
}
```

NEW:
```cpp
inline constexpr size_t ALLOC_CLASS_TABLE[] = {
    4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128,
    194, 256, 386, 512, 770, 1024, 1538, 2048,
    3074, 4096, 6146, 8192, 12290, 16384
};
inline constexpr size_t NUM_ALLOC_CLASSES = sizeof(ALLOC_CLASS_TABLE) / sizeof(size_t);

inline constexpr size_t round_up_u64(size_t n) noexcept {
    for (size_t i = 0; i < NUM_ALLOC_CLASSES; ++i)
        if (n <= ALLOC_CLASS_TABLE[i]) return ALLOC_CLASS_TABLE[i];
    return n;  // beyond table — exact
}
```

---

### B. kntrie_compact.hpp

#### B1. New helpers (add after size_u64)

```cpp
// Value block size in u64s for a given entry count
static constexpr size_t val_block_u64(size_t count) noexcept {
    if constexpr (VT::IS_BOOL)
        return bool_slots::u64_for(count);
    else
        return align_up(count * sizeof(VST), U64_BYTES) / U64_BYTES;
}

// Max entries that fit in a total allocation
static constexpr size_t capacity_for(size_t total_u64) noexcept {
    size_t lo = 0, hi = (total_u64 - LEAF_HEADER_U64) * U64_BYTES / sizeof(K);
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (size_u64(mid) <= total_u64) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// Set alloc_u64 = value offset for a given total allocation
static void set_val_offset(uint64_t* node, size_t total_u64) noexcept {
    size_t cap = capacity_for(total_u64);
    size_t val_off = total_u64 - val_block_u64(cap);
    get_header(node)->set_alloc_u64(static_cast<uint16_t>(val_off));
}

// Recover total allocation from val_offset (cold — dealloc/shrink only)
static size_t total_from_val_offset(const node_header_t* h) noexcept {
    size_t val_off = h->alloc_u64();
    size_t key_cap = (val_off - LEAF_HEADER_U64) * U64_BYTES / sizeof(K);
    return val_off + val_block_u64(key_cap);
}
```

#### B2. vals_mut / vals / bool_vals_mut / bool_vals

OLD:
```cpp
static VST* vals_mut(uint64_t* node, size_t total, size_t header_size) noexcept {
    size_t kb = align_up(total * sizeof(K), U64_BYTES);
    return reinterpret_cast<VST*>(
        reinterpret_cast<char*>(node + header_size) + kb);
}
static const VST* vals(const uint64_t* node, size_t total, size_t header_size) noexcept {
    size_t kb = align_up(total * sizeof(K), U64_BYTES);
    return reinterpret_cast<const VST*>(
        reinterpret_cast<const char*>(node + header_size) + kb);
}
static bool_slots bool_vals_mut(uint64_t* node, size_t total, size_t header_size) noexcept {
    size_t kb = align_up(total * sizeof(K), U64_BYTES);
    return bool_slots{ reinterpret_cast<uint64_t*>(
        reinterpret_cast<char*>(node + header_size) + kb) };
}
static bool_slots bool_vals(const uint64_t* node, size_t total, size_t header_size) noexcept {
    size_t kb = align_up(total * sizeof(K), U64_BYTES);
    return bool_slots{ const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(
        reinterpret_cast<const char*>(node + header_size) + kb)) };
}
```

NEW:
```cpp
static VST* vals_mut(uint64_t* node) noexcept {
    return reinterpret_cast<VST*>(node + get_header(node)->alloc_u64());
}
static const VST* vals(const uint64_t* node) noexcept {
    return reinterpret_cast<const VST*>(node + get_header(node)->alloc_u64());
}
static bool_slots bool_vals_mut(uint64_t* node) noexcept {
    return bool_slots{ reinterpret_cast<uint64_t*>(
        node + get_header(node)->alloc_u64()) };
}
static bool_slots bool_vals(const uint64_t* node) noexcept {
    return bool_slots{ const_cast<uint64_t*>(reinterpret_cast<const uint64_t*>(
        node + get_header(node)->alloc_u64())) };
}
```

#### B3. has_room

OLD:
```cpp
static constexpr bool has_room(unsigned entries, unsigned alloc_u64) noexcept {
    return size_u64(entries + 1) <= alloc_u64;
}
```

NEW:
```cpp
static bool has_room(unsigned entries, const node_header_t* h) noexcept {
    size_t key_end = LEAF_HEADER_U64
        + align_up(static_cast<size_t>(entries + 1) * sizeof(K), U64_BYTES) / U64_BYTES;
    return key_end <= h->alloc_u64();
}
```

#### B4. val_ptr

OLD:
```cpp
static void* val_ptr(uint64_t* node, unsigned entries, size_t hs, uint16_t pos) noexcept {
    if constexpr (VT::IS_BOOL)
        return bool_vals_mut(node, entries, hs).data;
    else
        return &vals_mut(node, entries, hs)[pos];
}
```

NEW:
```cpp
static void* val_ptr(uint64_t* node, uint16_t pos) noexcept {
    if constexpr (VT::IS_BOOL)
        return bool_vals_mut(node).data;
    else
        return &vals_mut(node)[pos];
}
```

#### B5. make_leaf

OLD:
```cpp
static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                           unsigned count, BLD& bld) {
    constexpr size_t hu = LEAF_HEADER_U64;
    size_t au64 = round_up_u64(size_u64(count, hu));
    uint64_t* node = bld.alloc_node(au64, false);
    auto* h = get_header(node);
    h->set_entries(count);
    h->set_alloc_u64(au64);
    if (count > 0) {
        std::memcpy(keys(node, hu), sorted_keys, count * sizeof(K));
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node, count, hu);
            bv.clear_all(count);
            for (unsigned i = 0; i < count; ++i)
                bv.set(i, values[i]);
        } else {
            VT::copy_uninit(values, count, vals_mut(node, count, hu));
        }
    }
    return node;
}
```

NEW:
```cpp
static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                           unsigned count, BLD& bld) {
    constexpr size_t hu = LEAF_HEADER_U64;
    size_t total = round_up_u64(size_u64(count, hu));
    uint64_t* node = bld.alloc_node(total, false);
    auto* h = get_header(node);
    h->set_entries(count);
    set_val_offset(node, total);
    if (count > 0) {
        std::memcpy(keys(node, hu), sorted_keys, count * sizeof(K));
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node);
            bv.clear_all(count);
            for (unsigned i = 0; i < count; ++i)
                bv.set(i, values[i]);
        } else {
            VT::copy_uninit(values, count, vals_mut(node));
        }
    }
    return node;
}
```

#### B6. for_each

OLD:
```cpp
template<typename Fn>
static void for_each(const uint64_t* node, const node_header_t* h, Fn&& cb) {
    unsigned entries = h->entries();
    size_t hs = LEAF_HEADER_U64;
    const K* kd = keys(node, hs);
    if constexpr (VT::IS_BOOL) {
        auto bv = bool_vals(node, entries, hs);
        for (unsigned i = 0; i < entries; ++i)
            cb(kd[i], bv.get(i));
    } else {
        const VST* vd = vals(node, entries, hs);
        for (unsigned i = 0; i < entries; ++i)
            cb(kd[i], vd[i]);
    }
}
```

NEW:
```cpp
template<typename Fn>
static void for_each(const uint64_t* node, const node_header_t* h, Fn&& cb) {
    unsigned entries = h->entries();
    const K* kd = keys(node, LEAF_HEADER_U64);
    if constexpr (VT::IS_BOOL) {
        auto bv = bool_vals(node);
        for (unsigned i = 0; i < entries; ++i)
            cb(kd[i], bv.get(i));
    } else {
        const VST* vd = vals(node);
        for (unsigned i = 0; i < entries; ++i)
            cb(kd[i], vd[i]);
    }
}
```

#### B7. destroy_and_dealloc

OLD:
```cpp
static void destroy_and_dealloc(uint64_t* node, BLD& bld) {
    auto* h = get_header(node);
    if constexpr (VT::HAS_DESTRUCTOR) {
        unsigned entries = h->entries();
        size_t hs = LEAF_HEADER_U64;
        VST* vd = vals_mut(node, entries, hs);
        for (unsigned i = 0; i < entries; ++i)
            bld.destroy_value(vd[i]);
    }
    bld.dealloc_node(node, h->alloc_u64());
}
```

NEW:
```cpp
static void destroy_and_dealloc(uint64_t* node, BLD& bld) {
    auto* h = get_header(node);
    if constexpr (VT::HAS_DESTRUCTOR) {
        VST* vd = vals_mut(node);
        unsigned entries = h->entries();
        for (unsigned i = 0; i < entries; ++i)
            bld.destroy_value(vd[i]);
    }
    bld.dealloc_node(node, total_from_val_offset(h));
}
```

#### B8. dealloc_node_only

OLD:
```cpp
static void dealloc_node_only(uint64_t* node, BLD& bld) {
    bld.dealloc_node(node, get_header(node)->alloc_u64());
}
```

NEW:
```cpp
static void dealloc_node_only(uint64_t* node, BLD& bld) {
    bld.dealloc_node(node, total_from_val_offset(get_header(node)));
}
```

#### B9. insert ASSIGN path

OLD:
```cpp
if constexpr (ASSIGN) {
    if constexpr (VT::IS_BOOL) {
        bool_vals_mut(node, entries, hs).set(idx, value);
    } else {
        VST* vd = vals_mut(node, entries, hs);
        bld.destroy_value(vd[idx]);
        VT::init_slot(&vd[idx], value);
    }
}
```

NEW:
```cpp
if constexpr (ASSIGN) {
    if constexpr (VT::IS_BOOL) {
        bool_vals_mut(node).set(idx, value);
    } else {
        VST* vd = vals_mut(node);
        bld.destroy_value(vd[idx]);
        VT::init_slot(&vd[idx], value);
    }
}
```

#### B10. insert grow-realloc

OLD:
```cpp
if (!has_room(entries, h->alloc_u64())) [[unlikely]] {
    unsigned new_entries = entries + 1;
    size_t au64 = round_up_u64(size_u64(new_entries, hs));
    uint64_t* nn = bld.alloc_node(au64, false);
    auto* nh = get_header(nn);
    copy_leaf_header(node, nn);
    nh->set_entries(new_entries);
    nh->set_alloc_u64(au64);
    
    set_leaf_fns(nn, get_header(nn)->skip() > 0);

    K* nk = keys(nn, hs);
    // Copy keys with gap at ins
    if (ins > 0)
        std::memcpy(nk, kd, ins * sizeof(K));
    nk[ins] = suffix;
    int tail = entries - ins;
    if (tail > 0)
        std::memcpy(nk + ins + 1, kd + ins, tail * sizeof(K));

    if constexpr (VT::IS_BOOL) {
        auto old_bv = bool_vals(node, entries, hs);
        auto new_bv = bool_vals_mut(nn, new_entries, hs);
        new_bv.clear_all(new_entries);
        for (int i = 0; i < ins; ++i) new_bv.set(i, old_bv.get(i));
        new_bv.set(ins, value);
        for (unsigned i = ins; i < entries; ++i) new_bv.set(i + 1, old_bv.get(i));
    } else {
        VST* ov = vals_mut(node, entries, hs);
        VST* nv = vals_mut(nn, new_entries, hs);
        if (ins > 0) VT::copy_uninit(ov, ins, nv);
        VT::init_slot(&nv[ins], value);
        if (tail > 0) VT::copy_uninit(ov + ins, tail, nv + ins + 1);
    }

    bld.dealloc_node(node, h->alloc_u64());
    return {tag_leaf(nn), true, false, nullptr,
            nn, static_cast<uint16_t>(ins)};
}
```

NEW:
```cpp
if (!has_room(entries, h)) [[unlikely]] {
    unsigned new_entries = entries + 1;
    size_t total = round_up_u64(size_u64(new_entries, hs));
    uint64_t* nn = bld.alloc_node(total, false);
    auto* nh = get_header(nn);
    copy_leaf_header(node, nn);
    nh->set_entries(new_entries);
    set_val_offset(nn, total);
    
    set_leaf_fns(nn, get_header(nn)->skip() > 0);

    K* nk = keys(nn, hs);
    std::memcpy(nk, kd, ins * sizeof(K));
    nk[ins] = suffix;
    int tail = entries - ins;
    std::memcpy(nk + ins + 1, kd + ins, tail * sizeof(K));

    if constexpr (VT::IS_BOOL) {
        auto old_bv = bool_vals(node);
        auto new_bv = bool_vals_mut(nn);
        new_bv.clear_all(new_entries);
        for (int i = 0; i < ins; ++i) new_bv.set(i, old_bv.get(i));
        new_bv.set(ins, value);
        for (unsigned i = ins; i < entries; ++i) new_bv.set(i + 1, old_bv.get(i));
    } else {
        VST* ov = vals_mut(node);
        VST* nv = vals_mut(nn);
        if (ins > 0) VT::copy_uninit(ov, ins, nv);
        VT::init_slot(&nv[ins], value);
        if (tail > 0) VT::copy_uninit(ov + ins, tail, nv + ins + 1);
    }

    bld.dealloc_node(node, total_from_val_offset(h));
    return {tag_leaf(nn), true, false, nullptr,
            nn, static_cast<uint16_t>(ins)};
}
```

#### B11. insert in-place (full block, both paths)

OLD:
```cpp
// In-place: memmove tail right by 1
// Key array grows by sizeof(K), which may shift the value start.
int tail = entries - ins;
if constexpr (VT::IS_BOOL) {
    auto bv = bool_vals_mut(node, entries, hs);
    if (tail > 0) {
        std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
        bv.shift_right_1(ins, tail);
    }
    kd[ins] = suffix;
    bv.set(ins, value);
} else {
    VST* old_vd = vals_mut(node, entries, hs);
    VST* new_vd = vals_mut(node, entries + 1, hs);
    if (new_vd != old_vd) {
        // Value offset changed — relocate all values first (right shift)
        std::memmove(new_vd, old_vd, entries * sizeof(VST));
        // Shift tail values for insertion gap
        if (tail > 0)
            std::memmove(new_vd + ins + 1, new_vd + ins, tail * sizeof(VST));
        // Now safe to shift keys (may overwrite old value area)
        if (tail > 0)
            std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
        kd[ins] = suffix;
        VT::init_slot(&new_vd[ins], value);
    } else {
        // No relocation needed
        if (tail > 0) {
            std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
            std::memmove(old_vd + ins + 1, old_vd + ins, tail * sizeof(VST));
        }
        kd[ins] = suffix;
        VT::init_slot(&old_vd[ins], value);
    }
}
```

NEW:
```cpp
int tail = entries - ins;
if constexpr (VT::IS_BOOL) {
    auto bv = bool_vals_mut(node);
    std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
    bv.shift_right_1(ins, tail);
    kd[ins] = suffix;
    bv.set(ins, value);
} else {
    VST* vd = vals_mut(node);
    std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
    std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
    kd[ins] = suffix;
    VT::init_slot(&vd[ins], value);
}
```

#### B12. erase shrink-realloc

OLD:
```cpp
// Shrink check: does the allocation class drop?
size_t needed_u64 = size_u64(nc, hs);
if (should_shrink_u64(h->alloc_u64(), needed_u64)) [[unlikely]] {
    size_t au64 = round_up_u64(needed_u64);
    uint64_t* nn = bld.alloc_node(au64, false);
    auto* nh = get_header(nn);
    copy_leaf_header(node, nn);
    nh->set_entries(nc);
    nh->set_alloc_u64(au64);
    
    set_leaf_fns(nn, get_header(nn)->skip() > 0);

    K* nk = keys(nn, hs);
    // Copy keys skipping idx
    if (idx > 0) std::memcpy(nk, kd, idx * sizeof(K));
    unsigned tail = entries - idx - 1;
    if (tail > 0) std::memcpy(nk + idx, kd + idx + 1, tail * sizeof(K));

    if constexpr (VT::IS_BOOL) {
        auto old_bv = bool_vals(node, entries, hs);
        auto new_bv = bool_vals_mut(nn, nc, hs);
        new_bv.clear_all(nc);
        for (unsigned i = 0; i < idx; ++i) new_bv.set(i, old_bv.get(i));
        for (unsigned i = idx + 1; i < entries; ++i) new_bv.set(i - 1, old_bv.get(i));
    } else {
        const VST* ov = vals(node, entries, hs);
        VST* nv = vals_mut(nn, nc, hs);
        if (idx > 0) VT::copy_uninit(ov, idx, nv);
        if constexpr (VT::HAS_DESTRUCTOR)
            bld.destroy_value(const_cast<VST&>(ov[idx]));
        if (tail > 0) VT::copy_uninit(ov + idx + 1, tail, nv + idx);
    }

    bld.dealloc_node(node, h->alloc_u64());
    return {tag_leaf(nn), true, nc};
}
```

NEW:
```cpp
size_t needed_u64 = size_u64(nc, hs);
size_t current_total = total_from_val_offset(h);
if (should_shrink_u64(current_total, needed_u64)) [[unlikely]] {
    size_t total = round_up_u64(needed_u64);
    uint64_t* nn = bld.alloc_node(total, false);
    auto* nh = get_header(nn);
    copy_leaf_header(node, nn);
    nh->set_entries(nc);
    set_val_offset(nn, total);
    
    set_leaf_fns(nn, get_header(nn)->skip() > 0);

    K* nk = keys(nn, hs);
    std::memcpy(nk, kd, idx * sizeof(K));
    unsigned tail = entries - idx - 1;
    std::memcpy(nk + idx, kd + idx + 1, tail * sizeof(K));

    if constexpr (VT::IS_BOOL) {
        auto old_bv = bool_vals(node);
        auto new_bv = bool_vals_mut(nn);
        new_bv.clear_all(nc);
        for (unsigned i = 0; i < idx; ++i) new_bv.set(i, old_bv.get(i));
        for (unsigned i = idx + 1; i < entries; ++i) new_bv.set(i - 1, old_bv.get(i));
    } else {
        const VST* ov = vals(node);
        VST* nv = vals_mut(nn);
        if (idx > 0) VT::copy_uninit(ov, idx, nv);
        if constexpr (VT::HAS_DESTRUCTOR)
            bld.destroy_value(const_cast<VST&>(ov[idx]));
        if (tail > 0) VT::copy_uninit(ov + idx + 1, tail, nv + idx);
    }

    bld.dealloc_node(node, current_total);
    return {tag_leaf(nn), true, nc};
}
```

#### B13. erase in-place (full block, both paths)

OLD:
```cpp
// In-place: memmove tail left by 1
// Key array shrinks by sizeof(K), which may shift value start left.
unsigned tail = entries - idx - 1;
if constexpr (VT::IS_BOOL) {
    auto bv = bool_vals_mut(node, entries, hs);
    if (tail > 0) {
        std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
        bv.shift_left_1(idx + 1, tail);
    }
} else {
    VST* old_vd = vals_mut(node, entries, hs);
    VST* new_vd = vals_mut(node, nc, hs);
    if constexpr (VT::HAS_DESTRUCTOR)
        bld.destroy_value(old_vd[idx]);
    if (new_vd != old_vd) {
        // Value offset changed — shift keys first, then relocate values left
        if (tail > 0)
            std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
        // Copy values skipping idx directly to new position
        if (idx > 0)
            std::memmove(new_vd, old_vd, idx * sizeof(VST));
        if (tail > 0)
            std::memmove(new_vd + idx, old_vd + idx + 1, tail * sizeof(VST));
    } else {
        if (tail > 0) {
            std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
            std::memmove(old_vd + idx, old_vd + idx + 1, tail * sizeof(VST));
        }
    }
}
```

NEW:
```cpp
unsigned tail = entries - idx - 1;
if constexpr (VT::IS_BOOL) {
    auto bv = bool_vals_mut(node);
    std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
    bv.shift_left_1(idx + 1, tail);
} else {
    VST* vd = vals_mut(node);
    if constexpr (VT::HAS_DESTRUCTOR)
        bld.destroy_value(vd[idx]);
    std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
    std::memmove(vd + idx, vd + idx + 1, tail * sizeof(VST));
}
```

#### B14. find_fn val_ptr call

OLD:
```cpp
return {node, pos, 0, key, val_ptr(node, entries, hs, pos), true};
```

NEW:
```cpp
return {node, pos, 0, key, val_ptr(node, pos), true};
```

#### B15. find_edge_fn val_ptr call

OLD:
```cpp
return {node, pos, 0, key, val_ptr(node, entries, hs, pos), true};
```

NEW:
```cpp
return {node, pos, 0, key, val_ptr(node, pos), true};
```

---

### C. kntrie_ops.hpp

#### C1. value_ref_at — compact paths

OLD:
```cpp
unsigned entries = get_header(node)->entries();
if (nk_bits <= U16_BITS) {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint16_t, VALUE, ALLOC>::vals_mut(node, entries, hs)[pos]);
} else if (nk_bits <= U32_BITS) {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint32_t, VALUE, ALLOC>::vals_mut(node, entries, hs)[pos]);
} else {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint64_t, VALUE, ALLOC>::vals_mut(node, entries, hs)[pos]);
}
```

NEW:
```cpp
if (nk_bits <= U16_BITS) {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint16_t, VALUE, ALLOC>::vals_mut(node)[pos]);
} else if (nk_bits <= U32_BITS) {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint32_t, VALUE, ALLOC>::vals_mut(node)[pos]);
} else {
    return *reinterpret_cast<VALUE*>(
        &compact_ops<uint64_t, VALUE, ALLOC>::vals_mut(node)[pos]);
}
```

#### C2. bool_ref_at — compact paths

OLD:
```cpp
unsigned entries = get_header(node)->entries();
if (nk_bits <= U16_BITS) {
    auto bv = compact_ops<uint16_t, VALUE, ALLOC>::bool_vals_mut(node, entries, hs);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
} else if (nk_bits <= U32_BITS) {
    auto bv = compact_ops<uint32_t, VALUE, ALLOC>::bool_vals_mut(node, entries, hs);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
} else {
    auto bv = compact_ops<uint64_t, VALUE, ALLOC>::bool_vals_mut(node, entries, hs);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
}
```

NEW:
```cpp
if (nk_bits <= U16_BITS) {
    auto bv = compact_ops<uint16_t, VALUE, ALLOC>::bool_vals_mut(node);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
} else if (nk_bits <= U32_BITS) {
    auto bv = compact_ops<uint32_t, VALUE, ALLOC>::bool_vals_mut(node);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
} else {
    auto bv = compact_ops<uint64_t, VALUE, ALLOC>::bool_vals_mut(node);
    return {&bv.data[pos / U64_BITS], static_cast<uint8_t>(pos % U64_BITS)};
}
```

#### C3. leaf_first_after — compact val_ptr calls

OLD:
```cpp
return {node, pos, 0, key, CO::val_ptr(node, entries, hs, pos), true};
```

NEW (both FWD and BWD returns):
```cpp
return {node, pos, 0, key, CO::val_ptr(node, pos), true};
```

#### C4. convert_to_bitmask_tagged dealloc

OLD:
```cpp
bld.dealloc_node(const_cast<uint64_t*>(node), hdr->alloc_u64());
```

NEW:
```cpp
bld.dealloc_node(const_cast<uint64_t*>(node),
    compact_ops_for_hdr::total_from_val_offset(hdr));
```

Note: need to resolve the correct compact_ops type. Since this is inside
kntrie_ops which dispatches on BITS, use the already-scoped NK:
```cpp
using CO = compact_ops<NK, VALUE, ALLOC>;
bld.dealloc_node(const_cast<uint64_t*>(node), CO::total_from_val_offset(hdr));
```

---

## 2. Zero-alloc compact overflow (convert_to_bitmask_tagged)

### Problem

Two heap allocs for 4097-element temp arrays:
```cpp
auto wk = std::make_unique<NK[]>(total);
auto wv = std::make_unique<VST[]>(total);
```

### D. kntrie_ops.hpp

#### D1. convert_to_bitmask_tagged

OLD:
```cpp
template<int BITS>
static insert_result_t convert_to_bitmask_tagged(const uint64_t* node,
                                            const node_header_t* hdr,
                                            uint64_t ik, VST value,
                                            BLD& bld) {
    using NK = nk_for_bits_t<BITS>;
    NK suffix = leaf_ops_t<BITS>::template to_suffix<BITS>(ik);

    uint16_t old_count = hdr->entries();
    size_t total = old_count + 1;
    auto wk = std::make_unique<NK[]>(total);
    auto wv = std::make_unique<VST[]>(total);

    size_t wi = 0;
    bool ins = false;
    leaf_for_each<BITS>(node, hdr, [&](NK s, VST v) {
        if (!ins && suffix < s) {
            wk[wi] = suffix; wv[wi] = value; wi++; ins = true;
        }
        wk[wi] = s; wv[wi] = v; wi++;
    });
    if (!ins) { wk[wi] = suffix; wv[wi] = value; }

    uint64_t child_tagged = build_node_from_arrays_tagged<BITS>(
        wk.get(), wv.get(), total, ik, bld);

    // Propagate old skip to new child.
    uint8_t ps = hdr->skip();
    if (ps > 0) {
        if (child_tagged & LEAF_BIT) {
            uint64_t* leaf = untag_leaf_mut(child_tagged);
            uint8_t old_leaf_skip = get_header(leaf)->skip();
            uint8_t new_skip = old_leaf_skip + ps;
            int mshift = static_cast<int>(U64_BITS) - KEY_BITS + BITS - CHAR_BIT * old_leaf_skip;
            uint64_t mask = (mshift >= static_cast<int>(U64_BITS)) ? 0 : (~0ULL << mshift);
            set_leaf_prefix(leaf, ik & mask);
            set_depth(leaf, make_depth<BITS>(new_skip));
            if (old_leaf_skip == 0) set_leaf_fns_for<BITS>(leaf);
            child_tagged = tag_leaf(leaf);
        } else {
            constexpr int BS = byte_shift<BITS>();
            uint8_t pfx_bytes[MAX_SKIP];
            for (uint8_t i = 0; i < ps; ++i)
                pfx_bytes[i] = static_cast<uint8_t>(ik >> (BS + CHAR_BIT * (ps - i)));
            uint64_t* bm_node = bm_to_node(child_tagged);
            child_tagged = BO::wrap_in_chain(bm_node, pfx_bytes, ps, bld);
        }
    }

    bld.dealloc_node(const_cast<uint64_t*>(node), hdr->alloc_u64());

    // Locate the inserted entry in the new subtree (cache-hot).
    iter_entry_t lp;
    if (child_tagged & LEAF_BIT) {
        uint64_t* leaf = untag_leaf_mut(child_tagged);
        lp = get_find_fn(leaf)(leaf, ik);
    } else {
        constexpr int CONSUMED_BITS = KEY_BITS - BITS;
        int shift_amt = CONSUMED_BITS - ps * CHAR_BIT;
        uint64_t shifted = (shift_amt > 0) ? (ik << shift_amt) : ik;
        lp = BO::find_loop(child_tagged, ik, shifted);
    }
    return {child_tagged, true, false, nullptr, lp.leaf, lp.pos};
}
```

NEW:
```cpp
template<int BITS>
static insert_result_t convert_to_bitmask_tagged(const uint64_t* node,
                                            const node_header_t* hdr,
                                            uint64_t ik, VST value,
                                            BLD& bld) {
    using NK = nk_for_bits_t<BITS>;
    constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
    NK suffix = leaf_ops_t<BITS>::template to_suffix<BITS>(ik);
    uint8_t new_top_byte = static_cast<uint8_t>(suffix >> (NK_BITS - CHAR_BIT));

    uint16_t old_count = hdr->entries();
    constexpr size_t hs = LEAF_HEADER_U64;

    // Direct pointers into old leaf — no top-level temp arrays
    using CO = compact_ops<NK, VALUE, ALLOC>;
    const NK* old_keys = CO::keys(node, hs);
    const VST* old_vals = CO::vals(node);

    // Find insertion point in old array
    unsigned ins = 0;
    while (ins < old_count && old_keys[ins] < suffix) ++ins;

    // Partition by top byte into children.
    // Three loops: bytes < new_top_byte, byte == new_top_byte, bytes > new_top_byte.
    uint8_t indices[BYTE_VALUES];
    uint64_t children[BYTE_VALUES];
    int n_children = 0;

    size_t i = 0;

    // Loop 1: byte ranges < new_top_byte — pass old array slices directly
    while (i < old_count) {
        uint8_t ti = static_cast<uint8_t>(old_keys[i] >> (NK_BITS - CHAR_BIT));
        if (ti >= new_top_byte) break;
        size_t start = i;
        while (i < old_count &&
               static_cast<uint8_t>(old_keys[i] >> (NK_BITS - CHAR_BIT)) == ti) ++i;
        size_t cc = i - start;
        uint64_t child_ik = (ik & safe_prefix_mask<BITS>())
            | leaf_ops_t<BITS>::template suffix_to_u64<BITS>(old_keys[start]);
        indices[n_children] = ti;
        children[n_children] = build_node_from_arrays_tagged<BITS>(
            const_cast<NK*>(old_keys + start), const_cast<VST*>(old_vals + start),
            cc, child_ik, bld);
        n_children++;
    }

    // Loop 2: byte == new_top_byte — pass three segments, zero copy
    {
        size_t range_start = i;
        while (i < old_count &&
               static_cast<uint8_t>(old_keys[i] >> (NK_BITS - CHAR_BIT)) == new_top_byte) ++i;
        size_t range_old = i - range_start;
        unsigned local_ins = ins - static_cast<unsigned>(range_start);

        indices[n_children] = new_top_byte;
        children[n_children] = build_node_from_arrays_tagged_ins<BITS>(
            const_cast<NK*>(old_keys + range_start),
            const_cast<VST*>(old_vals + range_start),
            local_ins,
            suffix, value,
            const_cast<NK*>(old_keys + range_start + local_ins),
            const_cast<VST*>(old_vals + range_start + local_ins),
            range_old - local_ins,
            ik, bld);
        n_children++;
    }

    // Loop 3: byte ranges > new_top_byte — pass old array slices directly
    while (i < old_count) {
        uint8_t ti = static_cast<uint8_t>(old_keys[i] >> (NK_BITS - CHAR_BIT));
        size_t start = i;
        while (i < old_count &&
               static_cast<uint8_t>(old_keys[i] >> (NK_BITS - CHAR_BIT)) == ti) ++i;
        size_t cc = i - start;
        uint64_t child_ik = (ik & safe_prefix_mask<BITS>())
            | leaf_ops_t<BITS>::template suffix_to_u64<BITS>(old_keys[start]);
        indices[n_children] = ti;
        children[n_children] = build_node_from_arrays_tagged<BITS>(
            const_cast<NK*>(old_keys + start), const_cast<VST*>(old_vals + start),
            cc, child_ik, bld);
        n_children++;
    }

    size_t total = old_count + 1;
    uint64_t child_tagged;
    if (n_children == 1) {
        child_tagged = children[0];
    } else {
        child_tagged = tag_bitmask(
            BO::make_bitmask(indices, children, n_children, bld, total));
    }

    // Propagate old skip to new child (unchanged from before).
    uint8_t ps = hdr->skip();
    if (ps > 0) {
        if (child_tagged & LEAF_BIT) {
            uint64_t* leaf = untag_leaf_mut(child_tagged);
            uint8_t old_leaf_skip = get_header(leaf)->skip();
            uint8_t new_skip = old_leaf_skip + ps;
            int mshift = static_cast<int>(U64_BITS) - KEY_BITS + BITS - CHAR_BIT * old_leaf_skip;
            uint64_t mask = (mshift >= static_cast<int>(U64_BITS)) ? 0 : (~0ULL << mshift);
            set_leaf_prefix(leaf, ik & mask);
            set_depth(leaf, make_depth<BITS>(new_skip));
            if (old_leaf_skip == 0) set_leaf_fns_for<BITS>(leaf);
            child_tagged = tag_leaf(leaf);
        } else {
            constexpr int BS = byte_shift<BITS>();
            uint8_t pfx_bytes[MAX_SKIP];
            for (uint8_t i = 0; i < ps; ++i)
                pfx_bytes[i] = static_cast<uint8_t>(ik >> (BS + CHAR_BIT * (ps - i)));
            uint64_t* bm_node = bm_to_node(child_tagged);
            child_tagged = BO::wrap_in_chain(bm_node, pfx_bytes, ps, bld);
        }
    }

    bld.dealloc_node(const_cast<uint64_t*>(node), CO::total_from_val_offset(hdr));

    // Locate the inserted entry in the new subtree (cache-hot).
    iter_entry_t lp;
    if (child_tagged & LEAF_BIT) {
        uint64_t* leaf = untag_leaf_mut(child_tagged);
        lp = get_find_fn(leaf)(leaf, ik);
    } else {
        constexpr int CONSUMED_BITS = KEY_BITS - BITS;
        int shift_amt = CONSUMED_BITS - ps * CHAR_BIT;
        uint64_t shifted = (shift_amt > 0) ? (ik << shift_amt) : ik;
        lp = BO::find_loop(child_tagged, ik, shifted);
    }
    return {child_tagged, true, false, nullptr, lp.leaf, lp.pos};
}
```

Zero heap allocs. Loops 1 & 3 pass old array slices directly.
Loop 2 passes three segments to `build_node_from_arrays_tagged_ins`.

#### D1b. New helper: build_node_from_arrays_tagged_ins (add to kntrie_ops.hpp)

No OLD — new code:

```cpp
// Build node from three segments: before[0..before_count) + {new_key, new_val} + after[0..after_count).
// Same logic as build_node_from_arrays_tagged but reads from virtual merged sequence.
// Only the one byte bucket straddling the insertion calls this recursively;
// all other buckets are contiguous slices passed to build_node_from_arrays_tagged.
template<int BITS>
static uint64_t build_node_from_arrays_tagged_ins(
        nk_for_bits_t<BITS>* before_keys, VST* before_vals, size_t before_count,
        nk_for_bits_t<BITS> new_key, VST new_val,
        nk_for_bits_t<BITS>* after_keys, VST* after_vals, size_t after_count,
        uint64_t ik, BLD& bld) {
    using NK = nk_for_bits_t<BITS>;
    constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
    size_t total = before_count + 1 + after_count;

    // Leaf case: total fits in one compact leaf — copy-with-gap into it
    if (total <= COMPACT_MAX) {
        NK suf_arr[COMPACT_MAX + 1];
        VST val_arr[COMPACT_MAX + 1];
        std::memcpy(suf_arr, before_keys, before_count * sizeof(NK));
        suf_arr[before_count] = new_key;
        std::memcpy(suf_arr + before_count + 1, after_keys, after_count * sizeof(NK));
        std::memcpy(val_arr, before_vals, before_count * sizeof(VST));
        val_arr[before_count] = new_val;
        std::memcpy(val_arr + before_count + 1, after_vals, after_count * sizeof(VST));
        return tag_leaf(build_leaf<BITS>(suf_arr, val_arr, total, ik, bld));
    }

    // Multi-child bitmask: partition by top byte.
    // The new_key only affects one byte bucket.
    uint8_t new_top = static_cast<uint8_t>(new_key >> (NK_BITS - CHAR_BIT));

    uint8_t indices[BYTE_VALUES];
    uint64_t children[BYTE_VALUES];
    int n_children = 0;

    // Helper: process contiguous runs from an array
    auto process_runs = [&](NK* ks, VST* vs, size_t count, size_t& pos) {
        while (pos < count) {
            uint8_t ti = static_cast<uint8_t>(ks[pos] >> (NK_BITS - CHAR_BIT));
            if (ti >= new_top) return;
            size_t start = pos;
            while (pos < count &&
                   static_cast<uint8_t>(ks[pos] >> (NK_BITS - CHAR_BIT)) == ti) ++pos;
            size_t cc = pos - start;
            uint64_t child_ik = (ik & safe_prefix_mask<BITS>())
                | leaf_ops_t<BITS>::template suffix_to_u64<BITS>(ks[start]);
            indices[n_children] = ti;
            children[n_children] = build_node_from_arrays_tagged<BITS>(
                ks + start, vs + start, cc, child_ik, bld);
            n_children++;
        }
    };

    // Before-keys runs with byte < new_top
    size_t bi = 0;
    process_runs(before_keys, before_vals, before_count, bi);

    // The byte == new_top bucket: merge before tail + new + after head
    // before[bi..before_count) all have byte == new_top (or are empty)
    // after[0..aj) all have byte == new_top (or are empty)
    size_t aj = 0;
    while (aj < after_count &&
           static_cast<uint8_t>(after_keys[aj] >> (NK_BITS - CHAR_BIT)) == new_top) ++aj;

    size_t merged_before = before_count - bi;
    size_t merged_after = aj;
    size_t merged_total = merged_before + 1 + merged_after;

    if constexpr (BITS > U8_BITS) {
        indices[n_children] = new_top;
        children[n_children] = build_node_from_arrays_tagged_ins<BITS>(
            before_keys + bi, before_vals + bi, merged_before,
            new_key, new_val,
            after_keys, after_vals, merged_after,
            ik, bld);
        n_children++;
    }

    // After-keys runs with byte > new_top
    size_t ai = aj;
    auto process_runs_after = [&](NK* ks, VST* vs, size_t count, size_t& pos) {
        while (pos < count) {
            uint8_t ti = static_cast<uint8_t>(ks[pos] >> (NK_BITS - CHAR_BIT));
            size_t start = pos;
            while (pos < count &&
                   static_cast<uint8_t>(ks[pos] >> (NK_BITS - CHAR_BIT)) == ti) ++pos;
            size_t cc = pos - start;
            uint64_t child_ik = (ik & safe_prefix_mask<BITS>())
                | leaf_ops_t<BITS>::template suffix_to_u64<BITS>(ks[start]);
            indices[n_children] = ti;
            children[n_children] = build_node_from_arrays_tagged<BITS>(
                ks + start, vs + start, cc, child_ik, bld);
            n_children++;
        }
    };
    process_runs_after(after_keys, after_vals, after_count, ai);

    return tag_bitmask(
        BO::make_bitmask(indices, children, n_children, bld, total));
}
```

---

## 3. Single-pass coalesce

### Problem

`do_coalesce` → `collect_bm_final` → `collect_entries` → recursive temp arrays.
O(children) heap allocations.

### D2. New helper: walk_entries_in_order (add to kntrie_ops.hpp)

No OLD — new code:

```cpp
// Walk subtree in sorted order, calling cb(NK suffix, VST value) for each entry.
// Zero intermediate allocations.
template<int BITS, typename Fn>
static void walk_entries_in_order(uint64_t tagged, Fn&& cb) {
    using NK = nk_for_bits_t<BITS>;

    if (tagged & LEAF_BIT) [[unlikely]] {
        const uint64_t* node = untag_leaf(tagged);
        const auto* hdr = get_header(node);
        // Leaf: iterate entries directly
        if constexpr (sizeof(NK) == sizeof(uint8_t)) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(static_cast<NK>(s), v);
            });
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            CO::for_each(node, hdr, [&](NK s, const VST& v) {
                cb(s, v);
            });
        }
        return;
    }

    // Bitmask node: recurse into children in sorted order
    const uint64_t* node = bm_to_node_const(tagged);
    const auto* hdr = get_header(node);
    uint8_t sc = hdr->skip();

    if constexpr (BITS > U8_BITS) {
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
        using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
        constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](uint8_t idx, int slot) {
            walk_entries_in_order<BITS - CHAR_BIT>(rch[slot],
                [&](CNK child_suffix, VST v) {
                    NK full = (NK(idx) << (NK_BITS - CHAR_BIT))
                        | static_cast<NK>(
                            static_cast<uint64_t>(child_suffix)
                            << (U64_BITS - CNK_BITS)
                            >> (U64_BITS - NK_BITS + CHAR_BIT));
                    cb(full, v);
                });
        });
    }
}
```

#### D3. do_coalesce

OLD:
```cpp
template<int BITS> requires (BITS >= U8_BITS)
static erase_result_t do_coalesce(uint64_t* node, node_header_t* hdr,
                                    BLD& bld) {
    uint8_t sc = hdr->skip();

    auto c = collect_bm_final<BITS>(node, sc);

    uint64_t* leaf = build_leaf<BITS>(c.keys.get(), c.vals.get(), c.count,
                                       c.rep_key, bld);

    if (sc > 0) [[unlikely]] {
        leaf = prepend_skip<BITS>(leaf, sc, c.rep_key, bld);
    }

    dealloc_coalesced_node<BITS>(node, sc, bld);
    return {tag_leaf(leaf), true, c.count};
}
```

NEW:
```cpp
template<int BITS> requires (BITS >= U8_BITS)
static erase_result_t do_coalesce(uint64_t* node, node_header_t* hdr,
                                    BLD& bld) {
    using NK = nk_for_bits_t<BITS>;
    uint8_t sc = hdr->skip();
    uint64_t total_entries = BO::chain_descendants(node, sc, hdr->entries());

    // Representative key for prefix/fn-ptr setup
    uint64_t rep_key = first_descendant_prefix(
        tag_bitmask(node), (KEY_BITS - BITS) / CHAR_BIT);

    // Allocate destination leaf and write directly into it
    uint64_t* leaf;
    if constexpr (sizeof(NK) == sizeof(uint8_t)) {
        // Bitmap leaf — stack arrays, max BYTE_VALUES entries
        uint8_t byte_keys[BYTE_VALUES];
        VST vals_arr[BYTE_VALUES];
        size_t wi = 0;
        walk_entries_in_order<BITS>(tag_bitmask(node), [&](NK s, VST v) {
            byte_keys[wi] = static_cast<uint8_t>(s);
            vals_arr[wi] = v;
            wi++;
        });
        leaf = BO::make_bitmap_leaf(byte_keys, vals_arr,
            static_cast<uint32_t>(wi), bld);
    } else {
        // Compact leaf — allocate and write directly, zero intermediate arrays
        using CO = compact_ops<NK, VALUE, ALLOC>;
        constexpr size_t hu = LEAF_HEADER_U64;
        size_t total_u64 = round_up_u64(CO::size_u64(total_entries, hu));
        leaf = bld.alloc_node(total_u64, false);
        auto* lh = get_header(leaf);
        lh->set_entries(static_cast<uint16_t>(total_entries));
        CO::set_val_offset(leaf, total_u64);

        NK* dk = CO::keys(leaf, hu);
        VST* dv = CO::vals_mut(leaf);
        size_t wi = 0;
        walk_entries_in_order<BITS>(tag_bitmask(node), [&](NK s, VST v) {
            dk[wi] = s;
            VT::init_slot(&dv[wi], v);
            wi++;
        });
    }

    init_leaf_fns<BITS>(leaf, rep_key);

    if (sc > 0) [[unlikely]] {
        leaf = prepend_skip<BITS>(leaf, sc, rep_key, bld);
    }

    dealloc_coalesced_node<BITS>(node, sc, bld);
    return {tag_leaf(leaf), true, static_cast<uint64_t>(total_entries)};
}
```

Note: bitmap leaf case uses stack arrays (max BYTE_VALUES = 256 entries).
Compact leaf case writes directly — zero intermediate allocation.

#### D4. Dead code after #3

Remove these functions (no longer called):
- `collected_t` (struct)
- `collected_typed_t` (template struct)
- `collect_entries`
- `collect_leaf`
- `collect_leaf_skip`
- `collect_bm_skip`
- `collect_bm_final`

---

## Priority

1. **#1+#4** — hot path, every in-place insert/erase
2. **#2** — cold but frequent in sequential bulk insert
3. **#3** — cold, lowest priority
