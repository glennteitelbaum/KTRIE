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
};

// ==========================================================================
// compact_ops  -- compact leaf operations templated on K type
//
// Layout: [header (LEAF_HEADER_U64)][sorted_keys (aligned)][values (aligned)]
//
// Entry count equals the number of keys stored. No dup padding.
// Allocation uses round_up_u64 size classes for amortized headroom.
//
// K = suffix type (uint16_t, uint32_t, uint64_t)
// ==========================================================================

template<typename K, typename VALUE, typename ALLOC>
struct compact_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    // Suffix type constants for key width dispatch
    static constexpr uint8_t STYPE_U16 = 1;
    static constexpr uint8_t STYPE_U32 = 2;
    static constexpr uint8_t STYPE_U64 = 3;

    static constexpr uint8_t STYPE =
        (sizeof(K) == sizeof(uint16_t)) ? STYPE_U16 :
        (sizeof(K) == sizeof(uint32_t)) ? STYPE_U32 : STYPE_U64;

    // --- exact u64 size for a given entry count ---

    static constexpr size_t size_u64(size_t count, size_t hu = LEAF_HEADER_U64) noexcept {
        size_t kb = align_up(count * sizeof(K), U64_BYTES);
        size_t vb;
        if constexpr (VT::IS_BOOL)
            vb = bool_slots::bytes_for(count);
        else
            vb = align_up(count * sizeof(VST), U64_BYTES);
        return hu + (kb + vb) / U64_BYTES;
    }

    // --- capacity: max entries that fit in an allocation ---

    static constexpr bool has_room(unsigned entries, unsigned alloc_u64) noexcept {
        return size_u64(entries + 1) <= alloc_u64;
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

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

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot)
    // ==================================================================

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

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

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

    // Free node memory only — values have been transferred elsewhere
    static void dealloc_node_only(uint64_t* node, BLD& bld) {
        bld.dealloc_node(node, get_header(node)->alloc_u64());
    }

    // ==================================================================
    // Insert
    //
    // INSERT: allow inserting new keys
    // ASSIGN: allow overwriting existing values
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t insert(uint64_t* node, node_header_t* h,
                                  K suffix, VST value, BLD& bld) {
        unsigned entries = h->entries();
        
        size_t hs = LEAF_HEADER_U64;
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(kd, entries, suffix);

        // Key exists
        if (*base == suffix) [[unlikely]] {
            unsigned idx = static_cast<unsigned>(base - kd);
            if constexpr (ASSIGN) {
                if constexpr (VT::IS_BOOL) {
                    bool_vals_mut(node, entries, hs).set(idx, value);
                } else {
                    VST* vd = vals_mut(node, entries, hs);
                    bld.destroy_value(vd[idx]);
                    VT::init_slot(&vd[idx], value);
                }
            }
            return {tag_leaf(node), false, false, nullptr};
        }
        if constexpr (!INSERT) return {tag_leaf(node), false, false};

        if (entries >= COMPACT_MAX) [[unlikely]]
            return {tag_leaf(node), false, true};  // needs_split

        int ins = static_cast<int>(base - kd) + (*base < suffix);

        // Grow if no room
        if (!has_room(entries, h->alloc_u64())) [[unlikely]] {
            unsigned new_entries = entries + 1;
            size_t au64 = round_up_u64(size_u64(new_entries * GROW_NUMER / GROW_DENOM, hs));
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
            return {tag_leaf(nn), true, false};
        }

        // In-place: memmove tail right by 1
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
            VST* vd = vals_mut(node, entries, hs);
            if (tail > 0) {
                std::memmove(kd + ins + 1, kd + ins, tail * sizeof(K));
                std::memmove(vd + ins + 1, vd + ins, tail * sizeof(VST));
            }
            kd[ins] = suffix;
            VT::init_slot(&vd[ins], value);
        }
        h->set_entries(entries + 1);
        return {tag_leaf(node), true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header_t* h,
                                K suffix, BLD& bld) {
        unsigned entries = h->entries();
        
        size_t hs = LEAF_HEADER_U64;
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(kd, entries, suffix);
        if (*base != suffix) [[unlikely]] return {tag_leaf(node), false, 0};
        unsigned idx = static_cast<unsigned>(base - kd);

        unsigned nc = entries - 1;

        // Last entry
        if (nc == 0) [[unlikely]] {
            destroy_and_dealloc(node, bld);
            return {0, true, 0};
        }

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

        // In-place: memmove tail left by 1
        unsigned tail = entries - idx - 1;
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node, entries, hs);
            if (tail > 0) {
                std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
                bv.shift_left_1(idx + 1, tail);
            }
        } else {
            VST* vd = vals_mut(node, entries, hs);
            if constexpr (VT::HAS_DESTRUCTOR)
                bld.destroy_value(vd[idx]);
            if (tail > 0) {
                std::memmove(kd + idx, kd + idx + 1, tail * sizeof(K));
                std::memmove(vd + idx, vd + idx + 1, tail * sizeof(VST));
            }
        }
        h->set_entries(nc);
        return {tag_leaf(node), true, nc};
    }

    // ==================================================================
    // Conditional erase: find key, test predicate, erase only if true.
    // Returns {tagged, erased, remaining_entries}.
    // ==================================================================

private:
    // ==================================================================
    // Layout helpers
    // ==================================================================

    static K* keys(uint64_t* node, size_t header_size) noexcept {
        return reinterpret_cast<K*>(node + header_size);
    }
    static const K* keys(const uint64_t* node, size_t header_size) noexcept {
        return reinterpret_cast<const K*>(node + header_size);
    }

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
    // ==================================================================
    // Value pointer helper — return const VALUE* at position
    // ==================================================================
public:

    // ==================================================================
    // find_fn — exact match → leaf_pos_t
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_fn(uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            if (!skip_eq(leaf_prefix(node), d, ik)) [[unlikely]]
                return {};
        }

        K suffix = static_cast<K>(d.suffix(ik));
        const K* kd = keys(node, hs);
        unsigned entries = get_header(node)->entries();

        const K* base = adaptive_search<K>::find_base(kd, entries, suffix);
        uint16_t pos = static_cast<uint16_t>(base - kd);

        if (kd[pos] != suffix) [[unlikely]] return {};
        return {node, pos, true};
    }

    // ==================================================================
    // find_next_fn — first entry with key > ik → leaf_pos_t
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_next_fn(uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(leaf_prefix(node), d, ik);
            if (cmp > 0) return {};
            if (cmp < 0) return {node, 0, true};
        }

        K suffix = static_cast<K>(d.suffix(ik));
        if (suffix >= static_cast<K>(d.nk_max())) [[unlikely]]
            return {};

        K target = suffix + 1;
        const K* kd = keys(node, hs);
        unsigned entries = get_header(node)->entries();

        const K* base = adaptive_search<K>::find_base_first(kd, entries, target);
        uint16_t pos = static_cast<uint16_t>(base - kd);
        if (kd[pos] < target) [[unlikely]] {
            ++pos;
            if (pos >= entries) [[unlikely]] return {};
        }
        return {node, pos, true};
    }

    // ==================================================================
    // find_ge_fn — first entry with key >= ik → leaf_pos_t
    // Used by lower_bound at leaf level.
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_ge_fn(uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(leaf_prefix(node), d, ik);
            if (cmp > 0) return {};
            if (cmp < 0) return {node, 0, true};
        }

        K suffix = static_cast<K>(d.suffix(ik));
        const K* kd = keys(node, hs);
        unsigned entries = get_header(node)->entries();

        const K* base = adaptive_search<K>::find_base_first(kd, entries, suffix);
        uint16_t pos = static_cast<uint16_t>(base - kd);
        if (kd[pos] < suffix) {
            ++pos;
            if (pos >= entries) return {};
        }
        return {node, pos, true};
    }

    // ==================================================================
    // find_prev_fn — last entry with key < ik → leaf_pos_t
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_prev_fn(uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(leaf_prefix(node), d, ik);
            if (cmp < 0) return {};
            if (cmp > 0) {
                unsigned entries = get_header(node)->entries();
                return {node, static_cast<uint16_t>(entries - 1), true};
            }
        }

        K suffix = static_cast<K>(d.suffix(ik));
        if (suffix == 0) [[unlikely]] return {};

        K target = suffix - 1;
        const K* kd = keys(node, hs);
        unsigned entries = get_header(node)->entries();

        const K* base = adaptive_search<K>::find_base(kd, entries, target);
        uint16_t pos = static_cast<uint16_t>(base - kd);
        if (kd[pos] > target) [[unlikely]] return {};
        return {node, pos, true};
    }

    // ==================================================================
    // find_first_fn — minimum entry → leaf_pos_t
    // ==================================================================

    static leaf_pos_t find_first_fn(uint64_t* node) noexcept {
        unsigned entries = get_header(node)->entries();
        if (entries == 0) [[unlikely]] return {};
        return {node, 0, true};
    }

    // ==================================================================
    // find_last_fn — maximum entry → leaf_pos_t
    // ==================================================================

    static leaf_pos_t find_last_fn(uint64_t* node) noexcept {
        unsigned entries = get_header(node)->entries();
        if (entries == 0) [[unlikely]] return {};
        return {node, static_cast<uint16_t>(entries - 1), true};
    }

    // ==================================================================
    // Tables + set_leaf_fns
    // 2-entry tables indexed by DO_SKIP only.
    // ==================================================================

    static inline const find_fn_t FIND_TABLE[2] = {
        &find_fn<false>,  &find_fn<true>,
    };
    static inline const iter_fn_t NEXT_TABLE[2] = {
        &find_next_fn<false>,  &find_next_fn<true>,
    };
    static inline const iter_fn_t PREV_TABLE[2] = {
        &find_prev_fn<false>,  &find_prev_fn<true>,
    };

    static void set_leaf_fns(uint64_t* node, bool has_skip) noexcept {
        unsigned idx = has_skip ? 1u : 0u;
        set_find_fn(node, FIND_TABLE[idx]);
        set_find_next(node, NEXT_TABLE[idx]);
        set_find_prev(node, PREV_TABLE[idx]);
        set_find_first(node, &find_first_fn);
        set_find_last(node, &find_last_fn);
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_COMPACT_HPP
