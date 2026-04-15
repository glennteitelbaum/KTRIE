#ifndef KNTRIE_OPS_HPP
#define KNTRIE_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

#include <array>
#include <bit>

namespace gteitelbaum::kntrie_detail {

// ======================================================================
// kntrie_ops<K, VALUE, ALLOC> — stateless trie operations.
//
// K = stored key type (std::make_unsigned_t<KEY>, sign-flipped).
// All functions take K stored + unsigned shift.
// shift = byte position (MSB first): TOP_BYTE_SHIFT, TOP_BYTE_SHIFT-8, ..., 0.
// shift == 0 means bitmap leaf depth (final byte).
//
// No BITS template parameter.  No depth_switch.  No narrowing.
// ======================================================================

template<typename K, typename VALUE, typename ALLOC>
struct kntrie_ops {
    using BO  = bitmask_ops<K, VALUE, ALLOC>;
    using CO  = compact_ops<K, VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;
    using KO  = key_ops<K>;

    static constexpr unsigned TOP_BYTE_SHIFT = KO::TOP_BYTE_SHIFT;
    static constexpr int KEY_BYTES = KO::KEY_BYTES;

    // Is child at this shift a bitmap leaf? (final byte of key)
    static constexpr bool is_bitmap_depth(unsigned shift) noexcept {
        return shift == 0;
    }

    // ==================================================================
    // find_loop — bitmask descent + leaf dispatch in one function.
    //
    // Right-shifts stored by running shift counter to extract each byte.
    // Compact gets original stored for binary search.
    // Bitmap just gets the final byte — no header needed.
    // ==================================================================

    static iter_entry_t<K> find_loop(std::uint64_t ptr, K stored,
                                     unsigned shift) noexcept {
        while (!(ptr & LEAF_BIT)) [[likely]] {
            ptr = BO::bm_child(ptr, static_cast<std::uint8_t>((stored >> shift) & 0xFF));
            shift -= CHAR_BIT;
        }
        if (ptr & NOT_FOUND_BIT) [[unlikely]] return {};
        auto* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);  // prefetches cache line for compact_find
        if (hdr->is_bitmap()) [[unlikely]] {
            std::uint8_t byte = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            return BO::bitmap_find_byte(node, stored, byte);
        }
        return CO::compact_find(node, hdr, stored);
    }

    // ==================================================================
    // descend_edge_loop — walk to min/max leaf + dispatch in one call.
    // ==================================================================

    static iter_entry_t<K> descend_edge_loop(std::uint64_t ptr, dir_t dir) noexcept {
        if (dir == dir_t::FWD) {
            while (!(ptr & LEAF_BIT))
                ptr = BO::bm_first_child(ptr);
        } else {
            while (!(ptr & LEAF_BIT))
                ptr = BO::bm_last_child(ptr);
        }
        auto* node = untag_leaf_mut(ptr);
        auto* hdr = get_header(node);
        if (hdr->is_bitmap())
            return BO::bitmap_edge(node, dir);
        return CO::compact_edge(node, dir);
    }

    // ==================================================================
    // Descendant count helpers (bitmask nodes)
    // ==================================================================

    static void inc_descendants(std::uint64_t* node, node_header_t* hdr) noexcept {
        std::uint8_t sc = hdr->skip();
        unsigned nc = (sc > 0) ? BO::chain_child_count(node, sc) : hdr->entries();
        BO::chain_descendants_mut(node, sc, nc)++;
    }

    static std::uint64_t dec_descendants(std::uint64_t* node, node_header_t* hdr) noexcept {
        std::uint8_t sc = hdr->skip();
        unsigned nc = (sc > 0) ? BO::chain_child_count(node, sc) : hdr->entries();
        return --BO::chain_descendants_mut(node, sc, nc);
    }

    // ==================================================================
    // make_single_leaf — one entry, compact leaf
    // ==================================================================

    static std::uint64_t* make_single_leaf(K stored, VST value, BLD& bld) {
        std::uint64_t* leaf = CO::make_leaf(&stored, &value, 1, bld);
        return leaf;
    }

    // ==================================================================
    // build_node_from_arrays — recursive subtree builder.
    //
    // Builds from sorted full-K arrays.  At bitmap depth (shift==0),
    // creates bitmap leaf.  Otherwise, compact leaf or recursive bitmask.
    // Returns tagged pointer.
    // ==================================================================

    static std::uint64_t build_node_from_arrays_tagged(
            K* keys, VST* vals, std::size_t count,
            unsigned shift, K base_key, BLD& bld) {

        // Bitmap depth: create bitmap leaf
        if (is_bitmap_depth(shift)) {
            std::uint8_t suffixes[BYTE_VALUES];
            for (std::size_t i = 0; i < count; ++i)
                suffixes[i] = static_cast<std::uint8_t>(keys[i] & 0xFF);
            return tag_leaf(BO::make_bitmap_leaf(suffixes, vals,
                static_cast<unsigned>(count), base_key & ~K(0xFF), bld));
        }

        // Few entries: compact leaf (no further splitting)
        if (count <= COMPACT_MAX) {
            return tag_leaf(CO::make_leaf(keys, vals,
                static_cast<unsigned>(count), bld));
        }

        // Many entries: partition by byte at shift, build bitmask
        std::uint8_t indices[BYTE_VALUES];
        std::uint64_t child_ptrs[BYTE_VALUES];
        int n_children = 0;

        std::size_t i = 0;
        while (i < count) {
            std::uint8_t ti = static_cast<std::uint8_t>((keys[i] >> shift) & 0xFF);
            std::size_t start = i;
            while (i < count &&
                   static_cast<std::uint8_t>((keys[i] >> shift) & 0xFF) == ti) ++i;
            indices[n_children] = ti;
            child_ptrs[n_children] = build_node_from_arrays_tagged(
                keys + start, vals + start, i - start,
                shift - CHAR_BIT, keys[start], bld);
            n_children++;
        }

        return tag_bitmask(
            BO::make_bitmask(indices, child_ptrs, n_children, bld, count));
    }

    // ==================================================================
    // Leaf insert dispatch — compact or bitmap
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    static insert_result_t leaf_insert(std::uint64_t* node, node_header_t* hdr,
                                       K stored, unsigned shift,
                                       VST value, BLD& bld) {
        if (hdr->is_bitmap()) {
            std::uint8_t suffix = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            return BO::template bitmap_insert<INSERT, ASSIGN>(node, suffix, value, bld);
        } else {
            auto result = CO::template insert<INSERT, ASSIGN>(node, hdr, stored, value, bld);
            if (result.needs_split) [[unlikely]] {
                return convert_to_bitmask_tagged(node, hdr, stored, shift, value, bld);
            }
            return result;
        }
    }

    // ==================================================================
    // Leaf erase dispatch — compact or bitmap
    // ==================================================================

    static erase_result_t<K> leaf_erase(std::uint64_t* node, node_header_t* hdr,
                                        K stored, unsigned shift, BLD& bld) {
        if (hdr->is_bitmap()) {
            std::uint8_t suffix = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            return BO::bitmap_erase(node, suffix, bld);
        } else {
            return CO::erase(node, hdr, stored, bld);
        }
    }

    // ==================================================================
    // convert_to_bitmask_tagged — compact leaf overflow at COMPACT_MAX.
    //
    // Partitions old entries + new entry by byte at shift position.
    // Uses merge approach: old entries partitioned in 3 loops
    // (before/at/after new entry's byte group).
    // ==================================================================

    static insert_result_t convert_to_bitmask_tagged(
            const std::uint64_t* node, const node_header_t* hdr,
            K stored, unsigned shift, VST value, BLD& bld) {

        std::uint16_t old_count = hdr->entries();
        constexpr std::size_t hs = COMPACT_HEADER_U64;
        const K* old_keys = CO::keys(node, hs);

        // For IS_BOOL, unpack. Sized to 1 for non-bool to avoid dead stack.
        constexpr std::size_t UNPACK_SIZE = VT::IS_BOOL ? COMPACT_MAX : 1;
        VST unpacked_vals[UNPACK_SIZE];
        const VST* old_vals;
        if constexpr (VT::IS_BOOL) {
            auto bv = CO::bool_vals(node);
            for (std::uint16_t i = 0; i < old_count; ++i)
                unpacked_vals[i] = bv.get(i);
            old_vals = unpacked_vals;
        } else {
            old_vals = CO::vals(node);
        }

        // Find insertion point via adaptive binary search
        const K* ins_base = adaptive_search<K>::find_base_first(
            old_keys, old_count, stored);
        unsigned ins = static_cast<unsigned>(ins_base - old_keys)
                     + (*ins_base < stored);

        // Partition byte position
        std::uint8_t new_top_byte = static_cast<std::uint8_t>((stored >> shift) & 0xFF);

        // Partition children
        std::uint8_t indices[BYTE_VALUES];
        std::uint64_t child_ptrs[BYTE_VALUES];
        int n_children = 0;

        // Merge buffers for the new entry's byte group
        constexpr std::size_t MERGE_MAX = COMPACT_MAX + 1;
        K   merge_keys[MERGE_MAX];
        VST merge_vals[MERGE_MAX];

        unsigned child_shift = shift - CHAR_BIT;

        auto build_byte_group = [&](std::size_t start, std::size_t count, std::uint8_t ti) {
            indices[n_children] = ti;
            child_ptrs[n_children] = build_node_from_arrays_tagged(
                const_cast<K*>(old_keys + start),
                const_cast<VST*>(old_vals + start),
                count, child_shift, old_keys[start], bld);
            n_children++;
        };

        std::size_t i = 0;

        // Loop 1: byte ranges < new_top_byte
        while (i < old_count) {
            std::uint8_t ti = static_cast<std::uint8_t>((old_keys[i] >> shift) & 0xFF);
            if (ti >= new_top_byte) break;
            std::size_t start = i;
            while (i < old_count &&
                   static_cast<std::uint8_t>((old_keys[i] >> shift) & 0xFF) == ti) ++i;
            build_byte_group(start, i - start, ti);
        }

        // Loop 2: byte == new_top_byte — merge old range + new entry
        {
            std::size_t range_start = i;
            while (i < old_count &&
                   static_cast<std::uint8_t>((old_keys[i] >> shift) & 0xFF) == new_top_byte) ++i;
            std::size_t range_old = i - range_start;
            std::size_t range_new = range_old + 1;
            unsigned local_ins = ins - static_cast<unsigned>(range_start);

            if (local_ins > 0)
                std::memcpy(merge_keys, old_keys + range_start, local_ins * sizeof(K));
            merge_keys[local_ins] = stored;
            if (range_old > local_ins)
                std::memcpy(merge_keys + local_ins + 1, old_keys + range_start + local_ins,
                             (range_old - local_ins) * sizeof(K));
            if (local_ins > 0)
                std::memcpy(merge_vals, old_vals + range_start, local_ins * sizeof(VST));
            merge_vals[local_ins] = value;
            if (range_old > local_ins)
                std::memcpy(merge_vals + local_ins + 1, old_vals + range_start + local_ins,
                             (range_old - local_ins) * sizeof(VST));

            indices[n_children] = new_top_byte;
            child_ptrs[n_children] = build_node_from_arrays_tagged(
                merge_keys, merge_vals, range_new,
                child_shift, merge_keys[0], bld);
            n_children++;
        }

        // Loop 3: byte ranges > new_top_byte
        while (i < old_count) {
            std::uint8_t ti = static_cast<std::uint8_t>((old_keys[i] >> shift) & 0xFF);
            std::size_t start = i;
            while (i < old_count &&
                   static_cast<std::uint8_t>((old_keys[i] >> shift) & 0xFF) == ti) ++i;
            build_byte_group(start, i - start, ti);
        }

        std::size_t total = old_count + 1;

        std::uint64_t child_tagged;

        if (n_children == 1) [[unlikely]] {
            child_tagged = child_ptrs[0];

            if (child_tagged & LEAF_BIT) {
                std::uint64_t* leaf = untag_leaf_mut(child_tagged);
                if (get_header(leaf)->is_bitmap()) {
                    // Bitmap child: needs dispatch path — wrap in one-child bitmask
                    child_tagged = tag_bitmask(
                        BO::make_bitmask(indices, child_ptrs, 1, bld, total));
                }
                // Compact child: return directly (full K stored)
            } else {
                // Bitmask child: wrap dispatch byte in chain
                std::uint8_t pfx_bytes[1] = {indices[0]};
                std::uint64_t* bm_node = bm_to_node(child_tagged);
                child_tagged = BO::wrap_in_chain(bm_node, pfx_bytes, 1, bld);
            }
        } else {
            child_tagged = tag_bitmask(
                BO::make_bitmask(indices, child_ptrs, n_children, bld, total));
        }

        bld.dealloc_node(const_cast<std::uint64_t*>(node),
            CO::alloc_total_u64(hdr->alloc_u64()));

        // Locate the inserted entry in the new subtree (cache-hot)
        auto lp = find_loop(child_tagged, stored, shift);
        return {child_tagged, true, false, nullptr, lp.leaf, lp.pos};
    }

    // ==================================================================
    // split_skip_at — bitmask chain skip byte mismatch during insert.
    //
    // Create new bitmask at the divergence point within the skip chain.
    // ==================================================================

    static insert_result_t split_skip_at(std::uint64_t* node, node_header_t* hdr,
                                         std::uint8_t sc, std::uint8_t mismatch_pos,
                                         K stored, unsigned shift,
                                         VST value, BLD& bld) {
        // Divergence byte
        std::uint8_t expected_byte = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
        std::uint8_t actual_byte = BO::skip_byte(node, mismatch_pos);

        // Create a new single-entry leaf for the new key
        std::uint64_t* new_leaf = make_single_leaf(stored, value, bld);
        std::uint16_t new_pos = 0;

        // New bitmask at divergence: two children
        std::uint8_t bm_indices[2] = {
            std::min(expected_byte, actual_byte),
            std::max(expected_byte, actual_byte)
        };

        // Build old subtree child pointer: remaining skip chain after mismatch
        std::uint64_t old_child;
        std::uint8_t remaining_skip = sc - mismatch_pos - 1;
        if (remaining_skip > 0) {
            // Rebuild skip chain with remaining bytes
            std::uint8_t skip_buf[MAX_SKIP];
            for (std::uint8_t si = 0; si < remaining_skip; ++si)
                skip_buf[si] = BO::skip_byte(node, mismatch_pos + 1 + si);
            old_child = BO::wrap_in_chain(
                bm_to_node(BO::node_bm_ptr(node)),
                skip_buf, remaining_skip, bld);
        } else {
            old_child = BO::node_bm_ptr(node);
        }

        std::uint64_t new_child = tag_leaf(new_leaf);
        std::uint64_t child_ptrs[2];
        if (expected_byte < actual_byte) {
            child_ptrs[0] = new_child;
            child_ptrs[1] = old_child;
        } else {
            child_ptrs[0] = old_child;
            child_ptrs[1] = new_child;
        }

        // Count entries: old subtree entries + 1
        std::uint64_t old_entries = BO::chain_descendants(node, sc,
            (sc > 0) ? BO::chain_child_count(node, sc) : hdr->entries());
        std::uint64_t total = old_entries + 1;

        std::uint64_t* bm_node = BO::make_bitmask(bm_indices, child_ptrs, 2, bld, total);

        // Prepend skip bytes before the mismatch point
        std::uint64_t result_tagged;
        if (mismatch_pos > 0) {
            std::uint8_t prefix_buf[MAX_SKIP];
            for (std::uint8_t pi = 0; pi < mismatch_pos; ++pi)
                prefix_buf[pi] = BO::skip_byte(node, pi);
            result_tagged = BO::wrap_in_chain(bm_node, prefix_buf, mismatch_pos, bld);
        } else {
            result_tagged = tag_bitmask(bm_node);
        }

        // Dealloc old chain
        bld.dealloc_node(node, hdr->alloc_u64());

        return {result_tagged, true, false, nullptr, new_leaf, new_pos};
    }

    // ==================================================================
    // Insert — runtime-recursive, shift-based
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    static insert_result_t insert_node(std::uint64_t ptr, K stored,
                                       unsigned shift, VST value, BLD& bld) {
        // SENTINEL
        if (ptr == BO::SENTINEL_TAGGED) [[unlikely]] {
            if constexpr (!INSERT) return {ptr, false, false};
            auto* leaf = make_single_leaf(stored, value, bld);
            return {tag_leaf(leaf), true, false, nullptr, leaf, 0};
        }

        // LEAF — no leaf has skip, dispatch directly
        if (ptr & LEAF_BIT) [[unlikely]] {
            std::uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);
            return leaf_insert<INSERT, ASSIGN>(node, hdr, stored, shift, value, bld);
        }

        // BITMASK
        std::uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();

        // Walk chain skip bytes
        for (std::uint8_t pos = 0; pos < sc; ++pos) {
            std::uint8_t expected = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            std::uint8_t actual = BO::skip_byte(node, pos);
            if (expected != actual) [[unlikely]] {
                if constexpr (!INSERT) return {tag_bitmask(node), false, false};
                return split_skip_at(node, hdr, sc, pos,
                                     stored, shift, value, bld);
            }
            shift -= CHAR_BIT;
        }

        // Final bitmap lookup
        std::uint8_t ti = static_cast<std::uint8_t>((stored >> shift) & 0xFF);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};

            // Create new leaf for this key
            std::uint64_t* new_leaf;
            std::uint16_t new_pos;
            std::uint64_t leaf_tagged;

            unsigned child_shift = shift - CHAR_BIT;
            if (is_bitmap_depth(child_shift)) {
                std::uint8_t suffix = static_cast<std::uint8_t>(stored & 0xFF);
                K base = stored & ~K(0xFF);
                new_leaf = BO::make_single_bitmap(suffix, value, base, bld);
                new_pos = VT::IS_BOOL ? static_cast<std::uint16_t>(suffix) : 0;
                leaf_tagged = tag_leaf(new_leaf);
            } else {
                new_leaf = make_single_leaf(stored, value, bld);
                new_pos = 0;
                leaf_tagged = tag_leaf(new_leaf);
            }

            std::uint64_t* nn;
            if (sc > 0) [[unlikely]]
                nn = BO::chain_add_child(node, hdr, sc, ti, leaf_tagged, bld);
            else
                nn = BO::add_child(node, hdr, ti, leaf_tagged, bld);
            inc_descendants(nn, get_header(nn));
            return {tag_bitmask(nn), true, false, nullptr, new_leaf, new_pos};
        }

        // Recurse into child
        auto cr = insert_node<INSERT, ASSIGN>(
            cl.child, stored, shift - CHAR_BIT, value, bld);

        // Parent fixup on unwind
        if (cr.tagged_ptr != cl.child) [[unlikely]] {
            if (sc > 0) [[unlikely]]
                BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
            else
                BO::set_child(node, cl.slot, cr.tagged_ptr);
            link_child(node, cr.tagged_ptr, ti);
        }
        if (cr.inserted) [[likely]]
            inc_descendants(node, hdr);
        return {tag_bitmask(node), cr.inserted, false, cr.existing_value,
                cr.leaf, cr.pos};
    }

    // ==================================================================
    // Erase — runtime-recursive, shift-based
    // ==================================================================

    static erase_result_t<K> erase_node(std::uint64_t ptr, K stored,
                                        unsigned shift, BLD& bld) {
        if (ptr == BO::SENTINEL_TAGGED) [[unlikely]]
            return {ptr, false, 0, {}};

        // LEAF
        if (ptr & LEAF_BIT) [[unlikely]] {
            std::uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);
            // No leaf has skip — dispatch directly
            return leaf_erase(node, hdr, stored, shift, bld);
        }

        // BITMASK
        std::uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();

        // Walk chain skip bytes
        for (std::uint8_t pos = 0; pos < sc; ++pos) {
            std::uint8_t expected = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            std::uint8_t actual = BO::skip_byte(node, pos);
            if (expected != actual) [[unlikely]]
                return {tag_bitmask(node), false, 0, {}};
            shift -= CHAR_BIT;
        }

        // Final bitmap lookup
        std::uint8_t ti = static_cast<std::uint8_t>((stored >> shift) & 0xFF);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]]
            return {tag_bitmask(node), false, 0, {}};

        // Recurse into child
        auto cr = erase_node(cl.child, stored, shift - CHAR_BIT, bld);

        if (!cr.erased) [[unlikely]]
            return {tag_bitmask(node), false, 0, {}};

        // Resolve next entry
        iter_entry_t<K> resolved_next = cr.next;
        if (!resolved_next.found) {
            std::uint64_t bm_ptr = (sc > 0)
                ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                    &BO::chain_bitmap(node, sc)))
                : BO::node_bm_ptr(node);
            auto [sib, sib_found] = BO::bm_next_sibling(bm_ptr, ti);
            if (sib_found)
                resolved_next = descend_edge_loop(sib, dir_t::FWD);
        }

        if (cr.tagged_ptr) [[likely]] {
            if (cr.tagged_ptr != cl.child) [[unlikely]] {
                if (sc > 0) [[unlikely]]
                    BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
                else
                    BO::set_child(node, cl.slot, cr.tagged_ptr);
                link_child(node, cr.tagged_ptr, ti);
            }
            std::uint64_t exact = dec_descendants(node, hdr);
            if (exact <= COMPACT_MAX) [[unlikely]] {
                return do_coalesce(node, hdr, stored, exact, shift, bld);
            }
            return {tag_bitmask(node), true, exact, resolved_next};
        }

        // Child fully erased — remove from bitmap
        std::uint64_t* nn;
        if (sc > 0) [[unlikely]]
            nn = BO::chain_remove_child(node, hdr, sc, cl.slot, ti, bld);
        else
            nn = BO::remove_child(node, hdr, cl.slot, ti, bld);
        if (!nn) [[unlikely]] return {0, true, 0, resolved_next};

        hdr = get_header(nn);
        unsigned nc = hdr->entries();
        std::uint64_t exact = dec_descendants(nn, hdr);

        // Collapse when bitmask drops to 1 child
        if (nc == 1) [[unlikely]] {
            typename BO::collapse_info ci;
            if (sc > 0) [[unlikely]]
                ci = BO::chain_collapse_info(nn, sc);
            else
                ci = BO::standalone_collapse_info(nn);
            std::size_t nn_au64 = hdr->alloc_u64();

            if (ci.sole_child & LEAF_BIT) {
                std::uint64_t* leaf = untag_leaf_mut(ci.sole_child);
                if (get_header(leaf)->is_bitmap()) {
                    // Bitmap child: keep one-child bitmask.
                    // Bitmap needs a dispatch path to reach it — can't float free.
                    return {tag_bitmask(nn), true, exact, resolved_next};
                }
                // Compact child: return directly — full K, no skip needed.
                bld.dealloc_node(nn, nn_au64);
                return {ci.sole_child, true, exact, resolved_next};
            }
            // Bitmask child: wrap in chain
            std::uint64_t* child_node = bm_to_node(ci.sole_child);
            bld.dealloc_node(nn, nn_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, bld),
                    true, exact, resolved_next};
        }

        if (exact <= COMPACT_MAX) [[unlikely]] {
            return do_coalesce(nn, get_header(nn), stored, exact, shift, bld);
        }
        return {tag_bitmask(nn), true, exact, resolved_next};
    }

    // ==================================================================
    // walk_entries_in_order — zero-alloc subtree traversal
    // Callback: cb(K stored_key, VST value)
    // ==================================================================

    template<typename Fn>
    static void walk_entries_in_order(std::uint64_t tagged, unsigned shift, Fn&& cb) {
        if (tagged & LEAF_BIT) [[unlikely]] {
            std::uint64_t* node = untag_leaf_mut(tagged);
            const auto* hdr = get_header(node);
            if (hdr->is_bitmap()) {
                K base_key = BO::read_base_key(node);
                BO::for_each_bitmap(node, [&](std::uint8_t s, VST v) {
                    cb(base_key | K(s), v);
                });
            } else {
                CO::for_each(node, hdr, [&](K key, const VST& v) {
                    cb(key, v);
                });
            }
            return;
        }

        std::uint64_t* node = bm_to_node(tagged);
        const auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();
        unsigned child_shift = shift - (sc + 1) * CHAR_BIT;

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const std::uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](std::uint8_t /*idx*/, int slot) {
            walk_entries_in_order(rch[slot], child_shift, cb);
        });
    }

    // ==================================================================
    // walk_collect_and_dealloc — single-pass collect + post-order free
    // ==================================================================

    template<typename Fn>
    static void walk_collect_and_dealloc(std::uint64_t tagged, unsigned shift,
                                         Fn&& cb, BLD& bld) {
        if (tagged & LEAF_BIT) [[unlikely]] {
            std::uint64_t* node = untag_leaf_mut(tagged);
            const auto* hdr = get_header(node);
            if (hdr->is_bitmap()) {
                K base_key = BO::read_base_key(node);
                BO::for_each_bitmap(node, [&](std::uint8_t s, VST v) {
                    cb(base_key | K(s), v);
                });
                BO::bitmap_dealloc_node_only(node, bld);
            } else {
                CO::for_each(node, hdr, [&](K key, const VST& v) {
                    cb(key, v);
                });
                CO::dealloc_node_only(node, bld);
            }
            return;
        }

        std::uint64_t* node = bm_to_node(tagged);
        const auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();
        std::size_t node_au64 = hdr->alloc_u64();
        unsigned child_shift = shift - (sc + 1) * CHAR_BIT;

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const std::uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](std::uint8_t /*idx*/, int slot) {
            walk_collect_and_dealloc(rch[slot], child_shift, cb, bld);
        });

        // Post-order: children freed, now free this bitmask node
        bld.dealloc_node(node, node_au64);
    }

    // ==================================================================
    // do_coalesce — collect entries from subtree into single compact leaf
    // ==================================================================

    static erase_result_t<K> do_coalesce(std::uint64_t* node, node_header_t* hdr,
                                         K stored, std::uint64_t total_entries,
                                         unsigned shift, BLD& bld) {
        std::uint8_t sc = hdr->skip();

        // Allocate compact leaf for all entries
        constexpr std::size_t hu = COMPACT_HEADER_U64;
        std::size_t total_u64 = round_up_u64(CO::size_u64(total_entries, hu));
        std::uint64_t* leaf = bld.alloc_node(total_u64, false);
        auto* lh = get_header(leaf);
        lh->set_entries(static_cast<std::uint16_t>(total_entries));
        CO::set_capacity(leaf, total_u64);

        K* dk = CO::keys(leaf, hu);
        std::size_t wi = 0;

        unsigned walk_shift = shift - sc * CHAR_BIT;

        if constexpr (VT::IS_BOOL) {
            auto bv = CO::bool_vals_mut(leaf);
            bv.clear_all(total_entries);
            walk_collect_and_dealloc(tag_bitmask(node), walk_shift,
                [&](K key, VST v) {
                    dk[wi] = key;
                    bv.set(wi, v);
                    wi++;
                }, bld);
        } else {
            VST* dv = CO::vals_mut(leaf);
            walk_collect_and_dealloc(tag_bitmask(node), walk_shift,
                [&](K key, VST v) {
                    dk[wi] = key;
                    VT::init_slot(&dv[wi], v);
                    wi++;
                }, bld);
        }

        // Find next entry after erased key
        iter_entry_t<K> nx{};
        const K* kd = CO::keys(leaf, hu);
        unsigned entries = lh->entries();
        const K* base = adaptive_search<K>::find_base_first(kd, entries, stored);
        std::uint16_t idx = static_cast<std::uint16_t>(base - kd);
        if (idx < entries)
            nx = CO::entry_at_pos(leaf, idx);

        return {tag_leaf(leaf), true, static_cast<std::uint64_t>(total_entries), nx};
    }

    // ==================================================================
    // dealloc_subtree — free everything
    // ==================================================================

    static void dealloc_subtree(std::uint64_t tagged, unsigned shift, BLD& bld) {
        if (tagged & LEAF_BIT) {
            if (tagged & NOT_FOUND_BIT) return;
            std::uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            if (hdr->is_bitmap())
                BO::bitmap_destroy_and_dealloc(node, bld);
            else
                CO::destroy_and_dealloc(node, bld);
            return;
        }

        std::uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();
        unsigned child_shift = shift - (sc + 1) * CHAR_BIT;

        BO::chain_for_each_child(node, sc, [&](std::uint64_t child) {
            dealloc_subtree(child, child_shift, bld);
        });
        bld.dealloc_node(node, hdr->alloc_u64());
    }

    // Free node structures only — values transferred elsewhere (used by coalesce in impl)
    static void dealloc_subtree_nodes_only(std::uint64_t tagged, unsigned shift, BLD& bld) {
        if (tagged & LEAF_BIT) {
            if (tagged & NOT_FOUND_BIT) return;
            std::uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            if (hdr->is_bitmap())
                BO::bitmap_dealloc_node_only(node, bld);
            else
                CO::dealloc_node_only(node, bld);
            return;
        }

        std::uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();
        unsigned child_shift = shift - (sc + 1) * CHAR_BIT;

        BO::chain_for_each_child(node, sc, [&](std::uint64_t child) {
            dealloc_subtree_nodes_only(child, child_shift, bld);
        });
        bld.dealloc_node(node, hdr->alloc_u64());
    }

    // ==================================================================
    // entry_from_insert_pos — build iter_entry_t from insert result
    // ==================================================================

    static iter_entry_t<K> entry_from_insert_pos(std::uint64_t* leaf, std::uint16_t pos,
                                                  K stored) noexcept {
        auto* hdr = get_header(leaf);
        if (hdr->is_bitmap()) {
            return {leaf, pos, pos, stored,
                    BO::bm_val_ptr(leaf, BITMAP_LEAF_HEADER_U64,
                        VT::IS_BOOL ? 0 : static_cast<int>(pos)),
                    true};
        } else {
            return CO::entry_at_pos(leaf, pos);
        }
    }

    // ==================================================================
    // leaf_find_ge — find first entry >= stored (for lower_bound)
    // ==================================================================

    static iter_entry_t<K> leaf_find_ge(std::uint64_t* node, K stored) noexcept {
        auto* hdr = get_header(node);
        if (hdr->is_bitmap()) {
            std::uint8_t suffix = static_cast<std::uint8_t>(stored & 0xFF);
            constexpr std::size_t hs = BITMAP_LEAF_HEADER_U64;
            const bitmap_256_t& bmp = BO::bm(node, hs);
            // Find first set bit >= suffix
            if (bmp.has_bit(suffix)) {
                int slot = bmp.find_slot<slot_mode::UNFILTERED>(suffix);
                K base_key = BO::read_base_key(node);
                K key = base_key | K(suffix);
                std::uint16_t ret_pos = VT::IS_BOOL ? static_cast<std::uint16_t>(suffix)
                                                     : static_cast<std::uint16_t>(slot);
                return {node, ret_pos, static_cast<std::uint16_t>(suffix),
                        key, BO::bm_val_ptr(node, hs, slot), true};
            }
            auto nxt = bmp.next_bit_after(suffix);
            if (!nxt.found) return {};
            K base_key = BO::read_base_key(node);
            K key = base_key | K(nxt.idx);
            int slot = bmp.find_slot<slot_mode::UNFILTERED>(nxt.idx);
            std::uint16_t ret_pos = VT::IS_BOOL ? static_cast<std::uint16_t>(nxt.idx)
                                                 : static_cast<std::uint16_t>(slot);
            return {node, ret_pos, static_cast<std::uint16_t>(nxt.idx),
                    key, BO::bm_val_ptr(node, hs, slot), true};
        } else {
            const K* kd = CO::keys(node, COMPACT_HEADER_U64);
            unsigned entries = hdr->entries();
            const K* base = adaptive_search<K>::find_base_first(kd, entries, stored);
            std::uint16_t pos = static_cast<std::uint16_t>(base - kd);
            if (pos < entries && kd[pos] < stored) ++pos;
            if (pos >= entries) return {};
            return CO::entry_at_pos(node, pos);
        }
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_OPS_HPP
