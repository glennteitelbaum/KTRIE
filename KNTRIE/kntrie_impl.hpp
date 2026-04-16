#ifndef KNTRIE_IMPL_HPP
#define KNTRIE_IMPL_HPP

#include "kntrie_ops.hpp"
#include <memory>
#include <cstring>
#include <algorithm>

namespace gteitelbaum::kntrie_detail {

struct kntrie_stats_t {
    std::size_t total_bytes    = 0;
    std::size_t total_entries  = 0;
    std::size_t bitmap_leaves  = 0;
    std::size_t compact_leaves = 0;
    std::size_t bitmask_nodes  = 0;
    std::size_t bm_children    = 0;
};

// ======================================================================
// kntrie_impl<K, VALUE, ALLOC>
//
// K = unsigned stored key type (already sign-flipped by caller).
// All methods take K directly.  Caller (kntrie.hpp) handles KEY↔K.
// ======================================================================

template<typename K, typename VALUE, typename ALLOC>
class kntrie_impl {
    static_assert(std::is_unsigned_v<K>, "K must be unsigned stored type");

public:
    using key_type    = K;
    using mapped_type = VALUE;
    using size_type   = std::size_t;

private:
    using VT   = value_traits<VALUE, ALLOC>;
    using VST  = typename VT::slot_type;

    using NORM_V = normalized_ops_value_t<VALUE>;
    using NVT  = value_traits<NORM_V, ALLOC>;
    using NVST = typename NVT::slot_type;

    using BO   = bitmask_ops<K, NORM_V, ALLOC>;
    using CO   = compact_ops<K, NORM_V, ALLOC>;
    using BLD  = builder<NORM_V, NVT::IS_TRIVIAL, ALLOC>;
    using OPS  = kntrie_ops<K, NORM_V, ALLOC>;

    static constexpr unsigned TOP_SHIFT    = (sizeof(K) - 1) * CHAR_BIT;
    static constexpr int      KEY_BYTES    = static_cast<int>(sizeof(K));
    static constexpr int      MAX_ROOT_SKIP = KEY_BYTES - ROOT_CONSUMED_BYTES;

    // --- Data members ---
    std::uint64_t root_ptr_v;
    K             root_prefix_v;
    std::size_t   size_v;
    unsigned      root_skip_bytes_v;
    BLD           bld_v;

    K root_prefix_mask() const noexcept {
        unsigned shift = (sizeof(K) - root_skip_bytes_v) * CHAR_BIT;
        return ~K(0) << shift;
    }

    unsigned root_dispatch_shift() const noexcept {
        return TOP_SHIFT - root_skip_bytes_v * CHAR_BIT;
    }

    void set_root(unsigned skip_bytes) noexcept {
        root_skip_bytes_v = skip_bytes;
    }

    void mark_root() noexcept {
        set_root_parent(root_ptr_v);
    }

public:
    kntrie_impl() : root_ptr_v(BO::SENTINEL_TAGGED), root_prefix_v{},
                     size_v(0), root_skip_bytes_v(0), bld_v() {}

    explicit kntrie_impl(const ALLOC& a)
        : root_ptr_v(BO::SENTINEL_TAGGED), root_prefix_v{},
          size_v(0), root_skip_bytes_v(0), bld_v(a) {}

    ~kntrie_impl() { remove_all(); }

    kntrie_impl(const kntrie_impl&) = delete;
    kntrie_impl& operator=(const kntrie_impl&) = delete;

    kntrie_impl(kntrie_impl&& o) noexcept
        : root_ptr_v(o.root_ptr_v), root_prefix_v(o.root_prefix_v),
          size_v(o.size_v), root_skip_bytes_v(o.root_skip_bytes_v),
          bld_v(std::move(o.bld_v)) {
        o.root_ptr_v = BO::SENTINEL_TAGGED;
        o.root_prefix_v = K{};
        o.size_v = 0;
        o.root_skip_bytes_v = 0;
    }

    kntrie_impl& operator=(kntrie_impl&& o) noexcept {
        if (this != &o) {
            remove_all();
            bld_v.drain();
            root_ptr_v = o.root_ptr_v;
            root_prefix_v = o.root_prefix_v;
            size_v = o.size_v;
            root_skip_bytes_v = o.root_skip_bytes_v;
            bld_v = std::move(o.bld_v);
            o.root_ptr_v = BO::SENTINEL_TAGGED;
            o.root_prefix_v = K{};
            o.size_v = 0;
            o.root_skip_bytes_v = 0;
        }
        return *this;
    }

    void swap(kntrie_impl& o) noexcept {
        std::swap(root_ptr_v, o.root_ptr_v);
        std::swap(root_prefix_v, o.root_prefix_v);
        std::swap(size_v, o.size_v);
        std::swap(root_skip_bytes_v, o.root_skip_bytes_v);
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
    // Find — takes stored K directly
    // ==================================================================

    iter_entry_t<K> find_entry(K stored) const noexcept {
        if (root_skip_bytes_v != 0) [[unlikely]] {
            if ((stored ^ root_prefix_v) & root_prefix_mask()) [[unlikely]]
                return {};
        }
        return OPS::find_loop(root_ptr_v, stored, root_dispatch_shift());
    }

    bool contains(K stored) const noexcept {
        return find_entry(stored).found;
    }

    // ==================================================================
    // Edge entry
    // ==================================================================

    iter_entry_t<K> edge_entry(dir_t dir) const noexcept {
        if (size_v == 0) return {};
        return OPS::descend_edge_loop(root_ptr_v, dir);
    }

    // ==================================================================
    // Iteration helpers
    // ==================================================================

    static iter_entry_t<K> walk_from_leaf(std::uint64_t* leaf, dir_t dir) noexcept {
        auto* hdr = get_header(leaf);
        std::uint64_t* parent = leaf_parent(leaf);
        if (!parent || hdr->is_root()) [[unlikely]] return {};
        std::uint16_t byte = hdr->parent_byte();
        return walk_bm_chain(parent, byte, dir);
    }

    static iter_entry_t<K> walk_from_bm(std::uint64_t* bm_node, dir_t dir) noexcept {
        auto* hdr = get_header(bm_node);
        std::uint64_t* parent = bm_parent(bm_node);
        if (!parent || hdr->is_root()) [[unlikely]] return {};
        std::uint16_t byte = hdr->parent_byte();
        return walk_bm_chain(parent, byte, dir);
    }

    static iter_entry_t<K> walk_bm_chain(std::uint64_t* parent, std::uint16_t byte, dir_t dir) noexcept {
        auto* phdr = get_header(parent);
        std::uint8_t sc = phdr->skip();
        std::uint64_t bm_ptr = BO::node_bm_ptr(parent);

        typename bitmap_256_t::adj_result adj;
        if (dir == dir_t::FWD)
            adj = bitmap_ref(bm_ptr).next_set_after(static_cast<std::uint8_t>(byte));
        else
            adj = bitmap_ref(bm_ptr).prev_set_before(static_cast<std::uint8_t>(byte));

        if (adj.found) {
            std::uint64_t child = BO::chain_child(parent, sc, adj.slot);
            return OPS::descend_edge_loop(child, dir);
        }

        std::uint64_t* grandparent = bm_parent(parent);
        if (!grandparent || phdr->is_root()) [[unlikely]] return {};
        return walk_bm_chain(grandparent, phdr->parent_byte(), dir);
    }

    static iter_entry_t<K> advance_pos(iter_entry_t<K> cur, dir_t dir) noexcept {
        if (!cur.found) return {};
        auto* hdr = get_header(cur.leaf);

        iter_entry_t<K> next;
        if (hdr->is_bitmap())
            next = BO::bitmap_advance(cur.leaf, cur.pos, cur.bit, cur.key, cur.val, dir);
        else
            next = CO::compact_advance(cur.leaf, cur.pos, cur.val, dir);

        if (next.found) return next;
        return walk_from_leaf(cur.leaf, dir);
    }

    // ==================================================================
    // Lower/upper bound
    // ==================================================================

    iter_entry_t<K> lower_bound_entry(K stored) const noexcept {
        return find_ge_entry(stored);
    }

    iter_entry_t<K> find_ge_entry(K stored) const noexcept {
        return tracked_descent_fwd(stored, [](std::uint64_t* leaf, K s) -> iter_entry_t<K> {
            return OPS::leaf_find_ge(leaf, s);
        });
    }

    iter_entry_t<K> upper_bound_entry(K stored) const noexcept {
        return tracked_descent_fwd(stored, [](std::uint64_t* leaf, K s) -> iter_entry_t<K> {
            auto r = OPS::leaf_find_ge(leaf, s);
            if (r.found && r.key == s) {
                auto* hdr = get_header(leaf);
                if (hdr->is_bitmap())
                    return BO::bitmap_advance(leaf, r.pos, r.bit, r.key, r.val, dir_t::FWD);
                else
                    return CO::compact_advance(leaf, r.pos, r.val, dir_t::FWD);
            }
            return r;
        });
    }

    // ==================================================================
    // Insert — takes stored K directly
    // ==================================================================

    bool insert(K stored, const VALUE& value) {
        auto r = insert_dispatch<true, false>(stored, value);
        return r.inserted;
    }

    bool insert_or_assign(K stored, const VALUE& value) {
        auto r = insert_dispatch<true, true>(stored, value);
        return r.inserted;
    }

    insert_pos_result_t insert_with_pos(K stored, const VALUE& value) {
        return insert_dispatch<true, false>(stored, value);
    }

    iter_entry_t<K> entry_from_pos(std::uint64_t* leaf, std::uint16_t pos, K stored) const noexcept {
        return OPS::entry_from_insert_pos(leaf, pos, stored);
    }

    insert_pos_result_t upsert_with_pos(K stored, const VALUE& value) {
        return insert_dispatch<true, true>(stored, value);
    }

    // ==================================================================
    // Erase — takes stored K directly
    // ==================================================================

    bool erase(K stored) {
        if (size_v == 0) [[unlikely]] return false;
        return erase_stored(stored).erased;
    }

    bool erase_at(std::uint64_t* leaf, std::uint16_t pos) {
        if (size_v == 0) [[unlikely]] return false;
        auto* hdr = get_header(leaf);
        K stored;
        if (hdr->is_bitmap())
            stored = BO::read_base_key(leaf) | K(pos);
        else
            stored = CO::keys(leaf, COMPACT_HEADER_U64)[pos];
        return erase_stored(stored).erased;
    }

    iter_entry_t<K> erase_with_next(K stored) {
        if (size_v == 0) [[unlikely]] return {};
        auto r = erase_stored(stored);
        if (!r.erased) return {};
        if (r.next.found) [[likely]] return r.next;
        if (size_v == 0) return {};
        return find_ge_entry(stored);
    }

private:
    erase_result_t<K> erase_stored(K stored) {
        if (root_skip_bytes_v != 0) [[unlikely]] {
            if ((stored ^ root_prefix_v) & root_prefix_mask()) [[unlikely]]
                return {0, false, 0, {}};
        }

        std::uint64_t old_root_ptr = root_ptr_v;
        auto r = OPS::erase_node(root_ptr_v, stored, root_dispatch_shift(), bld_v);
        if (r.erased) [[likely]] {
            root_ptr_v = r.tagged_ptr ? r.tagged_ptr : BO::SENTINEL_TAGGED;
            mark_root();
            --size_v;
            if (size_v == 0) [[unlikely]] {
                root_ptr_v = BO::SENTINEL_TAGGED;
                root_prefix_v = K{};
                set_root(0);
                r.next = {};
            } else {
                bool root_changed = (root_ptr_v != old_root_ptr);
                if (root_changed) [[unlikely]] {
                    normalize_root();
                    if (size_v <= COMPACT_MAX && !(root_ptr_v & LEAF_BIT)) [[unlikely]]
                        coalesce_bm_to_leaf();
                    r.next = {};
                }
            }
        }
        return r;
    }

public:
    // ==================================================================
    // Stats
    // ==================================================================

    kntrie_stats_t debug_stats() const noexcept {
        kntrie_stats_t s{};
        s.total_bytes = sizeof(*this);
        // TODO: walk tree and collect stats
        return s;
    }

    std::size_t memory_usage() const noexcept { return debug_stats().total_bytes; }

    const std::uint64_t* debug_root() const noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return nullptr;
        if (root_ptr_v & LEAF_BIT) return untag_leaf(root_ptr_v);
        return bm_to_node_const(root_ptr_v);
    }

private:
    template<bool INSERT, bool ASSIGN>
    insert_pos_result_t insert_dispatch(K stored, const VALUE& value) {
        NVST sv;
        if constexpr (std::is_same_v<VALUE, NORM_V>) {
            sv = bld_v.store_value(value);
        } else {
            static_assert(sizeof(VALUE) <= sizeof(NVST));
            sv = NVST{};
            std::memcpy(&sv, &value, sizeof(VALUE));
        }

        if (size_v == 0) [[unlikely]] {
            if constexpr (!INSERT) { bld_v.destroy_value(sv); return {}; }
            set_root(0);
        }

        if (root_skip_bytes_v > 0) [[unlikely]] {
            K diff = (stored ^ root_prefix_v) & root_prefix_mask();
            if (diff) [[unlikely]] {
                if constexpr (!INSERT) { bld_v.destroy_value(sv); return {}; }
                unsigned shift_pos = TOP_SHIFT;
                unsigned div_pos = 0;
                while (div_pos < root_skip_bytes_v) {
                    if (((stored >> shift_pos) & 0xFF) != ((root_prefix_v >> shift_pos) & 0xFF))
                        break;
                    shift_pos -= CHAR_BIT;
                    div_pos++;
                }
                reduce_root_skip(div_pos);
            }
        }

        std::uint64_t old_root_ptr = root_ptr_v;
        auto r = OPS::template insert_node<INSERT, ASSIGN>(
            root_ptr_v, stored, root_dispatch_shift(), sv, bld_v);
        if (r.tagged_ptr != root_ptr_v) [[unlikely]] { root_ptr_v = r.tagged_ptr; mark_root(); }

        if (r.inserted) [[likely]] {
            ++size_v;
            if (size_v == 1) [[unlikely]] {
                root_prefix_v = stored;
                set_root(MAX_ROOT_SKIP);
                normalize_root();
            } else if (root_ptr_v != old_root_ptr) [[unlikely]] {
                normalize_root();
                if (size_v <= COMPACT_MAX && !(root_ptr_v & LEAF_BIT)) [[unlikely]]
                    coalesce_bm_to_leaf();
                auto lp = find_entry(stored);
                return {lp.leaf, lp.pos, true};
            }
            return {r.leaf, r.pos, true};
        }
        bld_v.destroy_value(sv);
        return {r.leaf, r.pos, false};
    }

    // ==================================================================
    // tracked_descent_fwd — for lower/upper bound
    // ==================================================================

    template<typename LeafFn>
    iter_entry_t<K> tracked_descent_fwd(K stored, LeafFn&& leaf_fn) const noexcept {
        if (size_v == 0) return {};

        if (root_skip_bytes_v != 0) [[unlikely]] {
            K diff = (stored ^ root_prefix_v) & root_prefix_mask();
            if (diff) {
            unsigned shift_pos = TOP_SHIFT;
            for (unsigned i = 0; i < root_skip_bytes_v; ++i) {
                std::uint8_t sb = static_cast<std::uint8_t>((stored >> shift_pos) & 0xFF);
                std::uint8_t rb = static_cast<std::uint8_t>((root_prefix_v >> shift_pos) & 0xFF);
                if (sb != rb) {
                    if (sb < rb) return edge_entry(dir_t::FWD);
                    return {};
                }
                shift_pos -= CHAR_BIT;
            }
        }
        }

        std::uint64_t ptr = root_ptr_v;
        unsigned shift = root_dispatch_shift();

        while (!(ptr & LEAF_BIT)) {
            std::uint64_t* node = bm_to_node(ptr);
            auto* hdr = get_header(node);
            std::uint8_t sc = hdr->skip();

            for (std::uint8_t si = 0; si < sc; ++si) {
                std::uint8_t expected = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
                std::uint8_t actual = BO::skip_byte(node, si);
                if (expected != actual) {
                    if (expected < actual)
                        return OPS::descend_edge_loop(ptr, dir_t::FWD);
                    return walk_from_bm(node, dir_t::FWD);
                }
                shift -= CHAR_BIT;
            }

            std::uint8_t ti = static_cast<std::uint8_t>((stored >> shift) & 0xFF);
            typename BO::child_lookup cl;
            if (sc > 0) [[unlikely]]
                cl = BO::chain_lookup(node, sc, ti);
            else
                cl = BO::lookup(node, ti);

            if (cl.found) {
                ptr = cl.child;
                shift -= CHAR_BIT;
            } else {
                std::uint64_t bm_ptr = (sc > 0)
                    ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                        &BO::chain_bitmap(node, sc)))
                    : BO::node_bm_ptr(node);
                auto [sib, sib_found] = BO::bm_next_sibling(bm_ptr, ti);
                if (sib_found)
                    return OPS::descend_edge_loop(sib, dir_t::FWD);
                return walk_from_bm(node, dir_t::FWD);
            }
        }

        if (ptr & NOT_FOUND_BIT) return {};
        std::uint64_t* leaf = untag_leaf_mut(ptr);
        auto result = leaf_fn(leaf, stored);
        if (result.found) return result;
        return walk_from_leaf(leaf, dir_t::FWD);
    }

    // ==================================================================
    // normalize_root
    // ==================================================================

    void normalize_root() {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return;

        // Leaf root → root_skip stays as-is (valid fast-reject filter)
        if (root_ptr_v & LEAF_BIT)
            return;

        // BM root → absorb chains into root_skip
        std::uint64_t* node = bm_to_node(root_ptr_v);
        auto* hdr = get_header(node);
        std::uint8_t sc = hdr->skip();
        if (sc == 0) return;

        unsigned old_skip = root_skip_bytes_v;
        unsigned nc = hdr->entries();
        std::uint64_t desc = BO::chain_descendants(node, sc, nc);

        // Read prefix from first descendant leaf
        std::uint64_t tagged = root_ptr_v;
        while (!(tagged & LEAF_BIT)) {
            std::uint64_t* n = bm_to_node(tagged);
            auto* h = get_header(n);
            std::uint8_t s = h->skip();
            const std::uint64_t* ch = BO::chain_children(n, s);
            tagged = ch[0];
        }
        std::uint64_t* first_leaf = untag_leaf_mut(tagged);
        auto* first_hdr = get_header(first_leaf);
        if (first_hdr->is_bitmap())
            root_prefix_v = BO::read_base_key(first_leaf);
        else
            root_prefix_v = CO::keys(first_leaf, COMPACT_HEADER_U64)[0];

        const bitmap_256_t& fbm = BO::chain_bitmap(node, sc);
        const std::uint64_t* cch = BO::chain_children(node, sc);

        std::uint8_t indices[BYTE_VALUES];
        std::uint64_t children[BYTE_VALUES];
        fbm.for_each_set([&](std::uint8_t idx, int slot) {
            indices[slot] = idx;
            children[slot] = cch[slot];
        });

        auto* new_node = BO::make_bitmask(indices, children, nc, bld_v, desc);
        bld_v.dealloc_node(node, hdr->alloc_u64());
        root_ptr_v = tag_bitmask(new_node);

        set_root(old_skip + sc);
        mark_root();
    }

    // ==================================================================
    // coalesce_bm_to_leaf
    // ==================================================================

    void coalesce_bm_to_leaf() {
        constexpr std::size_t hu = COMPACT_HEADER_U64;
        std::size_t total_u64 = CO::get_compact_u64(static_cast<std::uint16_t>(size_v));
        std::uint64_t* leaf = bld_v.alloc_node(total_u64);
        auto* lh = get_header(leaf);
        lh->set_entries(static_cast<std::uint16_t>(size_v));
        CO::set_capacity(leaf, static_cast<std::uint16_t>(size_v));

        K* dk = CO::keys(leaf, hu);
        std::size_t wi = 0;

        unsigned walk_shift = root_dispatch_shift();

        if constexpr (NVT::IS_BOOL) {
            auto bv = CO::bool_vals_mut(leaf);
            bv.clear_all(size_v);
            OPS::walk_entries_in_order(root_ptr_v, walk_shift,
                [&](K key, NVST v) {
                    dk[wi] = key;
                    bv.set(wi, v);
                    wi++;
                });
        } else {
            auto* dv = CO::vals_mut(leaf);
            OPS::walk_entries_in_order(root_ptr_v, walk_shift,
                [&](K key, NVST v) {
                    dk[wi] = key;
                    NVT::init_slot(&dv[wi], v);
                    wi++;
                });
        }

        OPS::dealloc_subtree_nodes_only(root_ptr_v, walk_shift, bld_v);
        root_ptr_v = tag_leaf(leaf);
        set_root(0);
        mark_root();
    }

    // ==================================================================
    // reduce_root_skip
    // ==================================================================

    void reduce_root_skip(unsigned div_pos) {
        unsigned old_skip = root_skip_bytes_v;
        unsigned remaining_skip = old_skip - div_pos - 1;

        std::uint64_t old_subtree;
        if (remaining_skip > 0) {
            if (root_ptr_v & LEAF_BIT) {
                // Compact leaf at root: no skip needed, return directly
                old_subtree = root_ptr_v;
            } else {
                std::uint8_t chain_bytes[MAX_SKIP];
                unsigned prefix_shift = TOP_SHIFT - (div_pos + 1) * CHAR_BIT;
                for (unsigned i = 0; i < remaining_skip; ++i) {
                    chain_bytes[i] = static_cast<std::uint8_t>(
                        (root_prefix_v >> (prefix_shift - i * CHAR_BIT)) & 0xFF);
                }
                std::uint64_t* bm_node = bm_to_node(root_ptr_v);
                old_subtree = BO::wrap_in_chain(bm_node, chain_bytes, remaining_skip, bld_v);
            }
        } else {
            old_subtree = root_ptr_v;
        }

        unsigned div_shift = TOP_SHIFT - div_pos * CHAR_BIT;
        std::uint8_t old_byte = static_cast<std::uint8_t>((root_prefix_v >> div_shift) & 0xFF);
        std::uint8_t indices[1] = {old_byte};
        std::uint64_t children[1] = {old_subtree};
        auto* bm_node = BO::make_bitmask(indices, children, 1, bld_v, size_v);
        root_ptr_v = tag_bitmask(bm_node);

        set_root(div_pos);
        mark_root();
    }

    // ==================================================================
    // Remove all
    // ==================================================================

    void remove_all() noexcept {
        if (root_ptr_v == BO::SENTINEL_TAGGED) return;
        OPS::dealloc_subtree(root_ptr_v, root_dispatch_shift(), bld_v);
        root_ptr_v = BO::SENTINEL_TAGGED;
        root_prefix_v = K{};
        set_root(0);
    }

    static const bitmap_256_t& bitmap_ref(std::uint64_t bm_tagged) noexcept {
        return *reinterpret_cast<const bitmap_256_t*>(
            static_cast<std::uintptr_t>(bm_tagged));
    }
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_IMPL_HPP
