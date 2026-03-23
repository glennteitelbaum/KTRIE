#ifndef KNTRIE_BITMASK_HPP
#define KNTRIE_BITMASK_HPP

#include "kntrie_support.hpp"

namespace gteitelbaum::kntrie_detail {

// ==========================================================================
// bitmask_ops  -- unified bitmask node + bitmap_256_t leaf operations
//
// Bitmask node (internal): [header(1)][parent_ptr(1)][bitmap(4)][sentinel(1)][children(n)][desc(1 u64)]
//   - Parent pointer at node[1], bitmap starts at node[2] (= node + HEADER_U64)
//   - sentinel at offset 5 from bitmap = SENTINEL_TAGGED for branchless miss
//   - real children at offset 5 from bitmap (after sentinel)
//   - All children are tagged uint64_t values
//   - desc: one uint64_t at end of node, total descendant count for this subtree
//
// Bitmap256 leaf (suffix_type=0): [header(1 or 2)][bitmap(4)][values(n)]
//   - Parent pointer targets &node[0] | LEAF_BIT
//   - header_size = 1 (no skip) or 2 (with skip, prefix in node[1])
//   - values at header_size + 4
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct bitmask_ops {
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;
    using BLD  = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;

    // ==================================================================
    // Sentinel — pure tag value, no physical node.
    // Bitmap child[0] stores this on miss; find_loop detects via NOT_FOUND_BIT.
    // ==================================================================

    static constexpr uint64_t SENTINEL_TAGGED = LEAF_BIT | NOT_FOUND_BIT;

    // ==================================================================
    // Size calculations
    // ==================================================================

    static constexpr size_t bitmask_size_u64(size_t n_children, size_t hu = HEADER_U64) noexcept {
        return hu + BM_CHILDREN_START + n_children + DESC_U64;
    }

    static constexpr size_t bitmap_leaf_size_u64(size_t count, size_t hu = LEAF_HEADER_U64) noexcept {
        if constexpr (VT::IS_BOOL) {
            return hu + BITMAP_WORDS + BITMAP_WORDS;  // presence + value bitmap
        } else {
            size_t vb = align_up(count * sizeof(VST), U64_BYTES);
            return hu + BITMAP_WORDS + vb / U64_BYTES;
        }
    }

    // ==================================================================
    // Bitmask node: branchless descent (for find) — tagged version
    // Takes bitmap pointer directly (not node pointer).
    // Returns tagged uint64_t (next child or SENTINEL_TAGGED).
    // ==================================================================

    static uint64_t branchless_find_tagged(const uint64_t* bm_ptr, uint8_t idx) noexcept {
        const bitmap_256_t& bm = *reinterpret_cast<const bitmap_256_t*>(bm_ptr);
        int slot = bm.find_slot<slot_mode::BRANCHLESS>(idx);  // 0 on miss -> sentinel
        return bm_ptr[BITMAP_WORDS + slot];  // [0-3]=bitmap, [4]=sentinel, [5+]=children
    }

    // ==================================================================
    // Bitmask node: lookup child (returns tagged uint64_t)
    // ==================================================================

    struct child_lookup {
        uint64_t child;   // tagged value
        int       slot;
        bool      found;
    };

    static child_lookup lookup(const uint64_t* node, uint8_t idx) noexcept {
        return lookup_at(node, 1, idx);
    }

    // ==================================================================
    // Bitmask node: set child pointer (tagged)
    // ==================================================================

    static void set_child(uint64_t* node, int slot, uint64_t tagged_ptr) noexcept {
        real_children_mut(node, 1)[slot] = tagged_ptr;
    }

    // ==================================================================
    // Skip chain read accessors
    // ==================================================================

    // Read single skip byte at embed position e (0-based)
    static uint8_t skip_byte(const uint64_t* node, uint8_t e) noexcept {
        const auto* embed_bm = reinterpret_cast<const bitmap_256_t*>(node + HEADER_U64 + static_cast<size_t>(e) * EMBED_U64);
        return embed_bm->single_bit_index();
    }

    // Copy all skip bytes to buffer
    static void skip_bytes(const uint64_t* node, uint8_t sc, uint8_t* out) noexcept {
        for (uint8_t e = 0; e < sc; ++e)
            out[e] = skip_byte(node, e);
    }

    // Lookup in the final bitmap of a skip chain
    static child_lookup chain_lookup(const uint64_t* node, uint8_t sc, uint8_t idx) noexcept {
        return lookup_at(node, chain_hs(sc), idx);
    }

    // Read tagged child at slot in final bitmap of skip chain
    static uint64_t chain_child(const uint64_t* node, uint8_t sc, int slot) noexcept {
        return real_children(node, chain_hs(sc))[slot];
    }

    // Write tagged child at slot in final bitmap of skip chain
    static void chain_set_child(uint64_t* node, uint8_t sc, int slot, uint64_t tagged) noexcept {
        real_children_mut(node, chain_hs(sc))[slot] = tagged;
    }

    // Descendants count of a skip chain (const)
    static uint64_t chain_descendants(const uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return *descendants_ptr(node, chain_hs(sc), nc);
    }

    // Descendants count of a skip chain (mutable ref)
    static uint64_t& chain_descendants_mut(uint64_t* node, uint8_t sc, unsigned nc) noexcept {
        return *descendants_ptr_mut(node, chain_hs(sc), nc);
    }

    // Final bitmap reference (const)
    static const bitmap_256_t& chain_bitmap(const uint64_t* node, uint8_t sc) noexcept {
        return bm(node, chain_hs(sc));
    }

    // Number of children in final bitmap
    static unsigned chain_child_count(const uint64_t* node, uint8_t sc) noexcept {
        return static_cast<unsigned>(chain_bitmap(node, sc).popcount());
    }

    // Pointer to real children array in final bitmap
    static const uint64_t* chain_children(const uint64_t* node, uint8_t sc) noexcept {
        return real_children(node, chain_hs(sc));
    }
    static uint64_t* chain_children_mut(uint64_t* node, uint8_t sc) noexcept {
        return real_children_mut(node, chain_hs(sc));
    }

    // Iterate final bitmap children: cb(slot, tagged_child)
    template<typename Fn>
    static void chain_for_each_child(const uint64_t* node, uint8_t sc, Fn&& cb) noexcept {
        size_t hs = chain_hs(sc);
        unsigned nc = static_cast<unsigned>(bm(node, hs).popcount());
        const uint64_t* ch = real_children(node, hs);
        for (unsigned i = 0; i < nc; ++i)
            cb(i, ch[i]);
    }

    // Embed child pointer (the pointer in embed e that links to next embed or final bitmap)
    static uint64_t embed_child(const uint64_t* node, uint8_t e) noexcept {
        return node[HEADER_U64 + static_cast<size_t>(e) * EMBED_U64 + EMBED_CHILD_PTR];
    }
    static void set_embed_child(uint64_t* node, uint8_t e, uint64_t tagged) noexcept {
        node[HEADER_U64 + static_cast<size_t>(e) * EMBED_U64 + EMBED_CHILD_PTR] = tagged;
    }

    // ==================================================================
    // Tagged pointer accessors (for iteration on standalone bitmask)
    // ==================================================================

    // Bitmap256 ref from a bitmask tagged pointer (ptr points at bitmap)
    static const bitmap_256_t& bitmap_ref(uint64_t bm_tagged) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(bm_tagged);
    }

    // Read tagged child at slot from a bitmask tagged pointer
    static uint64_t child_at(uint64_t bm_tagged, int slot) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BM_CHILDREN_START + slot];
    }

    // First child (slot 0) from a bitmask tagged pointer
    static uint64_t first_child(uint64_t bm_tagged) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(bm_tagged);
        return bm[BM_CHILDREN_START];
    }

    // ==================================================================
    // Bitmask node: add child (tagged) — standalone
    // ==================================================================

    static uint64_t* add_child(uint64_t* node, node_header_t* h,
                                uint8_t idx, uint64_t child_tagged,
                                BLD& bld) {
        return add_child_at(node, h, 1, idx, child_tagged, bld);
    }

    // ==================================================================
    // Skip chain: add child to final bitmask
    // ==================================================================

    static uint64_t* chain_add_child(uint64_t* node, node_header_t* h,
                                      uint8_t sc, uint8_t idx,
                                      uint64_t child_tagged,
                                      BLD& bld) {
        uint64_t* nn = add_child_at(node, h, chain_hs(sc), idx,
                                      child_tagged, bld);
        if (nn != node && sc > 0) fix_embeds(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: remove child — standalone
    // Returns nullptr if node becomes empty.
    // ==================================================================

    static uint64_t* remove_child(uint64_t* node, node_header_t* h,
                                   int slot, uint8_t idx, BLD& bld) {
        return remove_child_at(node, h, 1, slot, idx, bld);
    }

    // ==================================================================
    // Skip chain: remove child from final bitmask
    // ==================================================================

    static uint64_t* chain_remove_child(uint64_t* node, node_header_t* h,
                                         uint8_t sc, int slot, uint8_t idx,
                                         BLD& bld) {
        uint64_t* nn = remove_child_at(node, h, chain_hs(sc), slot, idx, bld);
        if (nn && nn != node && sc > 0) fix_embeds(nn, sc);
        return nn;
    }

    // ==================================================================
    // Bitmask node: make from arrays (tagged children)
    // ==================================================================

    static uint64_t* make_bitmask(const uint8_t* indices,
                                   const uint64_t* child_tagged_ptrs,
                                   unsigned n_children, BLD& bld,
                                   uint64_t descendants_ = 0) {
        bitmap_256_t bm = bitmap_256_t::from_indices(indices, n_children);

        constexpr size_t hs = HEADER_U64;
        size_t needed = bitmask_size_u64(n_children, hs);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = bld.alloc_node(au64);
        auto* nh = get_header(nn);
        nh->set_entries(n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(0);

        bm_mut(nn, hs) = bm;
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_fill_sorted(bm, real_children_mut(nn, hs),
                                    indices, child_tagged_ptrs, n_children);

        *descendants_ptr_mut(nn, hs, n_children) = descendants_;

        // Link all children to this new parent
        relink_all_children(nn, hs);
        return nn;
    }

    // ==================================================================
    // Bitmask node: make skip chain (one allocation)
    //
    // Layout: [header(1)][embed_0(6)]...[embed_{S-1}(6)][final_bm(4)][sent(1)][children(N)]
    // Each embed = bitmap_256_t(4) + sentinel(1) + child_ptr(1)
    // child_ptr points to next embed's bitmap (or final bitmap).
    // Total: 1 + S*6 + 4 + 1 + 1 + N = 7 + S*6 + N  (but we use 6 + S*6 + N since
    //        final_bm(4)+sent(1)+first_child_slot = 6, and remaining N-1 children follow,
    //        but actually: header(1) + S*6 + 4 + 1 + N = 6 + S*6 + N)
    // ==================================================================

    static uint64_t* make_skip_chain(const uint8_t* skip_bytes, uint8_t skip_count,
                                      const uint8_t* final_indices,
                                      const uint64_t* final_children_tagged,
                                      unsigned final_n_children, BLD& bld,
                                      uint64_t descendants_ = 0) {
        // Allocation: header + skip_count*embed + bitmap + sentinel + children + desc
        size_t needed = HEADER_U64 + static_cast<size_t>(skip_count) * EMBED_U64 + BM_CHILDREN_START + final_n_children
                       + DESC_U64;
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = bld.alloc_node(au64);

        auto* nh = get_header(nn);
        nh->set_entries(final_n_children);
        nh->set_alloc_u64(au64);
        nh->set_skip(skip_count);

        // Build each embed: bitmap + sentinel + child_ptr
        for (uint8_t e = 0; e < skip_count; ++e) {
            uint64_t* embed = nn + HEADER_U64 + e * EMBED_U64;
            // bitmap with single bit
            bitmap_256_t& bm = *reinterpret_cast<bitmap_256_t*>(embed);
            bm = bitmap_256_t{};
            bm.set_bit(skip_bytes[e]);
            // sentinel
            embed[EMBED_SENTINEL] = SENTINEL_TAGGED;
            // child ptr → next embed's bitmap (or final bitmap)
            uint64_t* next_bm = nn + HEADER_U64 + (e + 1) * EMBED_U64;
            embed[EMBED_CHILD_PTR] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(next_bm));  // no LEAF_BIT
        }

        // Final bitmask
        size_t final_offset = HEADER_U64 + static_cast<size_t>(skip_count) * EMBED_U64;
        bitmap_256_t fbm = bitmap_256_t::from_indices(final_indices, final_n_children);
        *reinterpret_cast<bitmap_256_t*>(nn + final_offset) = fbm;
        nn[final_offset + EMBED_SENTINEL] = SENTINEL_TAGGED;
        bitmap_256_t::arr_fill_sorted(fbm, nn + final_offset + BM_CHILDREN_START,
                                    final_indices, final_children_tagged,
                                    final_n_children);

        // Single descendants count (after children)
        nn[final_offset + BM_CHILDREN_START + final_n_children] = descendants_;

        // Link all final-level children to this node
        relink_all_children(nn, final_offset);

        return nn;
    }

    // ==================================================================
    // build_remainder: extract embeds [from..sc-1] + final bitmask
    //                  into a new standalone bitmask or skip chain.
    //
    // Returns: tagged pointer (bitmask or skip chain).
    // ==================================================================

    static uint64_t build_remainder(uint64_t* old_node, uint8_t old_sc,
                                     uint8_t from_pos, BLD& bld) {
        uint8_t rem_skip = old_sc - from_pos;
        unsigned final_nc = get_header(old_node)->entries();
        uint64_t old_descendants = chain_descendants(old_node, old_sc, final_nc);

        // Extract final bitmask indices + children
        const bitmap_256_t& fbm = chain_bitmap(old_node, old_sc);
        const uint64_t* old_ch = chain_children(old_node, old_sc);

        uint8_t indices[BYTE_VALUES];
        uint64_t children[BYTE_VALUES];
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = old_ch[slot];
        });

        if (rem_skip == 0) {
            return tag_bitmask(
                make_bitmask(indices, children, final_nc, bld, old_descendants));
        }

        // rem_skip > 0: extract skip bytes for [from_pos..old_sc-1]
        uint8_t sb[MAX_SKIP];
        for (uint8_t i = 0; i < rem_skip; ++i)
            sb[i] = skip_byte(old_node, from_pos + i);

        return tag_bitmask(
            make_skip_chain(sb, rem_skip, indices, children, final_nc, bld, old_descendants));
    }

    // ==================================================================
    // wrap_in_chain: wrap an existing bitmask node in a skip chain
    //               with prepended skip bytes.
    //
    // Deallocates the old child node. Returns tagged pointer.
    // ==================================================================

    static uint64_t wrap_in_chain(uint64_t* child, const uint8_t* bytes,
                                   uint8_t count, BLD& bld) {
        auto* ch = get_header(child);
        uint8_t child_sc = ch->skip();
        unsigned nc = ch->entries();
        uint64_t old_descendants = chain_descendants(child, child_sc, nc);

        // Collect all skip bytes: new prefix + child's existing skip
        uint8_t all_bytes[MAX_COMBINED_SKIP];
        std::memcpy(all_bytes, bytes, count);
        skip_bytes(child, child_sc, all_bytes + count);
        uint8_t total_skip = count + child_sc;

        // Extract final bitmask indices + children
        const bitmap_256_t& fbm = chain_bitmap(child, child_sc);
        const uint64_t* cch = chain_children(child, child_sc);

        uint8_t indices[BYTE_VALUES];
        uint64_t children[BYTE_VALUES];
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = cch[slot];
        });

        auto* new_chain = make_skip_chain(all_bytes, total_skip, indices, children,
                                          nc, bld, old_descendants);
        bld.dealloc_node(child, ch->alloc_u64());
        return tag_bitmask(new_chain);
    }

    // ==================================================================
    // collapse_info: info needed for single-child collapse
    // ==================================================================

    struct collapse_info {
        uint64_t sole_child;       // tagged child pointer
        uint8_t  bytes[MAX_SKIP + 1];         // skip bytes to prepend (skip_bytes[0..sc-1] + sole_idx)
        uint8_t  total_skip;       // sc + 1
        uint64_t sole_entries;     // exact subtree count of sole_child
    };

    // Extract collapse info from a skip chain (sc > 0, entries() == 1)
    static collapse_info chain_collapse_info(const uint64_t* node, uint8_t sc) noexcept {
        collapse_info ci;
        skip_bytes(node, sc, ci.bytes);
        ci.bytes[sc] = chain_bitmap(node, sc).first_set_bit();
        ci.total_skip = sc + 1;
        ci.sole_child = chain_children(node, sc)[0];
        ci.sole_entries = chain_descendants(node, sc, 1);
        return ci;
    }

    // Extract collapse info from a standalone bitmask (sc == 0, entries() == 1)
    static collapse_info standalone_collapse_info(const uint64_t* node) noexcept {
        collapse_info ci;
        const bitmap_256_t& bmp = bm(node, 1);
        ci.bytes[0] = bmp.first_set_bit();
        ci.total_skip = 1;
        ci.sole_child = real_children(node, 1)[0];
        ci.sole_entries = *descendants_ptr(node, 1, 1);
        return ci;
    }

    // ==================================================================
    // Bitmask node: iterate  cb(uint8_t idx, int slot, uint64_t tagged_child)
    // ==================================================================

    // Exact subtree count from a tagged pointer.
    // Leaf: entries (exact). Bitmask: descendants (exact).
    static uint64_t exact_subtree_count(uint64_t tagged) noexcept {
        if (tagged & LEAF_BIT)
            return get_header(untag_leaf(tagged))->entries();
        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        return chain_descendants(node, hdr->skip(), hdr->entries());
    }

    // ==================================================================

    template<typename Fn>
    static void for_each_child(const uint64_t* node, Fn&& cb) {
        constexpr size_t hs = HEADER_U64;
        const bitmap_256_t& bmp = bm(node, hs);
        const uint64_t* rch = real_children(node, hs);
        bmp.for_each_set([&](uint8_t idx, int slot) {
            cb(idx, slot, rch[slot]);
        });
    }

    // ==================================================================
    // Bitmask node: child count / alloc
    // ==================================================================

    static int child_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // --- Descendants count accessors (standalone bitmask, hs=1) ---
    static uint64_t node_descendants(const uint64_t* node) noexcept {
        constexpr size_t hs = HEADER_U64;
        unsigned nc = get_header(node)->entries();
        return *descendants_ptr(node, hs, nc);
    }
    static uint64_t& node_descendants_mut(uint64_t* node) noexcept {
        constexpr size_t hs = HEADER_U64;
        unsigned nc = get_header(node)->entries();
        return *descendants_ptr_mut(node, hs, nc);
    }

    static size_t node_alloc_u64(const uint64_t* node) noexcept {
        return get_header(node)->alloc_u64();
    }

    // ==================================================================
    // Bitmask node: deallocate (node only, not children)
    // ==================================================================

    static void dealloc_bitmask(uint64_t* node, BLD& bld) noexcept {
        bld.dealloc_node(node, get_header(node)->alloc_u64());
    }

    // ==================================================================
    // Bitmap value pointer helper
    // IS_BOOL uses idx (suffix byte) to check val bitmap.
    // Non-bool uses slot (ordinal position) to index bl_vals.
    // ==================================================================

    // Mutable value reference at slot for live iterators (non-bool only).
    static VALUE& bl_val_ref_at(uint64_t* node, size_t hs, int slot) noexcept {
        return *reinterpret_cast<VALUE*>(&bl_vals_mut(node, hs)[slot]);
    }

    // ==================================================================
    // find_fn_bitmap — exact match → leaf_pos_t (pos = byte value)
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_fn_bitmap(uint64_t* node, uint64_t ik) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            if (!skip_eq(leaf_prefix(node), d, ik)) [[unlikely]]
                return {};
        }

        uint8_t suffix = static_cast<uint8_t>(d.suffix(ik));
        if (!bm(node, hs).has_bit(suffix)) [[unlikely]] return {};
        return {node, static_cast<uint16_t>(suffix), true};
    }

    // ==================================================================
    // find_adv_fn_bitmap — directional advance → leaf_pos_t
    // FWD: first entry with byte > suffix. BWD: last with byte < suffix.
    // ==================================================================

    template<bool DO_SKIP>
    static leaf_pos_t find_adv_fn_bitmap(uint64_t* node, uint64_t ik, dir_t dir) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        depth_t d = get_depth(node);

        if constexpr (DO_SKIP) {
            int cmp = skip_cmp(leaf_prefix(node), d, ik);
            // FWD: cmp>0 means all entries > ik → miss at this leaf
            //      cmp<0 means all entries < ik → return edge
            // BWD: reversed
            if (dir == dir_t::FWD) {
                if (cmp > 0) return {};
                if (cmp < 0) {
                    uint8_t byte = bm(node, hs).first_set_bit();
                    return {node, static_cast<uint16_t>(byte), true};
                }
            } else {
                if (cmp < 0) return {};
                if (cmp > 0) {
                    uint8_t byte = bm(node, hs).last_set_bit();
                    return {node, static_cast<uint16_t>(byte), true};
                }
            }
        }

        uint8_t suffix = static_cast<uint8_t>(d.suffix(ik));

        if (dir == dir_t::FWD) {
            if (suffix >= static_cast<uint8_t>(d.nk_max())) [[unlikely]] return {};
            auto r = bm(node, hs).next_set_after(suffix);
            if (!r.found) [[unlikely]] return {};
            return {node, static_cast<uint16_t>(r.idx), true};
        } else {
            if (suffix == 0) [[unlikely]] return {};
            auto r = bm(node, hs).prev_set_before(suffix);
            if (!r.found) [[unlikely]] return {};
            return {node, static_cast<uint16_t>(r.idx), true};
        }
    }

    // ==================================================================
    // find_edge_fn_bitmap — edge entry → leaf_pos_t
    // FWD: first set bit. BWD: last set bit.
    // ==================================================================

    static leaf_pos_t find_edge_fn_bitmap(uint64_t* node, dir_t dir) noexcept {
        constexpr size_t hs = LEAF_HEADER_U64;
        uint8_t idx = (dir == dir_t::FWD)
            ? bm(node, hs).first_set_bit()
            : bm(node, hs).last_set_bit();
        return {node, static_cast<uint16_t>(idx), true};
    }

    // ==================================================================
    // Bitmap256 leaf: insert
    // ==================================================================

    template<bool INSERT = true, bool ASSIGN = true>
    requires (INSERT || ASSIGN)
    static insert_result_t bitmap_insert(uint64_t* node, uint8_t suffix,
                                          VST value, BLD& bld) {
        auto* h = get_header(node);
        size_t hs = LEAF_HEADER_U64;
        bitmap_256_t& bm = bm_mut(node, hs);
        unsigned count = h->entries();

        if constexpr (VT::IS_BOOL) {
            if (bm.has_bit(suffix)) {
                if constexpr (ASSIGN) {
                    if (value) val_bm_mut(node, hs).set_bit(suffix);
                    else       val_bm_mut(node, hs).clear_bit(suffix);
                }
                return {tag_leaf(node), false, false, nullptr,
                        node, static_cast<uint16_t>(suffix)};
            }
            if constexpr (!INSERT) return {tag_leaf(node), false, false};

            unsigned nc = count + 1;
            size_t new_sz = bitmap_leaf_size_u64(nc, hs);

            if (new_sz <= h->alloc_u64()) {
                bm.set_bit(suffix);
                if (value) val_bm_mut(node, hs).set_bit(suffix);
                h->set_entries(nc);
                return {tag_leaf(node), true, false, nullptr,
                        node, static_cast<uint16_t>(suffix)};
            }

            size_t au64 = round_up_u64(new_sz);
            uint64_t* nn = bld.alloc_node(au64);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn);
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);
            bm_mut(nn, hs) = bm;
            bm_mut(nn, hs).set_bit(suffix);
            val_bm_mut(nn, hs) = val_bm(node, hs);
            if (value) val_bm_mut(nn, hs).set_bit(suffix);
            bld.dealloc_node(node, h->alloc_u64());
            return {tag_leaf(nn), true, false, nullptr,
                    nn, static_cast<uint16_t>(suffix)};
        } else {
            VST* vd = bl_vals_mut(node, hs);

            if (bm.has_bit(suffix)) {
                int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
                if constexpr (ASSIGN) {
                    bld.destroy_value(vd[slot]);
                    VT::write_slot(&vd[slot], value);
                }
                return {tag_leaf(node), false, false, nullptr,
                        node, static_cast<uint16_t>(suffix)};
            }

            if constexpr (!INSERT) return {tag_leaf(node), false, false};

            unsigned nc = count + 1;
            size_t new_sz = bitmap_leaf_size_u64(nc, hs);

            if (new_sz <= h->alloc_u64()) {
                int isl = bm.find_slot<slot_mode::UNFILTERED>(suffix);
                bm.set_bit(suffix);
                std::memmove(vd + isl + 1, vd + isl, (count - isl) * sizeof(VST));
                VT::write_slot(&vd[isl], value);
                h->set_entries(nc);
                return {tag_leaf(node), true, false, nullptr,
                        node, static_cast<uint16_t>(suffix)};
            }

            size_t au64 = round_up_u64(new_sz);
            uint64_t* nn = bld.alloc_node(au64);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn);
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);
            bitmap_256_t& nbm = bm_mut(nn, hs);
            nbm = bm;
            nbm.set_bit(suffix);
            VST* nvd = bl_vals_mut(nn, hs);
            int isl = nbm.find_slot<slot_mode::UNFILTERED>(suffix);
            std::memcpy(nvd, vd, isl * sizeof(VST));
            VT::write_slot(&nvd[isl], value);
            std::memcpy(nvd + isl + 1, vd + isl, (count - isl) * sizeof(VST));

            bld.dealloc_node(node, h->alloc_u64());
            return {tag_leaf(nn), true, false, nullptr,
                    nn, static_cast<uint16_t>(suffix)};
        }
    }

    // ==================================================================
    // Bitmap256 leaf: modify existing value in-place
    // ==================================================================

    template<typename F>
    static bool bitmap_modify(uint64_t* node, uint8_t suffix, F&& fn) {
        auto* h = get_header(node);
        constexpr size_t hs = LEAF_HEADER_U64;
        bitmap_256_t& bm = bm_mut(node, hs);
        if (!bm.has_bit(suffix)) return false;

        if constexpr (VT::IS_BOOL) {
            bool tmp = val_bm(node, hs).has_bit(suffix);
            fn(tmp);
            if (tmp) val_bm_mut(node, hs).set_bit(suffix);
            else     val_bm_mut(node, hs).clear_bit(suffix);
        } else if constexpr (VT::HAS_DESTRUCTOR) {
            int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
            VST* vd = bl_vals_mut(node, hs);
            VALUE* ptr = reinterpret_cast<VALUE*>(vd[slot]);
            fn(*ptr);
        } else {
            int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
            VST* vd = bl_vals_mut(node, hs);
            fn(vd[slot]);
        }
        return true;
    }

    // ==================================================================
    // Bitmap256 leaf: erase
    // ==================================================================

    static erase_result_t bitmap_erase(uint64_t* node, uint8_t suffix,
                                        BLD& bld) {
        auto* h = get_header(node);
        size_t hs = LEAF_HEADER_U64;
        bitmap_256_t& bm = bm_mut(node, hs);
        if (!bm.has_bit(suffix)) return {tag_leaf(node), false, 0};

        unsigned count = h->entries();

        if constexpr (VT::IS_BOOL) {
            unsigned nc = count - 1;
            if (nc == 0) {
                bld.dealloc_node(node, h->alloc_u64());
                return {0, true, 0};
            }
            bm.clear_bit(suffix);
            val_bm_mut(node, hs).clear_bit(suffix);
            h->set_entries(nc);

            size_t new_sz = bitmap_leaf_size_u64(nc, hs);
            if (should_shrink_u64(h->alloc_u64(), new_sz)) {
                size_t au64 = round_up_u64(new_sz);
                uint64_t* nn = bld.alloc_node(au64);
                auto* nh = get_header(nn);
                copy_leaf_header(node, nn);
                nh->set_alloc_u64(au64);
                bm_mut(nn, hs) = bm;
                val_bm_mut(nn, hs) = val_bm(node, hs);
                bld.dealloc_node(node, h->alloc_u64());
                return {tag_leaf(nn), true, nc};
            }
            return {tag_leaf(node), true, nc};
        } else {
            int slot = bm.find_slot<slot_mode::UNFILTERED>(suffix);
            bld.destroy_value(bl_vals_mut(node, hs)[slot]);

            unsigned nc = count - 1;
            if (nc == 0) {
                bld.dealloc_node(node, h->alloc_u64());
                return {0, true, 0};
            }

            size_t new_sz = bitmap_leaf_size_u64(nc, hs);

            if (!should_shrink_u64(h->alloc_u64(), new_sz)) {
                VST* vd = bl_vals_mut(node, hs);
                bm.clear_bit(suffix);
                std::memmove(vd + slot, vd + slot + 1, (nc - slot) * sizeof(VST));
                h->set_entries(nc);
                return {tag_leaf(node), true, nc};
            }

            size_t au64 = round_up_u64(new_sz);
            uint64_t* nn = bld.alloc_node(au64);
            auto* nh = get_header(nn);
            copy_leaf_header(node, nn);
            nh->set_entries(nc);
            nh->set_alloc_u64(au64);
            bm_mut(nn, hs) = bm;
            bm_mut(nn, hs).clear_bit(suffix);
            const VST* ov = bl_vals(node, hs);
            VST*       nv = bl_vals_mut(nn, hs);
            std::memcpy(nv, ov, slot * sizeof(VST));
            std::memcpy(nv + slot, ov + slot + 1, (nc - slot) * sizeof(VST));

            bld.dealloc_node(node, h->alloc_u64());
            return {tag_leaf(nn), true, nc};
        }
    }

    // ==================================================================
    // Bitmap256 leaf: make from sorted suffixes
    // ==================================================================

    static uint64_t* make_bitmap_leaf(const uint8_t* sorted_suffixes,
                                       const VST* values, unsigned count,
                                       BLD& bld) {
        constexpr size_t hs = LEAF_HEADER_U64;
        size_t sz = round_up_u64(bitmap_leaf_size_u64(count));
        uint64_t* node = bld.alloc_node(sz);
        auto* h = get_header(node);
        h->set_entries(count);
        h->set_alloc_u64(sz);
        bitmap_256_t& bm = bm_mut(node, hs);
        bm = bitmap_256_t{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(sorted_suffixes[i]);
        if constexpr (VT::IS_BOOL) {
            bitmap_256_t& vbm = val_bm_mut(node, hs);
            vbm = bitmap_256_t{};
            for (unsigned i = 0; i < count; ++i)
                if (values[i]) vbm.set_bit(sorted_suffixes[i]);
        } else {
            VST* vd = bl_vals_mut(node, hs);
            for (unsigned i = 0; i < count; ++i)
                VT::init_slot(&vd[bm.find_slot<slot_mode::UNFILTERED>(sorted_suffixes[i])], values[i]);
        }
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: make single entry
    // ==================================================================

    static uint64_t* make_single_bitmap(uint8_t suffix, VST value, BLD& bld) {
        constexpr size_t hs = LEAF_HEADER_U64;
        size_t sz = round_up_u64(bitmap_leaf_size_u64(1));
        uint64_t* node = bld.alloc_node(sz);
        auto* h = get_header(node);
        h->set_entries(1);
        h->set_alloc_u64(sz);
        bm_mut(node, hs).set_bit(suffix);
        if constexpr (VT::IS_BOOL) {
            if (value) val_bm_mut(node, hs).set_bit(suffix);
        } else {
            VT::init_slot(&bl_vals_mut(node, hs)[0], value);
        }
        return node;
    }

    // ==================================================================
    // Bitmap256 leaf: iterate  cb(uint8_t suffix, VST value_slot)
    // ==================================================================

    template<typename Fn>
    static void for_each_bitmap(const uint64_t* node, Fn&& cb) {
        size_t hs = LEAF_HEADER_U64;
        const bitmap_256_t& bmp = bm(node, hs);
        if constexpr (VT::IS_BOOL) {
            const bitmap_256_t& vbm = val_bm(node, hs);
            bmp.for_each_set([&](uint8_t idx, int /*slot*/) {
                cb(idx, static_cast<VST>(vbm.has_bit(idx)));
            });
        } else {
            const VST* vd = bl_vals(node, hs);
            bmp.for_each_set([&](uint8_t idx, int slot) {
                cb(idx, vd[slot]);
            });
        }
    }

    // ==================================================================
    // Bitmap256 leaf: count
    // ==================================================================

    static uint32_t bitmap_count(const uint64_t* node) noexcept {
        return get_header(node)->entries();
    }

    // ==================================================================
    // Bitmap256 leaf: destroy values + deallocate
    // ==================================================================

    static void bitmap_destroy_and_dealloc(uint64_t* node, BLD& bld) {
        auto* h = get_header(node);
        if constexpr (VT::HAS_DESTRUCTOR) {
            uint16_t count = h->entries();
            VST* vd = bl_vals_mut(node, LEAF_HEADER_U64);
            for (uint16_t i = 0; i < count; ++i)
                bld.destroy_value(vd[i]);
        }
        bld.dealloc_node(node, h->alloc_u64());
    }

    // Free node memory only — values have been transferred elsewhere
    static void bitmap_dealloc_node_only(uint64_t* node, BLD& bld) {
        bld.dealloc_node(node, get_header(node)->alloc_u64());
    }

    // --- Chain header size: 1 (base header) + sc * 6 (embed slots) ---
    static constexpr size_t chain_hs(uint8_t sc) noexcept {
        return HEADER_U64 + static_cast<size_t>(sc) * EMBED_U64;
    }

private:
    // --- Fix embed internal pointers after reallocation ---
    static void fix_embeds(uint64_t* nn, uint8_t sc) noexcept {
        for (uint8_t e = 0; e < sc; ++e) {
            uint64_t* next_bm = nn + HEADER_U64 + static_cast<size_t>(e + 1) * EMBED_U64;
            nn[HEADER_U64 + static_cast<size_t>(e) * EMBED_U64 + EMBED_CHILD_PTR] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(next_bm));
        }
        // Fix sentinel of final bitmap
        size_t fo = chain_hs(sc);
        nn[fo + BITMAP_WORDS] = SENTINEL_TAGGED;
    }

    // --- Shared add child core: works for any header size ---
    static uint64_t* add_child_at(uint64_t* node, node_header_t* h, size_t hs,
                                    uint8_t idx, uint64_t child_tagged,
                                    BLD& bld) {
        bitmap_256_t& bm = bm_mut(node, hs);
        unsigned oc = h->entries();
        unsigned nc = oc + 1;
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        size_t needed = bitmask_size_u64(nc, hs);

        // In-place
        if (needed <= h->alloc_u64()) {
            // Save descendants (children shift will overwrite it)
            uint64_t saved = *descendants_ptr(node, hs, oc);

            // Insert child
            uint64_t* rch = real_children_mut(node, hs);
            std::memmove(rch + isl + 1, rch + isl, (oc - isl) * sizeof(uint64_t));
            rch[isl] = child_tagged;
            bm.set_bit(idx);
            h->set_entries(nc);

            // Write descendants at new position
            *descendants_ptr_mut(node, hs, nc) = saved;

            // Link new child to this parent
            link_child(node, child_tagged, idx);
            return node;
        }

        // Realloc
        uint64_t saved = *descendants_ptr(node, hs, oc);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = bld.alloc_node(au64);

        // Copy header + embeds/bitmap + sentinel (everything before children)
        size_t prefix_u64 = hs + BM_CHILDREN_START;
        std::memcpy(nn, node, prefix_u64 * U64_BYTES);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut(nn, hs).set_bit(idx);
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_copy_insert(real_children(node, hs), real_children_mut(nn, hs),
                                    oc, isl, child_tagged);

        *descendants_ptr_mut(nn, hs, nc) = saved;

        // Relink all children to new parent
        relink_all_children(nn, hs);

        bld.dealloc_node(node, h->alloc_u64());
        return nn;
    }

    // --- Shared remove child core: works for any header size ---
    static uint64_t* remove_child_at(uint64_t* node, node_header_t* h, size_t hs,
                                       int slot, uint8_t idx, BLD& bld) {
        unsigned oc = h->entries();
        unsigned nc = oc - 1;
        if (nc == 0) {
            bld.dealloc_node(node, h->alloc_u64());
            return nullptr;
        }

        size_t needed = bitmask_size_u64(nc, hs);

        // In-place — no parent changes needed (removed child is freed)
        if (!should_shrink_u64(h->alloc_u64(), needed)) {
            uint64_t saved = *descendants_ptr(node, hs, oc);

            bitmap_256_t::arr_remove(bm_mut(node, hs), real_children_mut(node, hs),
                                  oc, slot, idx);
            h->set_entries(nc);

            *descendants_ptr_mut(node, hs, nc) = saved;
            return node;
        }

        // Realloc
        uint64_t saved = *descendants_ptr(node, hs, oc);
        size_t au64 = round_up_u64(needed);
        uint64_t* nn = bld.alloc_node(au64);

        // Copy header + embeds/bitmap + sentinel
        size_t prefix_u64 = hs + BM_CHILDREN_START;
        std::memcpy(nn, node, prefix_u64 * U64_BYTES);

        auto* nh = get_header(nn);
        nh->set_entries(nc);
        nh->set_alloc_u64(au64);

        bm_mut(nn, hs).clear_bit(idx);
        children_mut(nn, hs)[0] = SENTINEL_TAGGED;

        bitmap_256_t::arr_copy_remove(real_children(node, hs), real_children_mut(nn, hs),
                                    oc, slot);

        *descendants_ptr_mut(nn, hs, nc) = saved;

        // Relink all children to new parent
        relink_all_children(nn, hs);

        bld.dealloc_node(node, h->alloc_u64());
        return nn;
    }

    // --- Relink all children of a bitmask node to point to it as parent ---
    static void relink_all_children(uint64_t* node, size_t hs) noexcept {
        const bitmap_256_t& bmp = bm(node, hs);
        unsigned nc = get_header(node)->entries();
        const uint64_t* rch = real_children(node, hs);
        // Walk set bits to get byte values
        unsigned slot = 0;
        for (unsigned w = 0; w < BITMAP_WORDS && slot < nc; ++w) {
            uint64_t word = bmp.words[w];
            while (word && slot < nc) {
                unsigned bit = static_cast<unsigned>(std::countr_zero(word));
                uint8_t byte = static_cast<uint8_t>(w * U64_BITS + bit);
                link_child(node, rch[slot], byte);
                word &= word - 1;  // clear lowest set bit
                ++slot;
            }
        }
    }

    // --- Shared lookup core: works for any header size ---
    static child_lookup lookup_at(const uint64_t* node, size_t hs, uint8_t idx) noexcept {
        const bitmap_256_t& bmp = bm(node, hs);
        int slot = bmp.find_slot<slot_mode::FAST_EXIT>(idx);
        if (slot < 0) return {0, -1, false};
        uint64_t child = real_children(node, hs)[slot];
        return {child, slot, true};
    }

    // --- Shared: bitmap starts after header (1 or 2 u64s) ---
public:
    static const bitmap_256_t& bm(const uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(n + header_size);
    }
private:
    static bitmap_256_t& bm_mut(uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<bitmap_256_t*>(n + header_size);
    }

    // --- Bitmask node: children array (includes sentinel at [0]) ---
    static const uint64_t* children(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_WORDS;
    }
    static uint64_t* children_mut(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BITMAP_WORDS;
    }

    // --- Bitmask node: real children (past sentinel) ---
    static const uint64_t* real_children(const uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BM_CHILDREN_START;
    }
    static uint64_t* real_children_mut(uint64_t* n, size_t header_size) noexcept {
        return n + header_size + BM_CHILDREN_START;
    }

    // --- Bitmap256 leaf: values after bitmap ---
    static const VST* bl_vals(const uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<const VST*>(n + header_size + BITMAP_WORDS);
    }
    static VST* bl_vals_mut(uint64_t* n, size_t header_size) noexcept {
        return reinterpret_cast<VST*>(n + header_size + BITMAP_WORDS);
    }

    // --- Bitmap256 leaf: value bitmap for bool (sits after presence bitmap) ---
public:
    static const bitmap_256_t& val_bm(const uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(n + header_size + BITMAP_WORDS);
    }
    static bitmap_256_t& val_bm_mut(uint64_t* n, size_t header_size) noexcept {
        return *reinterpret_cast<bitmap_256_t*>(n + header_size + BITMAP_WORDS);
    }
private:

    // --- Bitmask node: descendants count (single u64 after children) ---
    static const uint64_t* descendants_ptr(const uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return n + header_size + BM_CHILDREN_START + nc;
    }
    static uint64_t* descendants_ptr_mut(uint64_t* n, size_t header_size, unsigned nc) noexcept {
        return n + header_size + BM_CHILDREN_START + nc;
    }

public:
    // ==================================================================
    // Read path helpers — encapsulate bitmap navigation
    // ==================================================================

    // Branchless child lookup — returns child ptr (sentinel on miss)
    static uint64_t bm_child(uint64_t ptr, uint8_t byte) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        int slot = reinterpret_cast<const bitmap_256_t*>(bm)->
                       find_slot<slot_mode::BRANCHLESS>(byte);
        return bm[BITMAP_WORDS + slot];
    }

    // Exact child lookup — returns {child_ptr, slot} or {0, -1}
    struct bm_exact_t { uint64_t child; int slot; };
    static bm_exact_t bm_child_exact(uint64_t ptr, uint8_t byte) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        int slot = reinterpret_cast<const bitmap_256_t*>(bm)->
                       find_slot<slot_mode::FAST_EXIT>(byte);
        if (slot < 0) [[unlikely]] return {0, -1};
        return {bm[BM_CHILDREN_START + slot], slot};
    }

    // First real child (child[1], skipping sentinel at child[0])
    static uint64_t bm_first_child(uint64_t ptr) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        return bm[BM_CHILDREN_START];
    }

    // Last real child (child[popcount])
    static uint64_t bm_last_child(uint64_t ptr) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        int last = reinterpret_cast<const bitmap_256_t*>(bm)->popcount();
        return bm[BITMAP_WORDS + last];
    }

    // Next sibling after byte — returns {child_ptr, found}
    struct bm_sibling_t { uint64_t child; bool found; };
    static bm_sibling_t bm_next_sibling(uint64_t ptr, uint8_t byte) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        auto adj = reinterpret_cast<const bitmap_256_t*>(bm)->next_set_after(byte);
        if (!adj.found) return {0, false};
        return {bm[BM_CHILDREN_START + adj.slot], true};
    }

    // Prev sibling before byte
    static bm_sibling_t bm_prev_sibling(uint64_t ptr, uint8_t byte) noexcept {
        const uint64_t* bm = reinterpret_cast<const uint64_t*>(ptr);
        auto adj = reinterpret_cast<const bitmap_256_t*>(bm)->prev_set_before(byte);
        if (!adj.found) return {0, false};
        return {bm[BM_CHILDREN_START + adj.slot], true};
    }

    // ==================================================================
    // Read/iter loop functions — called by kntrie_impl
    // ==================================================================

    // find_loop: branchless bitmask descent, fn-ptr dispatch at leaf → leaf_pos_t
    static leaf_pos_t find_loop(uint64_t ptr, uint64_t ik,
                                 uint64_t shifted) noexcept {
        int depth = 0;
        while (!(ptr & LEAF_BIT)) [[likely]] {
            ptr = bm_child(ptr, static_cast<uint8_t>(shifted >> (U64_TOP_BYTE_SHIFT - depth * CHAR_BIT)));
            ++depth;
        }
        if (ptr & NOT_FOUND_BIT) [[unlikely]]
            return {};
        uint64_t* node = untag_leaf_mut(ptr);
        auto fn = get_find_fn(node);
        return fn(node, ik);
    }

    // descend_edge_loop: walk to min (FWD) or max (BWD) leaf → leaf_pos_t
    static leaf_pos_t descend_edge_loop(uint64_t ptr, dir_t dir) noexcept {
        while (!(ptr & LEAF_BIT))
            ptr = (dir == dir_t::FWD) ? bm_first_child(ptr) : bm_last_child(ptr);
        uint64_t* node = untag_leaf_mut(ptr);
        auto fn = get_find_edge(node);
        return fn(node, dir);
    }

    // ==================================================================
    // Bitmap leaf type check — for advance_pos dispatch
    // ==================================================================

    static constexpr uint8_t BITMAP_SHIFT_THRESHOLD = U64_BITS - U8_BITS;  // 56

    static bool is_bitmap_leaf(const uint64_t* node) noexcept {
        return get_depth(node).shift >= BITMAP_SHIFT_THRESHOLD;
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_BITMASK_HPP
