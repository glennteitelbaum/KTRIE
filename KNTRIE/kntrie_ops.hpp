#ifndef KNTRIE_OPS_HPP
#define KNTRIE_OPS_HPP

#include "kntrie_bitmask.hpp"
#include "kntrie_compact.hpp"

#include <array>
#include <bit>

namespace gteitelbaum::kntrie_detail {

// ======================================================================
// kntrie_ops<VALUE, ALLOC, KEY_BITS> — stateless trie operations.
//
// All functions take uint64_t ik — root-level, left-aligned in u64.
// ik is NEVER shifted during descent. Each level extracts its byte via
// constexpr shift: ik >> byte_shift<BITS>().
//
// KEY_BITS: total key bits (16/32/64). Lets leaf_ops_t compute depth.
// BITS: compile-time remaining key bits at this tree level.
// NK narrowing eliminated — NK only at leaf storage boundary.
// ======================================================================

template<typename VALUE, typename ALLOC, int KEY_BITS>
struct kntrie_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;


    // ==================================================================
    // byte_shift<BITS>: right-shift to extract current byte from root ik.
    //   DEPTH = (KEY_BITS - BITS) / CHAR_BIT
    //   Byte at depth D occupies bits [63-8D .. 56-8D].
    //   shift = 56 - 8*D = 64 - KEY_BITS + BITS - 8
    // ==================================================================

    template<int BITS>
    static constexpr int byte_shift() { return static_cast<int>(U64_BITS) - KEY_BITS + BITS - CHAR_BIT; }

    // Safe prefix mask — avoids UB when shift >= 64 (BITS == KEY_BITS, no prefix)
    template<int BITS_>
    static constexpr uint64_t safe_prefix_mask() noexcept {
        constexpr int S = static_cast<int>(U64_BITS) - KEY_BITS + BITS_;
        if constexpr (S >= static_cast<int>(U64_BITS)) return 0;
        else return ~0ULL << S;
    }

    template<int BITS>
    static uint8_t extract_byte(uint64_t ik) noexcept {
        return static_cast<uint8_t>(ik >> byte_shift<BITS>());
    }

    // ==================================================================
    // depth_switch: runtime depth → compile-time BITS dispatch.
    // depth is absolute byte position (0 = first byte of key).
    // BITS = KEY_BITS - CHAR_BIT * depth.
    // ==================================================================

    static constexpr int MAX_DEPTH = KEY_BITS / CHAR_BIT;

    template<typename F>
    static decltype(auto) depth_switch(uint8_t depth, F&& fn_) {
        switch (depth) {
        case 0: return fn_.template operator()<KEY_BITS>();
        case 1: return fn_.template operator()<KEY_BITS - 1 * CHAR_BIT>();
        case 2: if constexpr (MAX_DEPTH > 2) return fn_.template operator()<KEY_BITS - 2 * CHAR_BIT>(); [[fallthrough]];
        case 3: if constexpr (MAX_DEPTH > 3) return fn_.template operator()<KEY_BITS - 3 * CHAR_BIT>(); [[fallthrough]];
        case 4: if constexpr (MAX_DEPTH > 4) return fn_.template operator()<KEY_BITS - 4 * CHAR_BIT>(); [[fallthrough]];
        case 5: if constexpr (MAX_DEPTH > 5) return fn_.template operator()<KEY_BITS - 5 * CHAR_BIT>(); [[fallthrough]];
        case 6: if constexpr (MAX_DEPTH > 6) return fn_.template operator()<KEY_BITS - 6 * CHAR_BIT>(); [[fallthrough]];
        case 7: if constexpr (MAX_DEPTH > 7) return fn_.template operator()<KEY_BITS - 7 * CHAR_BIT>(); [[fallthrough]];
        default: std::unreachable();
        }
    }

    // ==================================================================
    // leaf_ops_t<BITS> — suffix extraction helpers.
    // Knows KEY_BITS from enclosing kntrie_ops.
    // All functions receive root-level ik.
    // leaf_prefix(node) is left-aligned in node[3].
    // ==================================================================

    template<int BITS>
    struct leaf_ops_t {
        static constexpr int MAX_LEAF_SKIP = (BITS - CHAR_BIT) / CHAR_BIT;

        // --- to_suffix: extract suffix from root-level ik ---
        // REMAINING = BITS - 8*SKIP = bits left after depth+skip
        // Suffix occupies REMAINING bits starting after
        // (KEY_BITS - REMAINING) consumed bits.
        template<int REMAINING>
        static auto to_suffix(uint64_t ik) noexcept {
            using SNK = nk_for_bits_t<REMAINING>;
            constexpr int SNK_BITS = static_cast<int>(sizeof(SNK) * CHAR_BIT);
            constexpr int CONSUMED = KEY_BITS - REMAINING;
            return static_cast<SNK>((ik << CONSUMED) >> (U64_BITS - SNK_BITS));
        }

        // --- suffix_to_u64: place suffix at root-level position ---
        // Result OR'd with (prefix >> DEPTH) to get full root-level key.
        template<int REMAINING, typename SUF>
        static uint64_t suffix_to_u64(SUF suf) noexcept {
            constexpr int SUF_BITS = static_cast<int>(sizeof(SUF) * CHAR_BIT);
            constexpr int CONSUMED = KEY_BITS - REMAINING;
            // Left-align in u64, then shift right to root-level position
            return (static_cast<uint64_t>(suf) << (U64_BITS - SUF_BITS)) >> CONSUMED;
        }
    };

    // ==================================================================
    // make_depth<BITS> — construct depth_t from compile-time BITS + runtime skip
    // ==================================================================

    template<int BITS>
    static depth_t make_depth(uint8_t skip = 0) noexcept {
        int remaining = BITS - CHAR_BIT * skip;
        uint8_t nk_bits;
        if      (remaining > U32_BITS) nk_bits = U64_BITS;
        else if (remaining > U16_BITS) nk_bits = U32_BITS;
        else if (remaining > U8_BITS)  nk_bits = U16_BITS;
        else                           nk_bits = U8_BITS;
        return depth_t{skip > 0, static_cast<uint16_t>(skip),
                       static_cast<uint16_t>(KEY_BITS - remaining),
                       static_cast<uint16_t>(U64_BITS - nk_bits)};
    }

    // ==================================================================
    // set_leaf_fns_for: set all 3 fn pointers from node's actual data NK
    // Reads NK from depth_t.shift (data layout), NOT from template BITS.
    // ==================================================================

    template<int BITS>
    static void set_leaf_fns_for(uint64_t* node) noexcept {
        depth_t d = get_depth(node);
        uint8_t nk_bits = U64_BITS - d.shift;
        bool has_skip = d.skip > 0;

        if (nk_bits <= U8_BITS) {
            // bitmap
            if (has_skip) {
                set_find_fn(node, &BO::template find_fn_bitmap<true>);
                set_find_adv(node, &BO::template find_adv_fn_bitmap<true>);
            } else {
                set_find_fn(node, &BO::template find_fn_bitmap<false>);
                set_find_adv(node, &BO::template find_adv_fn_bitmap<false>);
            }
            set_find_edge(node, &BO::find_edge_fn_bitmap);
        } else if (nk_bits <= U16_BITS) {
            using CO = compact_ops<uint16_t, VALUE, ALLOC>;
            CO::set_leaf_fns(node, has_skip);
        } else if (nk_bits <= U32_BITS) {
            using CO = compact_ops<uint32_t, VALUE, ALLOC>;
            CO::set_leaf_fns(node, has_skip);
        } else {
            using CO = compact_ops<uint64_t, VALUE, ALLOC>;
            CO::set_leaf_fns(node, has_skip);
        }
    }
    // ==================================================================
    // reconstruct_ik — rebuild full u64 IK from leaf + position
    // ==================================================================

    static uint64_t reconstruct_ik(const uint64_t* node, uint16_t pos) noexcept {
        depth_t d = get_depth(node);
        uint64_t pfx = leaf_prefix(node);
        constexpr size_t hs = LEAF_HEADER_U64;
        uint8_t nk_bits = U64_BITS - d.shift;
        if (nk_bits <= U8_BITS) {
            // bitmap leaf — pos is the bit index (byte value)
            return d.to_ik(pfx, static_cast<uint64_t>(pos));
        } else if (nk_bits <= U16_BITS) {
            auto suf = reinterpret_cast<const uint16_t*>(node + hs)[pos];
            return d.to_ik(pfx, static_cast<uint64_t>(suf));
        } else if (nk_bits <= U32_BITS) {
            auto suf = reinterpret_cast<const uint32_t*>(node + hs)[pos];
            return d.to_ik(pfx, static_cast<uint64_t>(suf));
        } else {
            auto suf = reinterpret_cast<const uint64_t*>(node + hs)[pos];
            return d.to_ik(pfx, suf);
        }
    }

    // ==================================================================
    // value_ref_at — mutable VALUE& from leaf + pos for live iterators.
    // For bitmap leaves, pos is the byte value; slot computed via popcount.
    // ==================================================================

    static VALUE& value_ref_at(uint64_t* node, uint16_t pos) noexcept {
        depth_t d = get_depth(node);
        constexpr size_t hs = LEAF_HEADER_U64;
        uint8_t nk_bits = U64_BITS - d.shift;
        if (nk_bits <= U8_BITS) {
            // bitmap leaf: pos is byte value, compute slot
            int slot = BO::bm(node, hs).template find_slot<slot_mode::FAST_EXIT>(
                static_cast<uint8_t>(pos));
            return BO::bl_val_ref_at(node, hs, slot);
        }
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
    }

    static const VALUE& value_cref_at(const uint64_t* node, uint16_t pos) noexcept {
        return value_ref_at(const_cast<uint64_t*>(node), pos);
    }

    // bool_ref_at — packed bit proxy for IS_BOOL live iterators.
    static bool_ref bool_ref_at(uint64_t* node, uint16_t pos) noexcept {
        depth_t d = get_depth(node);
        constexpr size_t hs = LEAF_HEADER_U64;
        uint8_t nk_bits = U64_BITS - d.shift;
        if (nk_bits <= U8_BITS) {
            // bitmap leaf: pos is byte value, val_bm is after key bitmap
            auto& vbm = BO::val_bm_mut(node, hs);
            unsigned word_idx = pos / U64_BITS;
            uint8_t  bit_idx  = static_cast<uint8_t>(pos % U64_BITS);
            return {&vbm.words[word_idx], bit_idx};
        }
        // compact leaf: packed bits in bool_slots after keys
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
    }

    // ==================================================================
    // Make single leaf — narrow to storage NK at boundary
    // ==================================================================

    template<int BITS> requires (BITS >= U8_BITS)
    static uint64_t* make_single_leaf(uint64_t ik, VST value, BLD& bld) {
        uint64_t* node;
        if constexpr (BITS <= U8_BITS) {
            node = BO::make_single_bitmap(extract_byte<BITS>(ik), value, bld);
        } else {
            using SNK = nk_for_bits_t<BITS>;
            SNK suffix = leaf_ops_t<BITS>::template to_suffix<BITS>(ik);
            using CO = compact_ops<SNK, VALUE, ALLOC>;
            node = CO::make_leaf(&suffix, &value, 1, bld);
        }
        init_leaf_fns<BITS>(node, ik);
        return node;
    }

    // Recursively descend `depth` bytes (consuming BITS), then create leaf.
    // Returns {leaf, pos} where pos is the position of the sole entry.
    struct leaf_and_pos { uint64_t* leaf; uint16_t pos; };
    static leaf_and_pos make_leaf_descended(uint64_t ik, VST value,
                                             uint8_t target_depth,
                                             BLD& bld) {
        return depth_switch(target_depth, [&]<int BITS>() -> leaf_and_pos {
            auto* leaf = make_single_leaf<BITS>(ik, value, bld);
            return {leaf, single_entry_pos<BITS>(ik)};
        });
    }

    // Position of the sole entry in a freshly created single-entry leaf.
    // Compact: always pos 0. Bitmap: pos = byte value.
    template<int BITS>
    static uint16_t single_entry_pos(uint64_t ik) noexcept {
        if constexpr (BITS <= U8_BITS) {
            return static_cast<uint16_t>(extract_byte<BITS>(ik));
        } else {
            return 0;
        }
    }

    // ==================================================================
    // Leaf iterate / build helpers
    // ==================================================================

    template<int BITS, typename Fn>
    static void leaf_for_each(const uint64_t* node, const node_header_t* hdr,
                                Fn&& cb) {
        using NK = nk_for_bits_t<BITS>;
        if constexpr (sizeof(NK) == sizeof(uint8_t)) {
            BO::for_each_bitmap(node, [&](uint8_t s, VST v) {
                cb(static_cast<NK>(s), v);
            });
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            CO::for_each(node, hdr,
                [&](NK s, const VST& v) { cb(s, v); });
        }
    }

    template<int BITS>
    static uint64_t* build_leaf(nk_for_bits_t<BITS>* suf, VST* vals,
                                  size_t count, uint64_t ik, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        uint64_t* node;
        if constexpr (sizeof(NK) == sizeof(uint8_t)) {
            node = BO::make_bitmap_leaf(reinterpret_cast<uint8_t*>(suf), vals,
                static_cast<uint32_t>(count), bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            node = CO::make_leaf(suf, vals, static_cast<uint32_t>(count), bld);
        }
        init_leaf_fns<BITS>(node, ik);
        return node;
    }

    // Build node from sorted arrays. Returns tagged pointer.
    template<int BITS>
    static uint64_t build_node_from_arrays_tagged(nk_for_bits_t<BITS>* suf,
                                                     VST* vals,
                                                     size_t count, uint64_t ik,
                                                     BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);

        // Leaf case
        if (count <= COMPACT_MAX)
            return tag_leaf(build_leaf<BITS>(suf, vals, count, ik, bld));

        // Skip compression: all entries share same top byte?
        uint8_t first_top = static_cast<uint8_t>(suf[0] >> (NK_BITS - CHAR_BIT));
        bool all_same = true;
        for (size_t i = 1; i < count; ++i)
            if (static_cast<uint8_t>(suf[i] >> (NK_BITS - CHAR_BIT)) != first_top)
                { all_same = false; break; }

        if (all_same && BITS > U8_BITS) {
            if constexpr (BITS > U8_BITS) {
            using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);
            auto cs = std::make_unique<CNK[]>(count);
            for (size_t i = 0; i < count; ++i) {
                NK shifted = static_cast<NK>(suf[i] << CHAR_BIT);
                cs[i] = static_cast<CNK>(shifted >> (NK_BITS - CNK_BITS));
            }

            uint64_t child_tagged = build_node_from_arrays_tagged<BITS - CHAR_BIT>(
                cs.get(), vals, count, ik, bld);

            uint8_t byte_arr[1] = {first_top};
            if (child_tagged & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(child_tagged);
                leaf = prepend_skip<BITS>(leaf, 1, ik, bld);
                return tag_leaf(leaf);
            }
            auto* bm_node = bm_to_node(child_tagged);
            return BO::wrap_in_chain(bm_node, byte_arr, 1, bld);
            }
        }

        // Multi-child bitmask
        uint8_t indices[BYTE_VALUES];
        uint64_t child_tagged[BYTE_VALUES];
        int n_children = 0;

        size_t i = 0;
        while (i < count) {
            uint8_t ti = static_cast<uint8_t>(suf[i] >> (NK_BITS - CHAR_BIT));
            size_t start = i;
            while (i < count && static_cast<uint8_t>(suf[i] >> (NK_BITS - CHAR_BIT)) == ti) ++i;
            size_t cc = i - start;

            if constexpr (BITS > U8_BITS) {
                using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
                constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);
                auto cs = std::make_unique<CNK[]>(cc);
                for (size_t j = 0; j < cc; ++j) {
                    NK shifted = static_cast<NK>(suf[start + j] << CHAR_BIT);
                    cs[j] = static_cast<CNK>(shifted >> (NK_BITS - CNK_BITS));
                }
                // Reconstruct root-level ik from this group's first suffix
                uint64_t child_ik = (ik & safe_prefix_mask<BITS>())
                    | leaf_ops_t<BITS>::template suffix_to_u64<BITS>(suf[start]);
                child_tagged[n_children] = build_node_from_arrays_tagged<BITS - CHAR_BIT>(
                    cs.get(), vals + start, cc, child_ik, bld);
            }
            indices[n_children] = ti;
            n_children++;
        }

        return tag_bitmask(
            BO::make_bitmask(indices, child_tagged, n_children, bld, count));
    }

    // ==================================================================
    // prepend_skip / remove_skip — no realloc, sets depth + prefix.
    // prefix is LEFT-ALIGNED in node[3] (skip bytes at top of u64).
    // ==================================================================

    template<int BITS>
    static uint64_t* prepend_skip(uint64_t* node, uint8_t new_len,
                                    uint64_t ik, BLD&) {
        uint8_t old_skip = get_header(node)->skip();
        uint8_t new_skip = old_skip + new_len;
        uint64_t mask = ~0ULL << (U64_BITS - KEY_BITS + BITS - CHAR_BIT * new_skip);
        set_leaf_prefix(node, ik & mask);
        set_depth(node, make_depth<BITS>(new_skip));
        if (old_skip == 0) set_leaf_fns_for<BITS>(node);
        return node;
    }

    template<int BITS>
    static uint64_t* remove_skip(uint64_t* node, BLD&) {
        constexpr uint64_t MASK = safe_prefix_mask<BITS>();
        set_leaf_prefix(node, leaf_prefix(node) & MASK);
        bool had_skip = get_header(node)->skip() > 0;
        set_depth(node, make_depth<BITS>());
        if (had_skip) set_leaf_fns_for<BITS>(node);
        return node;
    }

    template<int BITS>
    static void init_leaf_fns(uint64_t* node, uint64_t ik) noexcept {
        // depth: always skip=0 at creation
        set_depth(node, make_depth<BITS>());
        // prefix
        constexpr uint64_t MASK = safe_prefix_mask<BITS>();
        set_leaf_prefix(node, ik & MASK);
        // slot fns: dispatch by data type
        set_leaf_fns_for<BITS>(node);
    }

    // ==================================================================
    // split_on_prefix: leaf skip prefix diverges.
    // ik is root-level. BITS already consumed by recursion.
    // ==================================================================

    template<int BITS> requires (BITS > U8_BITS)
    static insert_result_t split_on_prefix(uint64_t* node, node_header_t* hdr,
                                      uint64_t ik, VST value,
                                      uint8_t skip, uint8_t common,
                                      BLD& bld) {
        constexpr int BS = byte_shift<BITS>();
        uint8_t new_idx = static_cast<uint8_t>(ik >> BS);
        // Both ik and leaf_prefix(node) are root-level — BITS is at divergence level
        uint8_t old_idx = extract_byte<BITS>(leaf_prefix(node));
        uint8_t old_rem = skip - 1 - common;

        // Save common prefix bytes for wrap_in_chain.
        uint8_t saved_prefix[MAX_SKIP] = {};
        for (uint8_t i = 0; i < common; ++i)
            saved_prefix[i] = static_cast<uint8_t>(
                leaf_prefix(node) >> (BS + CHAR_BIT * (common - i)));

        // Update old node: strip consumed prefix, keep remainder
        if (old_rem > 0) [[unlikely]] {
            uint64_t mask = ~0ULL << (U64_BITS - KEY_BITS + BITS - CHAR_BIT - CHAR_BIT * old_rem);
            set_leaf_prefix(node, leaf_prefix(node) & mask);
            set_depth(node, make_depth<BITS - CHAR_BIT>(old_rem));
        } else {
            node = remove_skip<BITS - CHAR_BIT>(node, bld);
            hdr = get_header(node);
        }

        // Build new leaf at BITS-8 (one byte past divergence)
        constexpr uint8_t BASE_DEPTH = (KEY_BITS - BITS) / CHAR_BIT + 1;
        auto [new_leaf, new_pos] = make_leaf_descended(ik, value, BASE_DEPTH + old_rem, bld);
        if (old_rem > 0) [[unlikely]] {
            new_leaf = prepend_skip<BITS - CHAR_BIT>(new_leaf, old_rem, ik, bld);
        }

        // Create parent bitmask with 2 children
        uint8_t bi[2];
        uint64_t cp[2];
        if (new_idx < old_idx) {
            bi[0] = new_idx; cp[0] = tag_leaf(new_leaf);
            bi[1] = old_idx; cp[1] = tag_leaf(node);
        } else {
            bi[0] = old_idx; cp[0] = tag_leaf(node);
            bi[1] = new_idx; cp[1] = tag_leaf(new_leaf);
        }

        uint64_t total = BO::exact_subtree_count(cp[0]) +
                         BO::exact_subtree_count(cp[1]);
        auto* bm_node = BO::make_bitmask(bi, cp, 2, bld, total);
        uint64_t tagged;
        if (common > 0) [[unlikely]]
            tagged = BO::wrap_in_chain(bm_node, saved_prefix, common, bld);
        else
            tagged = tag_bitmask(bm_node);
        return {tagged, true, false, nullptr, new_leaf, new_pos};
    }

    // ==================================================================
    // split_skip_at: key diverges in bitmask skip chain.
    // ik is root-level.
    // ==================================================================

    template<int BITS> requires (BITS >= U8_BITS)
    static insert_result_t split_skip_at(uint64_t* node, node_header_t* hdr,
                                    uint8_t sc, uint8_t split_pos,
                                    uint64_t ik, VST value, BLD& bld) {
        uint8_t expected = extract_byte<BITS>(ik);
        uint8_t actual_byte = BO::skip_byte(node, split_pos);

        // Build new leaf — one BITS-level past the split point
        uint64_t* new_leaf;
        uint16_t new_pos;
        if constexpr (BITS > U8_BITS) {
            new_leaf = make_single_leaf<BITS - CHAR_BIT>(ik, value, bld);
            new_pos = single_entry_pos<BITS - CHAR_BIT>(ik);
        } else {
            new_leaf = make_single_leaf<BITS>(ik, value, bld);
            new_pos = single_entry_pos<BITS>(ik);
        }
        uint64_t new_leaf_tagged = tag_leaf(new_leaf);

        // Build remainder from [split_pos+1..sc-1] + final bitmask
        uint64_t remainder = BO::build_remainder(node, sc, split_pos + 1, bld);

        // Create 2-child bitmask at split point
        uint8_t bi[2];
        uint64_t cp[2];
        if (expected < actual_byte) {
            bi[0] = expected;    cp[0] = new_leaf_tagged;
            bi[1] = actual_byte; cp[1] = remainder;
        } else {
            bi[0] = actual_byte; cp[0] = remainder;
            bi[1] = expected;    cp[1] = new_leaf_tagged;
        }
        uint64_t total = BO::exact_subtree_count(cp[0]) +
                         BO::exact_subtree_count(cp[1]);
        auto* split_node = BO::make_bitmask(bi, cp, 2, bld, total);

        // Wrap in skip chain for prefix bytes [0..split_pos-1]
        uint64_t result;
        if (split_pos > 0) [[unlikely]] {
            uint8_t prefix_bytes[MAX_SKIP];
            BO::skip_bytes(node, split_pos, prefix_bytes);
            result = BO::wrap_in_chain(split_node, prefix_bytes, split_pos, bld);
        } else {
            result = tag_bitmask(split_node);
        }

        bld.dealloc_node(node, hdr->alloc_u64());
        return {result, true, false, nullptr, new_leaf, new_pos};
    }

    // ==================================================================
    // convert_to_bitmask_tagged — compact leaf overflow
    // ==================================================================

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
        leaf_pos_t lp;
        if (child_tagged & LEAF_BIT) {
            uint64_t* leaf = untag_leaf_mut(child_tagged);
            lp = get_find_fn(leaf)(leaf, ik);
        } else {
            // Descent through bitmask (possibly skip chain).
            // Shifted starts at the subtree root: BITS bytes consumed
            // minus ps skip bytes now inside the chain.
            constexpr int CONSUMED_BITS = KEY_BITS - BITS;
            int shift_amt = CONSUMED_BITS - ps * CHAR_BIT;
            uint64_t shifted = (shift_amt > 0) ? (ik << shift_amt) : ik;
            lp = BO::find_loop(child_tagged, ik, shifted);
        }
        return {child_tagged, true, false, nullptr, lp.node, lp.pos};
    }

    // ==================================================================
    // Insert — runtime-recursive, depth-based
    // ==================================================================

    template<bool INSERT, bool ASSIGN>
    static insert_result_t insert_node(uint64_t ptr, uint64_t ik,
                                        uint64_t shifted, uint8_t depth,
                                        VST value, BLD& bld) {
        // SENTINEL
        if (ptr == BO::SENTINEL_TAGGED) [[unlikely]] {
            if constexpr (!INSERT) return {ptr, false, false};
            return depth_switch(depth, [&]<int BITS>() -> insert_result_t {
                auto* leaf = make_single_leaf<BITS>(ik, value, bld);
                uint16_t pos = single_entry_pos<BITS>(ik);
                return {tag_leaf(leaf), true, false, nullptr, leaf, pos};
            });
        }

        // LEAF
        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();

            // Walk leaf skip bytes
            uint64_t pfx_shifted = leaf_prefix(node) << (CHAR_BIT * depth);
            for (uint8_t pos = 0; pos < skip; ++pos) {
                uint8_t expected = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);
                uint8_t actual = static_cast<uint8_t>(pfx_shifted >> U64_TOP_BYTE_SHIFT);
                if (expected != actual) [[unlikely]] {
                    if constexpr (!INSERT) return {tag_leaf(node), false, false};
                    return depth_switch(depth, [&]<int BITS>() -> insert_result_t {
                        if constexpr (BITS > U8_BITS)
                            return split_on_prefix<BITS>(node, hdr, ik, value,
                                                          skip, pos, bld);
                        else
                            std::unreachable();
                    });
                }
                shifted <<= CHAR_BIT;
                pfx_shifted <<= CHAR_BIT;
                depth++;
            }

            // At leaf body
            return depth_switch(depth, [&]<int BITS>() {
                return leaf_insert<BITS, INSERT, ASSIGN>(
                    node, hdr, ik, value, bld);
            });
        }

        // BITMASK
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        // Walk chain skip bytes
        for (uint8_t pos = 0; pos < sc; ++pos) {
            uint8_t expected = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);
            uint8_t actual = BO::skip_byte(node, pos);
            if (expected != actual) [[unlikely]] {
                if constexpr (!INSERT) return {tag_bitmask(node), false, false};
                return depth_switch(depth, [&]<int BITS>() -> insert_result_t {
                    return split_skip_at<BITS>(node, hdr, sc, pos,
                                                ik, value, bld);
                });
            }
            shifted <<= CHAR_BIT;
            depth++;
        }

        // Final bitmap lookup
        uint8_t ti = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]] {
            if constexpr (!INSERT) return {tag_bitmask(node), false, false};

            uint64_t* new_leaf = nullptr;
            uint16_t new_pos = 0;
            auto leaf_tagged = depth_switch(depth + 1, [&]<int BITS>() {
                new_leaf = make_single_leaf<BITS>(ik, value, bld);
                new_pos = single_entry_pos<BITS>(ik);
                return tag_leaf(new_leaf);
            });

            uint64_t* nn;
            if (sc > 0) [[unlikely]]
                nn = BO::chain_add_child(node, hdr, sc, ti, leaf_tagged, bld);
            else
                nn = BO::add_child(node, hdr, ti, leaf_tagged, bld);
            inc_descendants(nn, get_header(nn));
            return {tag_bitmask(nn), true, false, nullptr, new_leaf, new_pos};
        }

        // Recurse into child
        auto cr = insert_node<INSERT, ASSIGN>(
            cl.child, ik, shifted << CHAR_BIT, depth + 1, value, bld);

        // Parent fixup on unwind
        if (cr.tagged_ptr != cl.child) [[unlikely]] {
            if (sc > 0) [[unlikely]]
                BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
            else
                BO::set_child(node, cl.slot, cr.tagged_ptr);
        }
        if (cr.inserted) [[likely]]
            inc_descendants(node, hdr);
        return {tag_bitmask(node), cr.inserted, false};
    }

    // --- Leaf insert: compile-time NK dispatch ---
    template<int BITS, bool INSERT, bool ASSIGN>
    static insert_result_t leaf_insert(uint64_t* node, node_header_t* hdr,
                                         uint64_t ik, VST value, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        NK suffix = leaf_ops_t<BITS>::template to_suffix<BITS>(ik);

        insert_result_t result;
        if constexpr (sizeof(NK) == sizeof(uint8_t)) {
            result = BO::template bitmap_insert<INSERT, ASSIGN>(
                node, static_cast<uint8_t>(suffix), value, bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            result = CO::template insert<INSERT, ASSIGN>(
                node, hdr, suffix, value, bld);
        }
        if (result.needs_split) [[unlikely]] {
            if constexpr (!INSERT) return {tag_leaf(node), false, false};
            return convert_to_bitmask_tagged<BITS>(node, hdr, ik, value, bld);
        }
        return result;
    }
    // ==================================================================
    // Erase — runtime-recursive, depth-based
    // ==================================================================

    static erase_result_t erase_node(uint64_t ptr, uint64_t ik,
                                      uint64_t shifted, uint8_t depth,
                                      BLD& bld) {
        if (ptr == BO::SENTINEL_TAGGED) [[unlikely]]
            return {ptr, false, 0};

        // LEAF
        if (ptr & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(ptr);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();

            // Walk leaf skip bytes
            uint64_t pfx_shifted = leaf_prefix(node) << (CHAR_BIT * depth);
            for (uint8_t pos = 0; pos < skip; ++pos) {
                uint8_t expected = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);
                uint8_t actual = static_cast<uint8_t>(pfx_shifted >> U64_TOP_BYTE_SHIFT);
                if (expected != actual) [[unlikely]]
                    return {tag_leaf(node), false, 0};
                shifted <<= CHAR_BIT;
                pfx_shifted <<= CHAR_BIT;
                depth++;
            }

            // At leaf body
            return depth_switch(depth, [&]<int BITS>() {
                return leaf_erase<BITS>(node, hdr, ik, bld);
            });
        }

        // BITMASK
        uint64_t* node = bm_to_node(ptr);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();

        // Walk chain skip bytes
        for (uint8_t pos = 0; pos < sc; ++pos) {
            uint8_t expected = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);
            uint8_t actual = BO::skip_byte(node, pos);
            if (expected != actual) [[unlikely]]
                return {tag_bitmask(node), false, 0};
            shifted <<= CHAR_BIT;
            depth++;
        }

        // Final bitmap lookup
        uint8_t ti = static_cast<uint8_t>(shifted >> U64_TOP_BYTE_SHIFT);

        typename BO::child_lookup cl;
        if (sc > 0) [[unlikely]]
            cl = BO::chain_lookup(node, sc, ti);
        else
            cl = BO::lookup(node, ti);

        if (!cl.found) [[unlikely]]
            return {tag_bitmask(node), false, 0};

        // Recurse into child
        auto cr = erase_node(cl.child, ik, shifted << CHAR_BIT, depth + 1, bld);

        if (!cr.erased) [[unlikely]]
            return {tag_bitmask(node), false, 0};

        if (cr.tagged_ptr) [[likely]] {
            // Child still exists — update pointer if changed
            if (cr.tagged_ptr != cl.child) [[unlikely]] {
                if (sc > 0) [[unlikely]]
                    BO::chain_set_child(node, sc, cl.slot, cr.tagged_ptr);
                else
                    BO::set_child(node, cl.slot, cr.tagged_ptr);
            }
            uint64_t exact = dec_descendants(node, hdr);
            if (exact <= COMPACT_MAX) [[unlikely]] {
                return depth_switch(depth, [&]<int BITS>() {
                    return do_coalesce<BITS>(node, hdr, bld);
                });
            }
            return {tag_bitmask(node), true, exact};
        }

        // Child fully erased — remove from bitmap
        uint64_t* nn;
        if (sc > 0) [[unlikely]]
            nn = BO::chain_remove_child(node, hdr, sc, cl.slot, ti, bld);
        else
            nn = BO::remove_child(node, hdr, cl.slot, ti, bld);
        if (!nn) [[unlikely]] return {0, true, 0};

        hdr = get_header(nn);
        unsigned nc = hdr->entries();
        uint64_t exact = dec_descendants(nn, hdr);

        // Collapse when final bitmask drops to 1 child
        if (nc == 1) [[unlikely]] {
            typename BO::collapse_info ci;
            if (sc > 0) [[unlikely]]
                ci = BO::chain_collapse_info(nn, sc);
            else
                ci = BO::standalone_collapse_info(nn);
            size_t nn_au64 = hdr->alloc_u64();

            if (ci.sole_child & LEAF_BIT) {
                uint64_t* leaf = untag_leaf_mut(ci.sole_child);
                leaf = depth_switch(depth, [&]<int BITS>() {
                    return prepend_skip<BITS>(leaf, ci.total_skip,
                               leaf_prefix(leaf), bld);
                });
                bld.dealloc_node(nn, nn_au64);
                return {tag_leaf(leaf), true, exact};
            }
            uint64_t* child_node = bm_to_node(ci.sole_child);
            bld.dealloc_node(nn, nn_au64);
            return {BO::wrap_in_chain(child_node, ci.bytes, ci.total_skip, bld),
                    true, exact};
        }

        if (exact <= COMPACT_MAX) [[unlikely]] {
            return depth_switch(depth, [&]<int BITS>() {
                return do_coalesce<BITS>(nn, get_header(nn), bld);
            });
        }
        return {tag_bitmask(nn), true, exact};
    }

    template<int BITS>
    static erase_result_t leaf_erase(uint64_t* node, node_header_t* hdr,
                                       uint64_t ik, BLD& bld) {
        using NK = nk_for_bits_t<BITS>;
        NK suffix = leaf_ops_t<BITS>::template to_suffix<BITS>(ik);

        if constexpr (sizeof(NK) == sizeof(uint8_t)) {
            return BO::bitmap_erase(node, static_cast<uint8_t>(suffix), bld);
        } else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            return CO::erase(node, hdr, suffix, bld);
        }
    }

    // ==================================================================
    // Collect entries — all use typed NK, narrow at leaf
    // ==================================================================

    struct collected_t {
        std::unique_ptr<nk_for_bits_t<U8_BITS>[]> keys;
        std::unique_ptr<VST[]> vals;
        size_t count;
    };

    template<int BITS>
    struct collected_typed_t {
        using NK = nk_for_bits_t<BITS>;
        std::unique_ptr<NK[]> keys;
        std::unique_ptr<VST[]> vals;
        size_t count;
        uint64_t rep_key;  // any surviving leaf's node[3] — full root-level key
    };

    template<int BITS> requires (BITS >= U8_BITS)
    static collected_typed_t<BITS> collect_entries(uint64_t tagged) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);

        if (tagged & LEAF_BIT) [[unlikely]] {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            if (skip) [[unlikely]]
                return collect_leaf_skip<BITS>(node, hdr, skip, 0);
            return collect_leaf<BITS>(node, hdr);
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc > 0) [[unlikely]]
            return collect_bm_skip<BITS>(node, sc, 0);
        return collect_bm_final<BITS>(node, 0);
    }

    template<int BITS> requires (BITS >= U8_BITS)
    static collected_typed_t<BITS> collect_leaf(const uint64_t* node,
                                                  const node_header_t* hdr) {
        using NK = nk_for_bits_t<BITS>;
        size_t n = hdr->entries();
        auto wk = std::make_unique<NK[]>(n);
        auto wv = std::make_unique<VST[]>(n);
        size_t wi = 0;
        leaf_for_each<BITS>(node, hdr, [&](NK s, VST v) {
            wk[wi] = s; wv[wi] = v; wi++;
        });
        return {std::move(wk), std::move(wv), wi, leaf_prefix(node)};
    }

    template<int BITS> requires (BITS >= U8_BITS)
    static collected_typed_t<BITS> collect_leaf_skip(const uint64_t* node,
                                                       const node_header_t* hdr,
                                                       uint8_t skip, uint8_t pos) {
        if (pos >= skip) [[unlikely]]
            return collect_leaf<BITS>(node, hdr);

        if constexpr (BITS > U8_BITS) {
            using NK = nk_for_bits_t<BITS>;
            constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
            uint8_t byte = extract_byte<BITS>(leaf_prefix(node));
            auto child = collect_leaf_skip<BITS - CHAR_BIT>(node, hdr, skip, pos + 1);

            using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);
            auto wk = std::make_unique<NK[]>(child.count);
            auto wv = std::make_unique<VST[]>(child.count);
            for (size_t i = 0; i < child.count; ++i) {
                wk[i] = (NK(byte) << (NK_BITS - CHAR_BIT))
                       | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (U64_BITS - CNK_BITS) >> (U64_BITS - NK_BITS + CHAR_BIT));
                wv[i] = child.vals[i];
            }
            return {std::move(wk), std::move(wv), child.count, child.rep_key};
        }
        std::unreachable();
    }

    template<int BITS> requires (BITS >= U8_BITS)
    static collected_typed_t<BITS> collect_bm_skip(const uint64_t* node,
                                                      uint8_t sc, uint8_t pos) {
        if (pos >= sc) [[unlikely]]
            return collect_bm_final<BITS>(node, sc);

        if constexpr (BITS > U8_BITS) {
            using NK = nk_for_bits_t<BITS>;
            constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
            uint8_t byte = BO::skip_byte(node, pos);
            auto child = collect_bm_skip<BITS - CHAR_BIT>(node, sc, pos + 1);

            using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
            constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);
            auto wk = std::make_unique<NK[]>(child.count);
            auto wv = std::make_unique<VST[]>(child.count);
            for (size_t i = 0; i < child.count; ++i) {
                wk[i] = (NK(byte) << (NK_BITS - CHAR_BIT))
                       | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (U64_BITS - CNK_BITS) >> (U64_BITS - NK_BITS + CHAR_BIT));
                wv[i] = child.vals[i];
            }
            return {std::move(wk), std::move(wv), child.count, child.rep_key};
        }
        std::unreachable();
    }

    template<int BITS> requires (BITS >= U8_BITS)
    static collected_typed_t<BITS> collect_bm_final(const uint64_t* node, uint8_t sc) {
        using NK = nk_for_bits_t<BITS>;
        constexpr int NK_BITS = static_cast<int>(sizeof(NK) * CHAR_BIT);
        auto* hdr = get_header(node);
        uint64_t total = BO::chain_descendants(node, sc, hdr->entries());

        auto wk = std::make_unique<NK[]>(total);
        auto wv = std::make_unique<VST[]>(total);
        size_t wi = 0;
        uint64_t rk = 0;

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* rch = BO::chain_children(node, sc);

        fbm.for_each_set([&](uint8_t idx, int slot) {
            if constexpr (BITS > U8_BITS) {
                using CNK = nk_for_bits_t<BITS - CHAR_BIT>;
                constexpr int CNK_BITS = static_cast<int>(sizeof(CNK) * CHAR_BIT);
                auto child = collect_entries<BITS - CHAR_BIT>(rch[slot]);
                if (wi == 0) rk = child.rep_key;
                for (size_t i = 0; i < child.count; ++i) {
                    wk[wi] = (NK(idx) << (NK_BITS - CHAR_BIT))
                           | static_cast<NK>(static_cast<uint64_t>(child.keys[i]) << (U64_BITS - CNK_BITS) >> (U64_BITS - NK_BITS + CHAR_BIT));
                    wv[wi] = child.vals[i];
                    wi++;
                }
            }
        });
        return {std::move(wk), std::move(wv), wi, rk};
    }

    // ==================================================================
    // do_coalesce — collect entries + build leaf
    // ==================================================================

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

    // ==================================================================
    // NK-independent helpers
    // ==================================================================

    static void inc_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        BO::chain_descendants_mut(node, hdr->skip(), hdr->entries())++;
    }

    static uint64_t dec_descendants(uint64_t* node, node_header_t* hdr) noexcept {
        uint64_t& d = BO::chain_descendants_mut(node, hdr->skip(), hdr->entries());
        return --d;
    }

    // ==================================================================
    // Subtree deallocation — runtime-recursive
    // ==================================================================

    static void dealloc_subtree(uint64_t tagged, uint8_t depth,
                                  BLD& bld) noexcept {
        if (tagged & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            depth_switch(depth + skip, [&]<int BITS>() {
                using NK = nk_for_bits_t<BITS>;
                if constexpr (sizeof(NK) == sizeof(uint8_t))
                    BO::bitmap_destroy_and_dealloc(node, bld);
                else {
                    using CO = compact_ops<NK, VALUE, ALLOC>;
                    CO::destroy_and_dealloc(node, bld);
                }
            });
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        uint8_t child_depth = depth + sc + 1;
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            dealloc_subtree(child, child_depth, bld);
        });
        bld.dealloc_node(node, hdr->alloc_u64());
    }

    template<int BITS> requires (BITS >= U8_BITS)
    static void dealloc_coalesced_node(uint64_t* node, uint8_t sc,
                                         BLD& bld) noexcept {
        constexpr uint8_t depth = (KEY_BITS - BITS) / CHAR_BIT;
        uint8_t child_depth = depth + sc + 1;
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            dealloc_subtree_nodes_only(child, child_depth, bld);
        });
        bld.dealloc_node(node, get_header(node)->alloc_u64());
    }

    // Free node structures only — values transferred to new leaf
    static void dealloc_subtree_nodes_only(uint64_t tagged, uint8_t depth,
                                             BLD& bld) noexcept {
        if (tagged & LEAF_BIT) [[unlikely]] {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            depth_switch(depth + skip, [&]<int BITS>() {
                using NK = nk_for_bits_t<BITS>;
                if constexpr (sizeof(NK) == sizeof(uint8_t))
                    BO::bitmap_dealloc_node_only(node, bld);
                else {
                    using CO = compact_ops<NK, VALUE, ALLOC>;
                    CO::dealloc_node_only(node, bld);
                }
            });
            return;
        }
        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        uint8_t child_depth = depth + sc + 1;
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            dealloc_subtree_nodes_only(child, child_depth, bld);
        });
        bld.dealloc_node(node, hdr->alloc_u64());
    }

};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_OPS_HPP
