#ifndef KNTRIE_COMPACT_HPP
#define KNTRIE_COMPACT_HPP

#include "kntrie_support.hpp"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace gteitelbaum {

// ==========================================================================
// AdaptiveSearch  (branchless binary search for pow2 and 3/4 midpoint counts)
//
// Every compact leaf has power-of-2 or 3/4 midpoint total slots, so the search is a
// pure halving loop — no alignment preamble, no branches.
// ==========================================================================

template<typename K>
struct adaptive_search {
    // Pure cmov loop — returns pointer to candidate.
    // Caller checks *result == key.
    // count must be power of 2.
    static const K* find_base(const K* base, unsigned count, K key) noexcept {
        do {
            count >>= 1;
            base += (base[count] <= key) ? count : 0;
        } while (count > 1);
        return base;
    }

    // Lower bound: finds first occurrence of key (or first element > key).
    // count must be power of 2.
    static const K* find_base_first(const K* base, unsigned count, K key) noexcept {
        do {
            count >>= 1;
            base += (base[count] < key) ? count : 0;
        } while (count > 1);
        return base;
    }
};

// ==========================================================================
// compact_ops  -- compact leaf operations templated on K type
//
// Layout: [header (LEAF_HEADER_U64)][sorted_keys (aligned)][values (aligned)]
//
// Slot count is always power-of-2. Extra slots are
// filled with evenly-spaced duplicates of neighboring keys.
// Insert consumes the nearest dup; erase creates a new dup.
//
// K = suffix type (uint16_t, uint32_t, uint64_t)
// ==========================================================================

template<typename K, typename VALUE, typename ALLOC>
struct compact_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    // Suffix type constant for this K
    static constexpr uint8_t STYPE =
        (sizeof(K) == sizeof(uint16_t)) ? 1 :
        (sizeof(K) == sizeof(uint32_t)) ? 2 : 3;

    // --- slot count: next power of 2 ---

    static constexpr uint16_t slots_for(unsigned entries) noexcept {
        unsigned p = std::max(2u, std::bit_ceil(entries));
        return static_cast<uint16_t>(std::min(p, unsigned(COMPACT_MAX)));
    }

    // --- exact u64 size for a given slot count ---

    static constexpr size_t size_u64(size_t slots, size_t hu = LEAF_HEADER_U64) noexcept {
        size_t kb = align_up(slots * sizeof(K), U64_BYTES);
        size_t vb;
        if constexpr (VT::IS_BOOL)
            vb = bool_slots::bytes_for(slots);
        else
            vb = align_up(slots * sizeof(VST), U64_BYTES);
        return hu + (kb + vb) / U64_BYTES;
    }

    // ==================================================================
    // Factory: build from pre-sorted working arrays
    // ==================================================================

    static uint64_t* make_leaf(const K* sorted_keys, const VST* values,
                               unsigned count, BLD& bld) {
        uint16_t ts = slots_for(count);
        constexpr size_t hu = LEAF_HEADER_U64;
        size_t au64 = size_u64(ts, hu);
        uint64_t* node = bld.alloc_node(au64, false);
        auto* h = get_header(node);
        h->set_entries(count);
        h->set_alloc_u64(au64);
        h->set_total_slots(ts);

        if (count > 0) {
            if constexpr (VT::IS_BOOL) {
                bool tmp_v[ts];
                seed_from_real(keys(node, hu), tmp_v,
                               sorted_keys, values, count, ts);
                bool_vals_mut(node, ts, hu).pack_from(tmp_v, ts);
            } else {
                seed_from_real(keys(node, hu), vals_mut(node, ts, hu),
                               sorted_keys, values, count, ts);
            }
        }
        return node;
    }

    // ==================================================================
    // Iterate entries: cb(K suffix, VST value_slot) -- skips dups
    // ==================================================================

    template<typename Fn>
    static void for_each(const uint64_t* node, const node_header_t* h, Fn&& cb) {
        unsigned ts = h->total_slots();
        size_t hs = LEAF_HEADER_U64;
        const K* kd = keys(node, hs);
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals(node, ts, hs);
            cb(kd[0], bv.get(0));
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                cb(kd[i], bv.get(i));
            }
        } else {
            const VST* vd = vals(node, ts, hs);
            cb(kd[0], vd[0]);
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                cb(kd[i], vd[i]);
            }
        }
    }

    // ==================================================================
    // Destroy all values + deallocate node
    // ==================================================================

    static void destroy_and_dealloc(uint64_t* node, BLD& bld) {
        auto* h = get_header(node);
        if constexpr (VT::HAS_DESTRUCTOR) {
            // C-type (pointer): dups share pointers — destroy unique only
            unsigned ts = h->total_slots();
            size_t hs = LEAF_HEADER_U64;
            const K* kd = keys(node, hs);
            VST* vd = vals_mut(node, ts, hs);
            bld.destroy_value(vd[0]);
            for (unsigned i = 1; i < ts; ++i) {
                if (kd[i] == kd[i - 1]) continue;
                bld.destroy_value(vd[i]);
            }
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
        unsigned ts = h->total_slots();
        size_t hs = LEAF_HEADER_U64;
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);

        // Key exists
        if (*base == suffix) [[unlikely]] {
            if constexpr (ASSIGN) {
                int idx = base - kd;
                const K* fb = adaptive_search<K>::find_base_first(kd, ts, suffix);
                int first = static_cast<int>(fb - kd) + (*fb != suffix);
                if constexpr (VT::IS_BOOL) {
                    auto bv = bool_vals_mut(node, ts, hs);
                    for (int i = first; i <= idx; ++i)
                        bv.set(i, value);
                } else {
                    VST* vd = vals_mut(node, ts, hs);
                    bld.destroy_value(vd[idx]);
                    VT::init_slot(&vd[idx], value);
                    for (int i = first; i < idx; ++i)
                        VT::write_slot(&vd[i], value);
                }
            }
            return {tag_leaf(node), false, false,
                    val_ptr_at(node, ts, hs, static_cast<unsigned>(base - kd))};
        }
        if constexpr (!INSERT) return {tag_leaf(node), false, false};

        if (entries >= COMPACT_MAX) [[unlikely]]
            return {tag_leaf(node), false, true};  // needs_split

        int ins = (base - kd) + (*base < suffix);
        unsigned dups = ts - entries;

        // Dups available: consume one in-place
        if (dups > 0) [[likely]] {
            if constexpr (VT::IS_BOOL) {
                auto bv = bool_vals_mut(node, ts, hs);
                // Find dup and shift keys (keys are always memmove'd)
                int dup_pos = find_dup_pos(kd, ts, ins, entries);
                int write_pos;
                if (dup_pos < ins) {
                    int sc = ins - 1 - dup_pos;
                    if (sc > 0) {
                        std::memmove(kd + dup_pos, kd + dup_pos + 1, sc * sizeof(K));
                        bv.shift_left_1(dup_pos + 1, sc);
                    }
                    write_pos = ins - 1;
                } else {
                    int sc = dup_pos - ins;
                    if (sc > 0) {
                        std::memmove(kd + ins + 1, kd + ins, sc * sizeof(K));
                        bv.shift_right_1(ins, sc);
                    }
                    write_pos = ins;
                }
                kd[write_pos] = suffix;
                bv.set(write_pos, value);
            } else {
                VST* vd = vals_mut(node, ts, hs);
                insert_consume_dup(kd, vd, ts, ins, entries, suffix, value);
            }
            h->set_entries(entries + 1);
            return {tag_leaf(node), true, false};
        }

        // No dups: realloc to next slot count
        unsigned new_entries = entries + 1;
        uint16_t new_ts = slots_for(new_entries);
        size_t au64 = size_u64(new_ts, hs);
        uint64_t* nn = bld.alloc_node(au64, false);
        auto* nh = get_header(nn);
        copy_leaf_header(node, nn);
        nh->set_entries(new_entries);
        nh->set_alloc_u64(au64);
        nh->set_total_slots(new_ts);
        set_leaf_fns(nn, new_ts, get_header(nn)->skip() > 0);  // TS changed — update fn pointers

        if constexpr (VT::IS_BOOL) {
            bool old_v[ts];
            bool_vals(node, ts, hs).unpack_to(old_v, ts);
            bool new_v[new_ts];
            seed_with_insert(keys(nn, hs), new_v, kd, old_v, ts, entries,
                              suffix, value, new_entries, new_ts);
            bool_vals_mut(nn, new_ts, hs).pack_from(new_v, new_ts);
        } else {
            VST* vd = vals_mut(node, ts, hs);
            seed_with_insert(keys(nn, hs), vals_mut(nn, new_ts, hs),
                              kd, vd, ts, entries,
                              suffix, value, new_entries, new_ts);
        }

        bld.dealloc_node(node, h->alloc_u64());
        return {tag_leaf(nn), true, false};
    }

    // ==================================================================
    // Erase
    // ==================================================================

    static erase_result_t erase(uint64_t* node, node_header_t* h,
                                K suffix, BLD& bld) {
        unsigned entries = h->entries();
        unsigned ts = h->total_slots();
        size_t hs = LEAF_HEADER_U64;
        K*   kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(
            kd, ts, suffix);
        if (*base != suffix) [[unlikely]] return {tag_leaf(node), false, 0};
        unsigned idx = static_cast<unsigned>(base - kd);

        unsigned nc = entries - 1;

        // Last entry
        if (nc == 0) [[unlikely]] {
            destroy_and_dealloc(node, bld);
            return {0, true, 0};
        }

        // Shrink check: if entries-1 fits in half the slots, realloc
        uint16_t new_ts = slots_for(nc);
        if (new_ts < ts) [[unlikely]] {
            // Realloc + re-seed at smaller slot count
            size_t au64 = size_u64(new_ts, hs);
            uint64_t* nn = bld.alloc_node(au64, false);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn);
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);
            nh->set_total_slots(new_ts);
            set_leaf_fns(nn, new_ts, get_header(nn)->skip() > 0);  // TS changed — update fn pointers

            auto tmp_k = std::make_unique<K[]>(nc);
            auto tmp_v = std::make_unique<VST[]>(nc);

            if constexpr (VT::IS_BOOL) {
                bool old_v[ts];
                bool_vals(node, ts, hs).unpack_to(old_v, ts);
                dedup_skip_into(kd, old_v, ts, suffix, tmp_k.get(), tmp_v.get(), bld);
                bool new_v[new_ts];
                seed_from_real(keys(nn, hs), new_v,
                               tmp_k.get(), tmp_v.get(), nc, new_ts);
                bool_vals_mut(nn, new_ts, hs).pack_from(new_v, new_ts);
            } else {
                VST* vd = vals_mut(node, ts, hs);
                dedup_skip_into(kd, vd, ts, suffix, tmp_k.get(), tmp_v.get(), bld);
                seed_from_real(keys(nn, hs), vals_mut(nn, new_ts, hs),
                               tmp_k.get(), tmp_v.get(), nc, new_ts);
            }

            bld.dealloc_node(node, h->alloc_u64());
            return {tag_leaf(nn), true, nc};
        }

        // In-place: convert erased entry's run to neighbor dups
        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node, ts, hs);
            const K* fb = adaptive_search<K>::find_base_first(kd, ts, suffix);
            int first = static_cast<int>(fb - kd) + (*fb != suffix);

            bool neighbor_val;
            K neighbor_key;
            if (first > 0) {
                neighbor_key = kd[first - 1];
                neighbor_val = bv.get(first - 1);
            } else {
                neighbor_key = kd[idx + 1];
                neighbor_val = bv.get(idx + 1);
            }
            for (int i = first; i <= (int)idx; ++i) {
                kd[i] = neighbor_key;
                bv.set(i, neighbor_val);
            }
        } else {
            VST* vd = vals_mut(node, ts, hs);
            erase_create_dup(kd, vd, ts, idx, suffix, bld);
        }
        h->set_entries(nc);
        return {tag_leaf(node), true, nc};
    }

    // ==================================================================
    // Modify existing value in-place with dup propagation
    //
    // Single-walk read-modify-write: finds the key, applies fn(old_val)
    // to get new_val, writes it to the primary slot and all dup copies.
    // Returns true if key was found and modified, false if not found.
    // ==================================================================

    template<typename F>
    static bool modify_existing(uint64_t* node, node_header_t* h,
                                K suffix, F&& fn, BLD& bld) {
        unsigned ts = h->total_slots();
        constexpr size_t hs = LEAF_HEADER_U64;
        K* kd = keys(node, hs);

        const K* base = adaptive_search<K>::find_base(kd, ts, suffix);
        if (*base != suffix) return false;

        int idx = static_cast<int>(base - kd);
        const K* fb = adaptive_search<K>::find_base_first(kd, ts, suffix);
        int first = static_cast<int>(fb - kd) + (*fb != suffix);

        if constexpr (VT::IS_BOOL) {
            auto bv = bool_vals_mut(node, ts, hs);
            VST new_val = fn(bv.get(idx));
            for (int i = first; i <= idx; ++i)
                bv.set(i, new_val);
        } else {
            VST* vd = vals_mut(node, ts, hs);
            VST new_val = fn(vd[idx]);
            bld.destroy_value(vd[idx]);
            VT::init_slot(&vd[idx], new_val);
            for (int i = first; i < idx; ++i)
                VT::write_slot(&vd[i], new_val);
        }
        return true;
    }

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
    // Dedup + skip one key, writing into output arrays
    // ==================================================================

    static void dedup_skip_into(const K* kd, VST* vd, uint16_t ts,
                                  K skip_suffix,
                                  K* out_k, VST* out_v, BLD& bld) {
        bool skipped = false;
        int wi = 0;
        if (kd[0] == skip_suffix) {
            skipped = true;
            if constexpr (VT::HAS_DESTRUCTOR)
                bld.destroy_value(vd[0]);
        } else {
            out_k[wi] = kd[0];
            VT::write_slot(&out_v[wi], vd[0]);
            wi++;
        }
        for (int i = 1; i < ts; ++i) {
            if (kd[i] == kd[i - 1]) continue;
            if (!skipped && kd[i] == skip_suffix) {
                skipped = true;
                if constexpr (VT::HAS_DESTRUCTOR)
                    bld.destroy_value(vd[i]);
                continue;
            }
            out_k[wi] = kd[i];
            VT::write_slot(&out_v[wi], vd[i]);
            wi++;
        }
        // C-type: erased pointer freed above; dups share pointers with
        //         copied entries (transferred), so no further destroy needed.
    }

    // ==================================================================
    // Single-pass: dedup old array + inject new key + seed dups into dest
    // No temp arrays. One read pass, one write pass.
    // ==================================================================

    static void seed_with_insert(K* dk, VST* dv,
                                   const K* old_k, const VST* old_v,
                                   uint16_t old_ts, uint16_t old_entries,
                                   K new_suffix, VST new_val,
                                   uint16_t new_entries, uint16_t new_ts) {

        if (new_entries == new_ts) {
            // No dups needed — straight copy with insert
            bool inserted = false;
            int wi = 0;
            // Handle first element (no dup check needed)
            if (new_suffix < old_k[0]) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
                wi++;
                inserted = true;
            }
            dk[wi] = old_k[0];
            VT::init_slot(&dv[wi], old_v[0]);
            wi++;
            for (int i = 1; i < old_ts; ++i) {
                if (old_k[i] == old_k[i - 1]) continue;
                if (!inserted && new_suffix < old_k[i]) {
                    dk[wi] = new_suffix;
                    VT::init_slot(&dv[wi], new_val);
                    wi++;
                    inserted = true;
                }
                dk[wi] = old_k[i];
                VT::init_slot(&dv[wi], old_v[i]);
                wi++;
            }
            if (!inserted) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
            }
            return;
        }

        // Dup seeding: distribute n_dups evenly among new_entries real entries
        uint16_t n_dups = new_ts - new_entries;
        uint16_t stride = new_entries / (n_dups + 1);
        uint16_t remainder = new_entries % (n_dups + 1);

        int wi = 0;         // write index into destination
        int real_out = 0;   // count of real entries emitted
        int placed = 0;     // dups placed so far
        int group_size = stride + (0 < remainder ? 1 : 0);
        int in_group = 0;   // entries written in current group

        bool inserted = false;

        // Handle first old entry (no dup check needed for i=0)
        // Check if new key goes before first old key
        if (new_suffix < old_k[0]) {
            dk[wi] = new_suffix;
            VT::init_slot(&dv[wi], new_val);
            wi++;
            real_out++;
            in_group++;
            inserted = true;

            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
                in_group = 0;
                group_size = stride + (placed < remainder ? 1 : 0);
            }
        }

        // Emit first old entry
        dk[wi] = old_k[0];
        VT::init_slot(&dv[wi], old_v[0]);
        wi++;
        real_out++;
        in_group++;

        if (placed < n_dups && in_group >= group_size) {
            dk[wi] = dk[wi - 1];
            VT::init_slot(&dv[wi], dv[wi - 1]);
            wi++;
            placed++;
            in_group = 0;
            group_size = stride + (placed < remainder ? 1 : 0);
        }

        for (int i = 1; i < old_ts; ++i) {
            if (old_k[i] == old_k[i - 1]) continue;  // skip dup

            // Inject new key at sorted position
            if (!inserted && new_suffix < old_k[i]) {
                dk[wi] = new_suffix;
                VT::init_slot(&dv[wi], new_val);
                wi++;
                real_out++;
                in_group++;
                inserted = true;

                // Check if group full → emit dup
                if (placed < n_dups && in_group >= group_size) {
                    dk[wi] = dk[wi - 1];
                    VT::init_slot(&dv[wi], dv[wi - 1]);
                    wi++;
                    placed++;
                    in_group = 0;
                    group_size = stride + (placed < remainder ? 1 : 0);
                }
            }

            // Emit real entry from old array
            dk[wi] = old_k[i];
            VT::init_slot(&dv[wi], old_v[i]);
            wi++;
            real_out++;
            in_group++;

            // Check if group full → emit dup
            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
                in_group = 0;
                group_size = stride + (placed < remainder ? 1 : 0);
            }
        }

        // New key is largest — append at end
        if (!inserted) {
            dk[wi] = new_suffix;
            VT::init_slot(&dv[wi], new_val);
            wi++;
            real_out++;
            in_group++;

            if (placed < n_dups && in_group >= group_size) {
                dk[wi] = dk[wi - 1];
                VT::init_slot(&dv[wi], dv[wi - 1]);
                wi++;
                placed++;
            }
        }
    }

    // ==================================================================
    // Dup helpers
    // ==================================================================

    static int find_dup_pos(const K* kd, int total, int ins, unsigned entries) {
        int dup_pos = -1;
        if (total <= static_cast<int>(DUP_SCAN_MAX)) {
            for (int i = ins; i < total - 1; ++i) {
                if (kd[i] == kd[i + 1]) { dup_pos = i; break; }
            }
            if (dup_pos < 0) {
                for (int i = ins - 1; i >= 1; --i) {
                    if (kd[i] == kd[i - 1]) { dup_pos = i; break; }
                }
            }
        } else {
            unsigned dups = total - entries;
            int band = entries / (dups + 1) + 1;
            int r_lo = ins, r_hi = ins;
            int l_hi = ins - 1, l_lo = ins - 1;

            while (dup_pos < 0) {
                r_hi = std::min(r_lo + band, total - 1);
                for (int i = r_lo; i < r_hi; ++i) {
                    if (kd[i] == kd[i + 1]) { dup_pos = i; break; }
                }
                if (dup_pos >= 0) break;
                r_lo = r_hi;

                l_lo = std::max(1, l_hi - band + 1);
                for (int i = l_lo; i <= l_hi; ++i) {
                    if (kd[i] == kd[i - 1]) { dup_pos = i; break; }
                }
                if (dup_pos >= 0) break;
                l_hi = l_lo - 1;
            }
        }
        return dup_pos;
    }

    static void insert_consume_dup(
            K* kd, VST* vd, int total, int ins, unsigned entries,
            K suffix, VST value) {
        int dup_pos = find_dup_pos(kd, total, ins, entries);

        int write_pos;
        if (dup_pos < ins) {
            int shift_count = ins - 1 - dup_pos;
            if (shift_count > 0) {
                std::memmove(kd + dup_pos, kd + dup_pos + 1, shift_count * sizeof(K));
                std::memmove(vd + dup_pos, vd + dup_pos + 1, shift_count * sizeof(VST));
            }
            write_pos = ins - 1;
        } else {
            int shift_count = dup_pos - ins;
            if (shift_count > 0) {
                std::memmove(kd + ins + 1, kd + ins, shift_count * sizeof(K));
                std::memmove(vd + ins + 1, vd + ins, shift_count * sizeof(VST));
            }
            write_pos = ins;
        }

        kd[write_pos] = suffix;
        VT::write_slot(&vd[write_pos], value);
    }

    static void erase_create_dup(
            K* kd, VST* vd, int total, int idx,
            K suffix, BLD& bld) {
        const K* fb = adaptive_search<K>::find_base_first(kd, total, suffix);
        int first = static_cast<int>(fb - kd) + (*fb != suffix);

        if constexpr (VT::HAS_DESTRUCTOR)
            bld.destroy_value(vd[first]);

        K   neighbor_key;
        VST neighbor_val;
        if (first > 0) {
            neighbor_key = kd[first - 1];
            neighbor_val = vd[first - 1];
        } else {
            neighbor_key = kd[idx + 1];
            neighbor_val = vd[idx + 1];
        }
        // vd[first] is destroyed (uninit for B), rest are live dups
        kd[first] = neighbor_key;
        VT::init_slot(&vd[first], neighbor_val);
        for (int i = first + 1; i <= idx; ++i) {
            kd[i] = neighbor_key;
            VT::write_slot(&vd[i], neighbor_val);
        }
    }

    // ==================================================================
    // Value pointer helpers — return const VALUE* at position
    // ==================================================================
public:

    static const VALUE* val_ptr_at(const uint64_t* node, unsigned ts,
                                     size_t hs, unsigned pos) noexcept {
        if constexpr (VT::IS_BOOL)
            return bool_vals(node, ts, hs).ptr_at(pos);
        else
            return VT::as_ptr(vals(node, ts, hs)[pos]);
    }
    static const VALUE* val_ptr_first(const uint64_t* node, size_t hs) noexcept {
        unsigned ts = get_header(node)->total_slots();
        return val_ptr_at(node, ts, hs, 0);
    }
    static const VALUE* val_ptr_last(const uint64_t* node, size_t hs) noexcept {
        unsigned ts = get_header(node)->total_slots();
        return val_ptr_at(node, ts, hs, ts - 1);
    }

    // ==================================================================
    // find_fn — exact match, returns VALUE* or null
    // ==================================================================

    template<bool DO_SKIP, bool DO_SCAN>
    static const VALUE* find_fn(const uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            if (!skip_eq(leaf_prefix(node), d, ik)) [[unlikely]]
                return nullptr;
        }

        K suffix = static_cast<K>(d.suffix(ik));
        const K* kd = keys(node, hs);
        unsigned ts = get_header(node)->total_slots();

        unsigned pos;
        if constexpr (DO_SCAN) {
            pos = 0;
            for (unsigned i = 1; i < ts; ++i)
                pos = (kd[i] <= suffix) ? i : pos;
        } else {
            const K* base = adaptive_search<K>::find_base(kd, ts, suffix);
            pos = static_cast<unsigned>(base - kd);
        }

        if (kd[pos] != suffix) [[unlikely]] return nullptr;
        return val_ptr_at(node, ts, hs, pos);
    }

    // ==================================================================
    // find_next_fn — first entry with key > ik
    // ==================================================================

    template<bool DO_SKIP, bool DO_SCAN>
    static leaf_result_t<VALUE> find_next_fn(const uint64_t* node,
                                               uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);
        uint64_t pfx = leaf_prefix(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(pfx, d, ik);
            if (cmp > 0) return {0, nullptr, false};
            if (cmp < 0) {
                // All entries > ik. Return minimum.
                return {d.to_ik(pfx, keys(node, hs)[0]),
                        val_ptr_first(node, hs), true};
            }
        }

        K suffix = static_cast<K>(d.suffix(ik));
        if (suffix >= static_cast<K>(d.nk_max())) [[unlikely]]
            return {0, nullptr, false};

        K target = suffix + 1;
        const K* kd = keys(node, hs);
        unsigned ts = get_header(node)->total_slots();

        unsigned pos;
        if constexpr (DO_SCAN) {
            pos = ts;
            for (unsigned i = ts; i-- > 0;)
                pos = (kd[i] >= target) ? i : pos;
            if (pos >= ts) [[unlikely]] return {0, nullptr, false};
        } else {
            const K* base = adaptive_search<K>::find_base_first(kd, ts, target);
            pos = static_cast<unsigned>(base - kd);
            if (kd[pos] < target) [[unlikely]] {
                ++pos;
                if (pos >= ts) [[unlikely]] return {0, nullptr, false};
            }
        }

        K found_suffix = kd[pos];
        return {d.to_ik(pfx, found_suffix), val_ptr_at(node, ts, hs, pos), true};
    }

    // ==================================================================
    // find_prev_fn — last entry with key < ik
    // ==================================================================

    template<bool DO_SKIP, bool DO_SCAN>
    static leaf_result_t<VALUE> find_prev_fn(const uint64_t* node,
                                               uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);
        uint64_t pfx = leaf_prefix(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(pfx, d, ik);
            if (cmp < 0) return {0, nullptr, false};
            if (cmp > 0) {
                // All entries < ik. Return maximum.
                unsigned ts = get_header(node)->total_slots();
                return {d.to_ik(pfx, keys(node, hs)[ts - 1]),
                        val_ptr_last(node, hs), true};
            }
        }

        K suffix = static_cast<K>(d.suffix(ik));
        if (suffix == 0) [[unlikely]]
            return {0, nullptr, false};

        K target = suffix - 1;
        const K* kd = keys(node, hs);
        unsigned ts = get_header(node)->total_slots();

        unsigned pos;
        if constexpr (DO_SCAN) {
            pos = 0;
            for (unsigned i = 1; i < ts; ++i)
                pos = (kd[i] <= target) ? i : pos;
            if (kd[pos] > target) [[unlikely]] return {0, nullptr, false};
        } else {
            const K* base = adaptive_search<K>::find_base(kd, ts, target);
            pos = static_cast<unsigned>(base - kd);
            if (kd[pos] > target) [[unlikely]] return {0, nullptr, false};
        }

        K found_suffix = kd[pos];
        return {d.to_ik(pfx, found_suffix), val_ptr_at(node, ts, hs, pos), true};
    }

    // ==================================================================
    // find_first_fn — minimum entry (no template, no skip check)
    // ==================================================================

    static leaf_result_t<VALUE> find_first_fn(const uint64_t* node) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);
        uint64_t pfx = leaf_prefix(node);
        K first_key = keys(node, hs)[0];
        return {d.to_ik(pfx, first_key), val_ptr_first(node, hs), true};
    }

    // ==================================================================
    // find_last_fn — maximum entry (no template, no skip check)
    // ==================================================================

    static leaf_result_t<VALUE> find_last_fn(const uint64_t* node) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);
        uint64_t pfx = leaf_prefix(node);
        unsigned ts = get_header(node)->total_slots();
        K last_key = keys(node, hs)[ts - 1];
        return {d.to_ik(pfx, last_key), val_ptr_last(node, hs), true};
    }

    // ==================================================================
    // Tables + set_leaf_fns
    // 4-entry tables indexed by (DO_SKIP * 2 + DO_SCAN)
    //   0 = {false, false}   1 = {false, true}
    //   2 = {true,  false}   3 = {true,  true}
    // ==================================================================

    static inline const find_fn_t<VALUE> FIND_TABLE[4] = {
        &find_fn<false, false>,  &find_fn<false, true>,
        &find_fn<true,  false>,  &find_fn<true,  true>,
    };
    static inline const iter_fn_t<VALUE> NEXT_TABLE[4] = {
        &find_next_fn<false, false>,  &find_next_fn<false, true>,
        &find_next_fn<true,  false>,  &find_next_fn<true,  true>,
    };
    static inline const iter_fn_t<VALUE> PREV_TABLE[4] = {
        &find_prev_fn<false, false>,  &find_prev_fn<false, true>,
        &find_prev_fn<true,  false>,  &find_prev_fn<true,  true>,
    };

    static void set_leaf_fns(uint64_t* node, unsigned ts, bool has_skip) noexcept {
        bool do_scan = (ts <= SCAN_MAX);
        unsigned idx = (has_skip ? 2u : 0u) + (do_scan ? 1u : 0u);
        set_find_fn<VALUE>(node, FIND_TABLE[idx]);
        set_find_next<VALUE>(node, NEXT_TABLE[idx]);
        set_find_prev<VALUE>(node, PREV_TABLE[idx]);
        set_find_first<VALUE>(node, &find_first_fn);
        set_find_last<VALUE>(node, &find_last_fn);
    }

    // ==================================================================
    // Seed: distribute dups evenly among real entries
    // ==================================================================

    static void seed_from_real(K* kd, VST* vd,
                                const K* real_keys, const VST* real_vals,
                                uint16_t n_entries, uint16_t total) {

        if (n_entries == total) {
            std::memcpy(kd, real_keys, n_entries * sizeof(K));
            VT::copy_uninit(real_vals, n_entries, vd);
            return;
        }

        uint16_t n_dups = total - n_entries;
        uint16_t stride = n_entries / (n_dups + 1);
        uint16_t remainder = n_entries % (n_dups + 1);

        int write = 0, src = 0, placed = 0;
        while (placed < n_dups) {
            int chunk = stride + (placed < remainder ? 1 : 0);
            std::memcpy(kd + write, real_keys + src, chunk * sizeof(K));
            VT::copy_uninit(real_vals + src, chunk, vd + write);
            write += chunk;
            src += chunk;
            kd[write] = kd[write - 1];
            VT::init_slot(&vd[write], vd[write - 1]);
            write++;
            placed++;
        }

        int remaining = n_entries - src;
        if (remaining > 0) {
            std::memcpy(kd + write, real_keys + src, remaining * sizeof(K));
            VT::copy_uninit(real_vals + src, remaining, vd + write);
        }
    }
};

} // namespace gteitelbaum

#endif // KNTRIE_COMPACT_HPP
