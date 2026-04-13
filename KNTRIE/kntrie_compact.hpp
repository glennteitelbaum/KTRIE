#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gteitelbaum::kntrie_detail {

// ==========================================================================
// AdaptiveSearch  (branchless binary search for any entry count)
//
// Do not condense this code. The separate variables and = assignments
// (not +=) are deliberate. Eliminating temporaries or switching to +=
// produces worse codegen. The compiler is sensitive to the exact
// expression structure.
//
// Precondition: count > 0.
// ==========================================================================

template<typename K>
struct adaptive_search {
    static const K* find_base(const K* base, unsigned count, K key) noexcept {
        int bw=std::bit_width((count - 1) | 1u);
        unsigned count2=1u << (bw-1);
        unsigned diff=count - count2;
        const K* diff_val=base+diff;
        bool is_diff = *diff_val <= key;
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
        int bw=std::bit_width((count - 1) | 1u);
        unsigned count2=1u << (bw-1);
        unsigned diff=count - count2;
        const K* diff_val=base+diff;
        bool is_diff = *diff_val < key;
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
};

// ==========================================================================
// compact_ops  -- compact leaf operations templated on K (stored key type)
//
// v2 layout: [header (COMPACT_HEADER_U64)][sorted_keys][values]
//
// K = stored key type (std::make_unsigned_t<KEY>, sign-flipped).
// Full keys stored — no narrowing, no suffix extraction.
// Binary search uses operator< on K directly.
// ==========================================================================

template<typename K, typename VALUE, typename ALLOC>
struct compact_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    static constexpr std::size_t HU = COMPACT_HEADER_U64;

    // --- exact u64 size for a given entry count ---

    static constexpr std::size_t size_u64(std::size_t count, std::size_t hu = HU) noexcept {
        std::size_t kb = align_up(count * sizeof(K), U64_BYTES);
        std::size_t vb;
        if constexpr (VT::IS_BOOL)
            vb = bool_slots::bytes_for(count);
        else
            vb = align_up(count * sizeof(VST), U64_BYTES);
        return hu + (kb + vb) / U64_BYTES;
    }

    // --- val_offset_u64: u64 position where values begin, from capacity ---
    static constexpr std::size_t val_offset_u64(std::size_t cap) noexcept {
        return HU + align_up(cap * sizeof(K), U64_BYTES) / U64_BYTES;
    }

    static bool has_room(unsigned entries, const node_header_t* h) noexcept {
        return entries + 1 <= h->alloc_u64();
    }

    static constexpr std::size_t val_block_u64(std::size_t count) noexcept {
        if constexpr (VT::IS_BOOL)
            return bool_slots::u64_for(count);
        else
            return align_up(count * sizeof(VST), U64_BYTES) / U64_BYTES;
    }

    static constexpr std::size_t capacity_for(std::size_t total_u64) noexcept {
        std::size_t lo = 0;
        std::size_t hi = (total_u64 - HU) * U64_BYTES / sizeof(K);
        while (lo < hi) {
            std::size_t mid = lo + (hi - lo + 1) / 2;
            if (size_u64(mid) <= total_u64) lo = mid; else hi = mid - 1;
        }
        return lo;
    }

    static std::size_t alloc_total_u64(std::size_t cap) noexcept {
        return round_up_u64(size_u64(cap));
    }

    static void set_capacity(std::uint64_t* node, std::size_t total_u64) noexcept {
        std::size_t cap = capacity_for(total_u64);
        get_header(node)->set_alloc_u64(static_cast<std::uint16_t>(cap));
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static std::uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               unsigned count, BLD& bld) {
        std::size_t total = round_up_u64(size_u64(count, HU));
        std::uint64_t* node = bld.alloc_node(total, false);
        auto* h = get_header(node);
        h->set_entries(count);
        set_capacity(node, total);
        if (count > 0) {
            std::memcpy(keys(node, HU), sorted_keys, count * sizeof(K));
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

    // ==================================================================
    // Iterate entries: cb(K key, VST value_slot)
    // ==================================================================

    template<typename Fn>
    static void for_each(const std::uint64_t* node, const node_header_t* h, Fn&& cb) {
        unsigned entries = h->entries();
        const K* kd = keys(node, HU);
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

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    static void destroy_and_dealloc(std::uint64_t* node, BLD& bld) {
        auto* h = get_header(node);
        if constexpr (VT::HAS_DESTRUCTOR) {
            VST* vd = vals_mut(node);
            unsigned entries = h->entries();
            for (unsigned i = 0; i < entries; ++i)
                bld.destroy_value(vd[i]);
        }
        bld.dealloc_node(node, alloc_total_u64(h->alloc_u64()));
    }

    static void dealloc_node_only(std::uint64_t* node, BLD& bld) {
        bld.dealloc_node(node, alloc_total_u64(get_header(node)->alloc_u64()));
    }

    // ==================================================================
    // Insert
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t insert(std::uint64_t* node, node_header_t* h,
                                  K stored, VST value, BLD& bld) {
        unsigned entries = h->entries();
        K* kd = keys(node, HU);

        const K* base = adaptive_search<K>::find_base(kd, entries, stored);

        // Key exists
        if (*base == stored) [[unlikely]] {
            unsigned idx = static_cast<unsigned>(base - kd);
            if constexpr (ASSIGN) {
                if constexpr (VT::IS_BOOL) {
                    bool_vals_mut(node).set(idx, value);
                } else {
                    VST* vd = vals_mut(node);
                    bld.destroy_value(vd[idx]);
                    VT::init_slot(&vd[idx], value);
                }
            }
            return {tag_leaf(node), false, false, nullptr,
                    node, static_cast<std::uint16_t>(idx)};
        }
        if constexpr (!INSERT) return {tag_leaf(node), false, false};

        if (entries >= COMPACT_MAX) [[unlikely]]
            return {tag_leaf(node), false, true};  // needs_split

        int ins = static_cast<int>(base - kd) + (*base < stored);

        // Grow if no room
        if (!has_room(entries, h)) [[unlikely]] {
            unsigned new_entries = entries + 1;
            std::size_t total = round_up_u64(size_u64(new_entries, HU));
            std::uint64_t* nn = bld.alloc_node(total, false);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn, HU);
            nh->set_entries(new_entries);
            set_capacity(nn, total);

            K* nk = keys(nn, HU);
            std::memcpy(nk, kd, ins * sizeof(K));
            nk[ins] = stored;
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

            bld.dealloc_node(node, alloc_total_u64(h->alloc_u64()));
            return {tag_leaf(nn), true, false, nullptr,
                    nn, static_cast<std::uint16_t>(ins)};
        }

        int tail = entries - ins;
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node);
            std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
            bv.shift_right_1(ins, tail);
            kd[ins] = stored;
            bv.set(ins, value);
        } else {
            VST* vd = vals_mut(node);
            std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
            std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
            kd[ins] = stored;
            VT::init_slot(&vd[ins], value);
        }
        h->set_entries(entries + 1);
        return {tag_leaf(node), true, false, nullptr,
                node, static_cast<std::uint16_t>(ins)};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t<K> erase(std::uint64_t* node, node_header_t* h,
                                K stored, BLD& bld) {
        unsigned entries = h->entries();
        K* kd = keys(node, HU);

        const K* base = adaptive_search<K>::find_base(kd, entries, stored);
        if (*base != stored) [[unlikely]] return {tag_leaf(node), false, 0, {}};
        unsigned idx = static_cast<unsigned>(base - kd);

        unsigned nc = entries - 1;

        // Last entry
        if (nc == 0) [[unlikely]] {
            destroy_and_dealloc(node, bld);
            return {0, true, 0, {}};
        }

        // Shrink check
        std::size_t needed_u64 = size_u64(nc, HU);
        std::size_t old_cap = h->alloc_u64();
        std::size_t current_total = alloc_total_u64(old_cap);
        if (should_shrink_u64(current_total, needed_u64)) [[unlikely]] {
            std::size_t total = round_up_u64(needed_u64);
            std::uint64_t* nn = bld.alloc_node(total, false);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn, HU);
            nh->set_entries(nc);
            set_capacity(nn, total);

            K* nk = keys(nn, HU);
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
            iter_entry_t<K> nx = (idx < nc) ? entry_at_pos(nn, static_cast<std::uint16_t>(idx))
                                            : iter_entry_t<K>{};
            return {tag_leaf(nn), true, nc, nx};
        }

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
        h->set_entries(nc);
        iter_entry_t<K> nx = (idx < nc) ? entry_at_pos(node, static_cast<std::uint16_t>(idx))
                                        : iter_entry_t<K>{};
        return {tag_leaf(node), true, nc, nx};
    }

    // ==================================================================
    // Layout helpers
    // ==================================================================

    static K* keys(std::uint64_t* node, std::size_t header_size) noexcept {
        return reinterpret_cast<K*>(node + header_size);
    }
    static const K* keys(const std::uint64_t* node, std::size_t header_size) noexcept {
        return reinterpret_cast<const K*>(node + header_size);
    }

    static VST* vals_mut(std::uint64_t* node) noexcept {
        std::size_t cap = get_header(node)->alloc_u64();
        return reinterpret_cast<VST*>(node + val_offset_u64(cap));
    }
    static const VST* vals(const std::uint64_t* node) noexcept {
        std::size_t cap = get_header(node)->alloc_u64();
        return reinterpret_cast<const VST*>(node + val_offset_u64(cap));
    }

    static bool_slots bool_vals_mut(std::uint64_t* node) noexcept {
        std::size_t cap = get_header(node)->alloc_u64();
        return bool_slots{ reinterpret_cast<std::uint64_t*>(
            node + val_offset_u64(cap)) };
    }
    static bool_slots bool_vals(const std::uint64_t* node) noexcept {
        std::size_t cap = get_header(node)->alloc_u64();
        return bool_slots{ const_cast<std::uint64_t*>(reinterpret_cast<const std::uint64_t*>(
            node + val_offset_u64(cap))) };
    }

    // ==================================================================
    // Value pointer + entry_at_pos
    // ==================================================================

    static void* val_ptr(std::uint64_t* node, std::uint16_t pos) noexcept {
        if constexpr (VT::IS_BOOL)
            return bool_vals_mut(node).data;
        else
            return &vals_mut(node)[pos];
    }

    // Build iter_entry_t at a given position.
    // v2: no depth_t, no prefix reconstruction — just read the stored key.
    static iter_entry_t<K> entry_at_pos(std::uint64_t* node, std::uint16_t pos) noexcept {
        const K* kd = keys(node, HU);
        return {node, pos, 0, kd[pos], val_ptr(node, pos), true};
    }

    // ==================================================================
    // compact_find — binary search on full stored K.
    // No skip check — compact nodes store full keys.
    // Called directly via is_bitmap() dispatch, not via function pointer.
    // ==================================================================

    static iter_entry_t<K> compact_find(std::uint64_t* node, const node_header_t* hdr,
                                        K stored) noexcept {
        const K* kd = keys(node, HU);
        unsigned entries = hdr->entries();

        const K* base = adaptive_search<K>::find_base(kd, entries, stored);
        std::uint16_t pos = static_cast<std::uint16_t>(base - kd);

        if (kd[pos] != stored) [[unlikely]] return {};
        return {node, pos, 0, kd[pos], val_ptr(node, pos), true};
    }

    // ==================================================================
    // compact_advance — positional advance for iteration.
    // FWD: pos+1.  BWD: pos-1.  O(1).
    // ==================================================================

    static iter_entry_t<K> compact_advance(std::uint64_t* node, std::uint16_t pos,
                                           void* val, dir_t dir) noexcept {
        unsigned entries = get_header(node)->entries();
        int d_int = static_cast<int>(dir);
        std::uint16_t next = static_cast<std::uint16_t>(static_cast<int>(pos) + d_int);
        if (dir == dir_t::FWD) {
            if (next >= entries) return {};
        } else {
            if (pos == 0) return {};
        }
        const K* kd = keys(node, HU);
        void* new_val;
        if constexpr (VT::IS_BOOL)
            new_val = val;  // packed bits base doesn't move
        else
            new_val = static_cast<char*>(val) + d_int * static_cast<int>(sizeof(VST));
        return {node, next, 0, kd[next], new_val, true};
    }

    // ==================================================================
    // compact_edge — first (FWD) or last (BWD) entry.
    // ==================================================================

    static iter_entry_t<K> compact_edge(std::uint64_t* node, dir_t dir) noexcept {
        unsigned entries = get_header(node)->entries();
        const K* kd = keys(node, HU);
        std::uint16_t pos = (dir == dir_t::FWD) ? 0
            : static_cast<std::uint16_t>(entries - 1);
        return {node, pos, 0, kd[pos], val_ptr(node, pos), true};
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_COMPACT_HPP
