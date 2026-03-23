#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
#include <memory>
#include <cstring>
#include <algorithm>

namespace gteitelbaum::kntrie_detail {

// Standalone stats accumulator
struct kntrie_stats_t {
    size_t total_bytes    = 0;
    size_t total_entries  = 0;
    size_t bitmap_leaves  = 0;
    size_t compact_leaves = 0;
    size_t bitmask_nodes  = 0;
    size_t bm_children    = 0;
};

// ======================================================================
// kntrie_iter_ops<VALUE, ALLOC> — destroy, stats.
//
// Iteration moved to fn-pointer dispatch in kntrie_ops.
// All functions take uint64_t ik. No NK narrowing.
// ======================================================================

template<typename VALUE, typename ALLOC, int KEY_BITS>
struct kntrie_iter_ops {
    using BO  = bitmask_ops<VALUE, ALLOC>;
    using VT  = value_traits<VALUE, ALLOC>;
    using VST = typename VT::slot_type;
    using BLD = builder<VALUE, VT::IS_TRIVIAL, ALLOC>;
    using OPS = kntrie_ops<VALUE, ALLOC, KEY_BITS>;

    // ==================================================================
    // Destroy leaf: compile-time NK dispatch via BITS
    // ==================================================================

    template<int BITS>
    static void destroy_leaf(uint64_t* node, BLD& bld) noexcept {
        using NK = nk_for_bits_t<BITS>;
        if constexpr (sizeof(NK) == 1)
            BO::bitmap_destroy_and_dealloc(node, bld);
        else {
            using CO = compact_ops<NK, VALUE, ALLOC>;
            CO::destroy_and_dealloc(node, bld);
        }
    }

    // ==================================================================
    // Remove subtree: runtime-recursive, depth-based
    // ==================================================================

    static void remove_subtree(uint64_t tagged, uint8_t depth,
                                 BLD& bld) noexcept {
        if (tagged == BO::SENTINEL_TAGGED) return;

        if (tagged & LEAF_BIT) {
            uint64_t* node = untag_leaf_mut(tagged);
            auto* hdr = get_header(node);
            uint8_t skip = hdr->skip();
            OPS::depth_switch(depth + skip, [&]<int BITS>() {
                destroy_leaf<BITS>(node, bld);
            });
            return;
        }

        uint64_t* node = bm_to_node(tagged);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        uint8_t child_depth = depth + sc + 1;
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            remove_subtree(child, child_depth, bld);
        });
        BO::dealloc_bitmask(node, bld);
    }

    // ==================================================================
    // Stats collection: runtime-recursive, depth-based
    // ==================================================================

    using stats_t = kntrie_stats_t;

    static void collect_stats(uint64_t tagged, uint8_t depth,
                                stats_t& s) noexcept {
        if (tagged & LEAF_BIT) {
            const uint64_t* node = untag_leaf(tagged);
            auto* hdr = get_header(node);
            s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * U64_BYTES;
            s.total_entries += hdr->entries();
            uint8_t skip = hdr->skip();
            OPS::depth_switch(depth + skip, [&]<int BITS>() {
                using NK = nk_for_bits_t<BITS>;
                if constexpr (sizeof(NK) == 1) s.bitmap_leaves++;
                else                           s.compact_leaves++;
            });
            return;
        }

        const uint64_t* node = bm_to_node_const(tagged);
        auto* hdr = get_header(node);
        s.total_bytes += static_cast<size_t>(hdr->alloc_u64()) * U64_BYTES;
        s.bitmask_nodes++;
        s.bm_children += hdr->entries();
        uint8_t sc = hdr->skip();
        uint8_t child_depth = depth + sc + 1;
        BO::chain_for_each_child(node, sc, [&](unsigned, uint64_t child) {
            collect_stats(child, child_depth, s);
        });
    }
};

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie_impl {
    static_assert(std::is_integral_v<KEY> && sizeof(KEY) >= 2,
                  "KEY must be integral and at least 16 bits");

public:
    using key_type       = KEY;
    using mapped_type    = VALUE;
    using size_type      = std::size_t;
    using allocator_type = ALLOC;

private:
    using KO   = key_ops<KEY>;
    using IK   = typename KO::IK;
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    // Normalized VALUE for ops: collapse same-sized trivial types
    using NORM_V = normalized_ops_value_t<VALUE>;
    using NVT  = value_traits<NORM_V, ALLOC>;
    using NVST = typename NVT::slot_type;

    using BO   = bitmask_ops<NORM_V, ALLOC>;
    using BLD  = builder<NORM_V, NVT::IS_TRIVIAL, ALLOC>;

    static constexpr int IK_BITS  = KO::IK_BITS;
    static constexpr int KEY_BITS = KO::KEY_BITS;

    using OPS  = kntrie_ops<NORM_V, ALLOC, KEY_BITS>;
    using ITER_OPS = kntrie_iter_ops<NORM_V, ALLOC, KEY_BITS>;

    // MAX_ROOT_SKIP: leave 1 byte for subtree root dispatch + 1 byte minimum
    // u16: 0, u32: 2, u64: 6
    static constexpr int MAX_ROOT_SKIP = KEY_BITS / CHAR_BIT - ROOT_CONSUMED_BYTES;

    // ==================================================================
    // key_to_u64: left-align internal key in uint64_t
    // ==================================================================

    static uint64_t key_to_u64(const KEY& key) noexcept {
        IK internal = KO::to_internal(key);
        return static_cast<uint64_t>(internal) << (U64_BITS - IK_BITS);
    }


    // --- Data members ---
    uint64_t  root_ptr_v;       // tagged child (SENTINEL, leaf, or bitmask)
    uint64_t  root_prefix_v;    // shared prefix bytes, left-aligned
    size_t    size_v;
    uint8_t   root_skip_bits_v;
    BLD       bld_v;

    uint64_t root_prefix_mask() const noexcept {
        return ~(~0ULL >> root_skip_bits_v);
    }
    uint8_t root_skip_bytes() const noexcept {
        return root_skip_bits_v / CHAR_BIT;
    }

    void set_root(uint8_t skip_bytes) noexcept {
        root_skip_bits_v = skip_bytes * CHAR_BIT;
    }

    void set_root(uint64_t ptr, uint64_t prefix, uint8_t skip_bytes) noexcept {
        root_ptr_v = ptr;
        root_prefix_v = prefix;
        set_root(skip_bytes);
        mark_root();
    }

    // Mark current root_ptr_v as root (no parent).
    void mark_root() noexcept {
        if (root_ptr_v != BO::SENTINEL_TAGGED)
            set_root_parent(root_ptr_v);
    }

public:
    // ==================================================================
    // Constructor / Destructor
    // ==================================================================

    kntrie_impl()
        : root_ptr_v(BO::SENTINEL_TAGGED),
          root_prefix_v(0),
          size_v(0),
          root_skip_bits_v(0),
          bld_v() {}

    ~kntrie_impl() { remove_all(); bld_v.drain(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : root_ptr_v(o.root_ptr_v),
          root_prefix_v(o.root_prefix_v),
          size_v(o.size_v),
          root_skip_bits_v(o.root_skip_bits_v),
          bld_v(std::move(o.bld_v)) {
        o.root_ptr_v = BO::SENTINEL_TAGGED;
        o.root_prefix_v = 0;
        o.size_v = 0;
        o.root_skip_bits_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            bld_v.drain();
            root_ptr_v = o.root_ptr_v;
            root_prefix_v = o.root_prefix_v;
            size_v = o.size_v;
            root_skip_bits_v = o.root_skip_bits_v;
            bld_v = std::move(o.bld_v);
            o.root_ptr_v = BO::SENTINEL_TAGGED;
            o.root_prefix_v = 0;
            o.size_v = 0;
            o.root_skip_bits_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        std::swap(root_ptr_v, o.root_ptr_v);
        std::swap(root_prefix_v, o.root_prefix_v);
        std::swap(size_v, o.size_v);
        std::swap(root_skip_bits_v, o.root_skip_bits_v);
        bld_v.swap(o.bld_v);
    }

    [[nodiscard]] bool      empty() const noexcept { return size_v == 0; }
    [[nodiscard]] size_type size()  const noexcept { return size_v; }
    [[nodiscard]] const ALLOC& get_allocator() const noexcept { return bld_v.get_allocator(); }

    void clear() noexcept {
        remove_all();
        bld_v.drain();
        size_v = 0;
    }

    // ==================================================================
    // Find — no sentinel checks, sentinel fn returns nullptr
    // ==================================================================

    // Find entry — returns iter_entry_t with key+val.
    iter_entry_t find_entry(const KEY& key) const noexcept {
        uint64_t ik = key_to_u64(key);
        if ((ik ^ root_prefix_v) & root_prefix_mask()) [[unlikely]] return {};
        return BO::find_loop(root_ptr_v, ik, ik << root_skip_bits_v);
    }

    // Legacy find — returns VALUE* for callers that just need the value.
    const VALUE* find_value(const KEY& key) const noexcept {
        auto r = find_entry(key);
        if (!r.found) return nullptr;
        return reinterpret_cast<const VALUE*>(r.val);
    }

    bool contains(const KEY& key) const noexcept {
        return find_entry(key).found;
    }

    // ==================================================================
    // Edge entry — first (FWD) or last (BWD) entry in container.
    // ==================================================================

    iter_entry_t edge_entry(dir_t dir) const noexcept {
        if (size_v == 0) [[unlikely]] return {};
        return BO::descend_edge_loop(root_ptr_v, dir);
    }

    // ==================================================================
    // Parent walk — static, no impl pointer needed.
    // All parents are bitmask nodes.
    // ==================================================================

    // Walk from a leaf: get parent byte + leaf_parent, then walk bitmask chain.
    static iter_entry_t walk_from_leaf(uint64_t* leaf, dir_t dir) noexcept {
        uint16_t byte = get_header(leaf)->parent_byte();
        uint64_t* parent = leaf_parent(leaf);
        return walk_bm_chain(parent, byte, dir);
    }

    // Walk from a bitmask node: get parent byte + bm_parent, then walk chain.
    static iter_entry_t walk_from_bm(uint64_t* bm_node, dir_t dir) noexcept {
        uint16_t byte = get_header(bm_node)->parent_byte();
        uint64_t* parent = bm_parent(bm_node);
        return walk_bm_chain(parent, byte, dir);
    }

    // Walk bitmask parent chain. All nodes are bitmask.
    static iter_entry_t walk_bm_chain(uint64_t* parent, uint16_t byte, dir_t dir) noexcept {
        while (byte != node_header_t::ROOT_BYTE) {
            uint64_t bm_ptr = BO::node_bm_ptr(parent);
            auto [sib, found] = (dir == dir_t::FWD)
                ? BO::bm_next_sibling(bm_ptr, static_cast<uint8_t>(byte))
                : BO::bm_prev_sibling(bm_ptr, static_cast<uint8_t>(byte));
            if (found) return BO::descend_edge_loop(sib, dir);
            byte = get_header(parent)->parent_byte();
            parent = bm_parent(parent);
        }
        return {};
    }

    // ==================================================================
    // Tracked descent for lower_bound / upper_bound.
    // On bitmask miss, tries next sibling then walks parents.
    // ==================================================================

    template<typename LeafFn>
    iter_entry_t tracked_descent_fwd(uint64_t ik, LeafFn&& leaf_fn) const noexcept {
        if (size_v == 0) [[unlikely]] return {};

        uint64_t diff = (ik ^ root_prefix_v) & root_prefix_mask();
        if (diff) [[unlikely]] {
            int shift = std::countl_zero(diff) & BYTE_BOUNDARY_MASK;
            uint8_t kb = static_cast<uint8_t>(ik >> (U64_TOP_BYTE_SHIFT - shift));
            uint8_t pb = static_cast<uint8_t>(root_prefix_v >> (U64_TOP_BYTE_SHIFT - shift));
            if (kb < pb) return edge_entry(dir_t::FWD);
            return {};
        }

        uint64_t ptr = root_ptr_v;
        uint64_t shifted = ik << root_skip_bits_v;
        int depth = 0;

        while (!(ptr & LEAF_BIT)) [[likely]] {
            uint8_t byte = static_cast<uint8_t>(shifted >> (U64_TOP_BYTE_SHIFT - depth * CHAR_BIT));
            auto [child, slot] = BO::bm_child_exact(ptr, byte);
            if (slot < 0) [[unlikely]] {
                auto [sib, found] = BO::bm_next_sibling(ptr, byte);
                if (found) return BO::descend_edge_loop(sib, dir_t::FWD);
                uint64_t* bm_node = bm_to_node(ptr);
                return walk_from_bm(bm_node, dir_t::FWD);
            }
            ptr = child;
            ++depth;
        }

        uint64_t* leaf = untag_leaf_mut(ptr);
        return leaf_fn(leaf, ik);
    }

    iter_entry_t lower_bound_entry(const KEY& key) const noexcept {
        uint64_t ik = key_to_u64(key);
        return tracked_descent_fwd(ik, [](uint64_t* leaf, uint64_t ik_) -> iter_entry_t {
            // Exact match?
            auto fn = get_find_fn(leaf);
            auto r = fn(leaf, ik_);
            if (r.found) return r;
            // First entry > ik in this leaf (key-based, cold path)
            auto r2 = OPS::leaf_first_after(leaf, ik_, dir_t::FWD);
            if (r2.found) return r2;
            return walk_from_leaf(leaf, dir_t::FWD);
        });
    }

    iter_entry_t upper_bound_entry(const KEY& key) const noexcept {
        uint64_t ik = key_to_u64(key);
        return tracked_descent_fwd(ik, [](uint64_t* leaf, uint64_t ik_) -> iter_entry_t {
            // First entry > ik in this leaf (key-based, cold path)
            auto r = OPS::leaf_first_after(leaf, ik_, dir_t::FWD);
            if (r.found) return r;
            return walk_from_leaf(leaf, dir_t::FWD);
        });
    }

public:
    // ==================================================================
    // Insert / Insert-or-assign / Assign
    // ==================================================================

    std::pair<bool, bool> insert(const KEY& key, const VALUE& value) {
        auto r = insert_dispatch<true, false>(key, value);
        return {true, r.inserted};
    }

    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        auto r = insert_dispatch<true, true>(key, value);
        return {true, r.inserted};
    }

    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        auto r = insert_dispatch<false, true>(key, value);
        return {true, r.inserted};
    }

    // Insert returning {leaf, pos, inserted} for live iterators.
    // Single-walk: insert_dispatch propagates leaf/pos from compact/bitmap insert.
    // Re-find only after rare normalize_root/coalesce_bm_to_leaf.
    insert_pos_result_t insert_with_pos(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, false>(key, value);
    }

    // Insert-or-assign returning {leaf, pos, was_new_insert}.
    insert_pos_result_t upsert_with_pos(const KEY& key, const VALUE& value) {
        return insert_dispatch<true, true>(key, value);
    }

    // ==================================================================
    // Erase
    // ==================================================================

    bool erase(const KEY& key) {
        if (size_v == 0) [[unlikely]] return false;
        return erase_ik(key_to_u64(key));
    }

    // Erase by leaf position — reconstruct key from {leaf, pos},
    // then delegate to key-based erase. Caller must advance iterator
    // before calling.
    bool erase_at(uint64_t* leaf, uint16_t pos) {
        if (size_v == 0) [[unlikely]] return false;
        uint64_t ik = OPS::reconstruct_ik(leaf, pos);
        return erase_ik(ik);
    }

    // Erase by cached ik — used by iterator which already has the key.
    bool erase_by_ik(uint64_t ik) {
        if (size_v == 0) [[unlikely]] return false;
        return erase_ik(ik);
    }

private:
    bool erase_ik(uint64_t ik) {
        if ((ik ^ root_prefix_v) & root_prefix_mask()) [[unlikely]] return false;

        uint64_t old_root_ptr = root_ptr_v;
        auto r = OPS::erase_node(root_ptr_v, ik, ik << root_skip_bits_v, root_skip_bytes(), bld_v);
        bool erased = r.erased;
        if (erased) [[likely]] {
            root_ptr_v = r.tagged_ptr ? r.tagged_ptr : BO::SENTINEL_TAGGED;
            mark_root();
            --size_v;
            if (size_v == 0) [[unlikely]] {
                root_ptr_v = BO::SENTINEL_TAGGED;
                root_prefix_v = 0;
                set_root(0);
            } else {
                if (root_ptr_v != old_root_ptr) [[unlikely]] normalize_root();
                // Belt and suspenders: coalesce if BM root under COMPACT_MAX
                if (size_v <= COMPACT_MAX && !(root_ptr_v & LEAF_BIT)
                    && root_ptr_v != BO::SENTINEL_TAGGED) [[unlikely]]
                    coalesce_bm_to_leaf();
            }
        }
        return erased;
    }
public:

    // ==================================================================
    // Stats / Memory
    // ==================================================================

    struct debug_stats_t {
        size_t compact_leaves = 0;
        size_t bitmap_leaves  = 0;
        size_t bitmask_nodes  = 0;
        size_t bm_children    = 0;
        size_t total_entries  = 0;
        size_t total_bytes    = 0;
    };

    debug_stats_t debug_stats() const noexcept {
        debug_stats_t s{};
        s.total_bytes = sizeof(*this);
        if (root_ptr_v != BO::SENTINEL_TAGGED) {
            typename ITER_OPS::stats_t os{};
            ITER_OPS::collect_stats(root_ptr_v, root_skip_bytes(), os);
            s.total_bytes    += os.total_bytes;
            s.total_entries  += os.total_entries;
            s.bitmap_leaves  += os.bitmap_leaves;
            s.compact_leaves += os.compact_leaves;
            s.bitmask_nodes  += os.bitmask_nodes;
            s.bm_children    += os.bm_children;
        }
        return s;
    }

    size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    struct root_info_t {
        uint16_t entries; uint8_t skip;
        bool is_leaf;
    };

    root_info_t debug_root_info() const {
        bool is_leaf = (root_ptr_v & LEAF_BIT) != 0;
        uint16_t entries = 0;
        if (root_ptr_v != BO::SENTINEL_TAGGED) {
            if (is_leaf)
                entries = get_header(untag_leaf(root_ptr_v))->entries();
            else
                entries = get_header(bm_to_node_const(root_ptr_v))->entries();
        }
        return {entries, root_skip_bytes(), is_leaf};
    }

    const uint64_t* debug_root() const noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return nullptr;
        if (root_ptr_v & LEAF_BIT) return untag_leaf(root_ptr_v);
        return bm_to_node_const(root_ptr_v);
    }

    template<bool INSERT, bool ASSIGN>
    insert_pos_result_t insert_dispatch(const KEY& key, const VALUE& value) {
        uint64_t ik = key_to_u64(key);

        // Convert VALUE → normalized slot for ops
        NVST sv;
        if constexpr (std::is_same_v<VALUE, NORM_V>) {
            sv = bld_v.store_value(value);
        } else {
            // Trivial same-size: direct bit conversion
            static_assert(sizeof(VALUE) <= sizeof(NVST));
            sv = NVST{};
            std::memcpy(&sv, &value, sizeof(VALUE));
        }

        // First insert: root at skip=0, leaf handles its own prefix
        if (size_v == 0) [[unlikely]] {
            if constexpr (!INSERT) { bld_v.destroy_value(sv); return {}; }
            set_root(0);
        }

        if (root_skip_bits_v > 0) [[unlikely]] {
            uint64_t diff = (ik ^ root_prefix_v) & root_prefix_mask();
            if (diff) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {}; }
                int clz = std::countl_zero(diff);
                uint8_t div_pos = static_cast<uint8_t>(clz / CHAR_BIT);
                reduce_root_skip(div_pos);
            }
        }

        uint64_t old_root_ptr = root_ptr_v;
        auto r = OPS::template insert_node<INSERT, ASSIGN>(
            root_ptr_v, ik, ik << root_skip_bits_v, root_skip_bytes(), sv, bld_v);
        if (r.tagged_ptr != root_ptr_v) [[unlikely]] { root_ptr_v = r.tagged_ptr; mark_root(); }

        if (r.inserted) [[likely]] {
            ++size_v;
            if (root_ptr_v != old_root_ptr) [[unlikely]] {
                normalize_root();
                if (size_v <= COMPACT_MAX && !(root_ptr_v & LEAF_BIT)) [[unlikely]]
                    coalesce_bm_to_leaf();
                // normalize/coalesce may have reallocated — re-find
                auto lp = find_entry(key);
                return {lp.leaf, lp.pos, true};
            }
            return {r.leaf, r.pos, true};
        }
        bld_v.destroy_value(sv);
        // Not inserted — r.leaf/r.pos point to the existing entry
        return {r.leaf, r.pos, false};
    }

private:

    // ==================================================================
    // first_descendant_prefix: descend to any leaf, return node[2]
    // ==================================================================

    static uint64_t first_descendant_prefix(uint64_t tagged) noexcept {
        while (!(tagged & LEAF_BIT)) {
            const uint64_t* node = bm_to_node_const(tagged);
            auto* hdr = get_header(node);
            uint8_t sc = hdr->skip();
            const uint64_t* ch = BO::chain_children(node, sc);
            tagged = ch[0];
        }
        return leaf_prefix(untag_leaf(tagged));
    }

    // ==================================================================
    // normalize_root: enforce invariant —
    //   leaf root → root_skip_bits_v=0 (prepend root skip bytes into leaf's own skip)
    //   BM root   → absorb bitmask chains into root_skip_bits_v
    // ==================================================================

    void normalize_root() {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return;

        // Leaf root → enforce root_skip_bits_v=0
        if (root_ptr_v & LEAF_BIT) {
            if (root_skip_bits_v > 0) {
                // Leaf returned from do_coalesce at depth=root_skip_bytes().
                // Prepend root's skip bytes into the leaf's own skip.
                // O(1): updates header + fn ptrs, no reallocation.
                // leaf_prefix already has the correct root prefix bytes
                // (set during init_leaf_fns via ik & safe_prefix_mask).
                uint64_t* leaf = untag_leaf_mut(root_ptr_v);
                leaf = OPS::template prepend_skip<KEY_BITS>(
                    leaf, root_skip_bytes(), leaf_prefix(leaf), bld_v);
                root_ptr_v = tag_leaf(leaf);
                set_root(0);
                mark_root();
            }
            return;
        }

        // BM root → absorb chains if present
        uint64_t* node = bm_to_node(root_ptr_v);
        auto* hdr = get_header(node);
        uint8_t sc = hdr->skip();
        if (sc == 0) return;

        uint8_t old_skip = root_skip_bytes();
        unsigned nc = hdr->entries();
        uint64_t desc = BO::chain_descendants(node, sc, nc);

        root_prefix_v = first_descendant_prefix(root_ptr_v);

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const uint64_t* cch = BO::chain_children(node, sc);

        uint8_t indices[BYTE_VALUES];
        uint64_t children[BYTE_VALUES];
        fbm.for_each_set([&](uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = cch[slot];
        });

        auto* new_node = BO::make_bitmask(indices, children, nc, bld_v, desc);
        bld_v.dealloc_node(node, hdr->alloc_u64());
        root_ptr_v = tag_bitmask(new_node);

        set_root(old_skip + sc);
        mark_root();
    }

    // coalesce_bm_to_leaf: flatten BM tree to single leaf.
    // Called when root is BM but size ≤ COMPACT_MAX (invariant violation).
    // BM tree is implicitly sorted (bitmap order × sorted child COs),
    // so collection produces sorted output. O(n) single pass.
    // ==================================================================

    void coalesce_bm_to_leaf() {
        OPS::depth_switch(root_skip_bytes(), [&]<int BITS>() {
            auto c = OPS::template collect_entries<BITS>(root_ptr_v);
            uint64_t* leaf = OPS::template build_leaf<BITS>(
                c.keys.get(), c.vals.get(), c.count, c.rep_key, bld_v);
            OPS::dealloc_subtree_nodes_only(root_ptr_v, root_skip_bytes(), bld_v);
            root_ptr_v = tag_leaf(leaf);
        });
        set_root(0);
        mark_root();
    }

    // ==================================================================
    // reduce_root_skip: restructure root when prefix diverges
    // ==================================================================

    void reduce_root_skip(uint8_t div_pos) {
        uint8_t old_skip = root_skip_bytes();
        uint8_t remaining_skip = old_skip - div_pos - 1;

        uint64_t old_subtree;
        if (remaining_skip > 0) {
            if (root_ptr_v & LEAF_BIT) {
                // Leaf: prepend_skip takes root_prefix_v directly
                uint64_t* leaf = untag_leaf_mut(root_ptr_v);
                auto do_prepend = [&]<int DIVP>() -> uint64_t* {
                    constexpr int BITS = KEY_BITS - CHAR_BIT * (DIVP + 1);
                    return OPS::template prepend_skip<BITS>(
                        leaf, remaining_skip, root_prefix_v, bld_v);
                };
                if constexpr (MAX_ROOT_SKIP >= 1) {
                    switch (div_pos) {
                    case 0: leaf = do_prepend.template operator()<0>(); break;
                    case 1: if constexpr (MAX_ROOT_SKIP >= 2) { leaf = do_prepend.template operator()<1>(); break; } [[fallthrough]];
                    case 2: if constexpr (MAX_ROOT_SKIP >= 3) { leaf = do_prepend.template operator()<2>(); break; } [[fallthrough]];
                    case 3: if constexpr (MAX_ROOT_SKIP >= 4) { leaf = do_prepend.template operator()<3>(); break; } [[fallthrough]];
                    case 4: if constexpr (MAX_ROOT_SKIP >= 5) { leaf = do_prepend.template operator()<4>(); break; } [[fallthrough]];
                    case 5: if constexpr (MAX_ROOT_SKIP >= 6) { leaf = do_prepend.template operator()<5>(); break; } [[fallthrough]];
                    default: __builtin_unreachable();
                    }
                }
                old_subtree = tag_leaf(leaf);
            } else {
                // Bitmask: needs raw bytes for wrap_in_chain
                uint8_t chain_bytes[MAX_SKIP];
                for (uint8_t i = 0; i < remaining_skip; ++i)
                    chain_bytes[i] = pfx_byte(root_prefix_v, div_pos + 1 + i);
                uint64_t* bm_node = bm_to_node(root_ptr_v);
                old_subtree = BO::wrap_in_chain(bm_node, chain_bytes, remaining_skip, bld_v);
            }
        } else {
            old_subtree = root_ptr_v;
        }

        // Create a new bitmask with single child at the divergence byte
        uint8_t old_byte = pfx_byte(root_prefix_v, div_pos);
        uint8_t indices[1] = {old_byte};
        uint64_t children[1] = {old_subtree};
        auto* bm_node = BO::make_bitmask(indices, children, 1, bld_v, size_v);
        root_ptr_v = tag_bitmask(bm_node);

        // Update skip
        set_root(div_pos);
        mark_root();
    }

    // ==================================================================
    // Remove all
    // ==================================================================

    void remove_all() noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return;
        ITER_OPS::remove_subtree(root_ptr_v, root_skip_bytes(), bld_v);
        root_ptr_v = BO::SENTINEL_TAGGED;
        root_prefix_v = 0;
        set_root(0);
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_IMPL_HPP
