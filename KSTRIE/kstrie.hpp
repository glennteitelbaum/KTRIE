#ifndef KSTRIE_HPP
#define KSTRIE_HPP

#include "kstrie_impl.hpp"
#include <iterator>
#include <optional>
#include <stdexcept>
#include <vector>

namespace gteitelbaum {

// Public trait types for kstrie template parameters
namespace kstrie_traits {
    using identity_char_map      = kstrie_detail::identity_char_map;
    using upper_char_map         = kstrie_detail::upper_char_map;
    using reverse_lower_char_map = kstrie_detail::reverse_lower_char_map;

    template <std::array<uint8_t, 256> M>
    using char_map = kstrie_detail::char_map<M>;
} // namespace kstrie_traits

// ============================================================================
// kstrie -- user-facing trie class
//
// Inherits engine (find/insert/erase) from kstrie_impl.
// Adds bidirectional iterator, ordered traversal, prefix queries.
//
// Thread safety: kstrie is not thread-safe. Concurrent reads are safe.
// Concurrent read+write or write+write requires external synchronization.
//
// IMPORTANT: Iterators are snapshots. operator*() returns a copy of the
// key/value pair, not a reference into the trie. Modifications via
// insert_or_assign do not update existing iterators. Each ++/--
// re-descends from the root.
// ============================================================================

template <typename VALUE,
          typename CHARMAP = kstrie_traits::identity_char_map,
          typename ALLOC   = std::allocator<uint64_t>>
class kstrie {
    using impl_t       = kstrie_detail::kstrie_impl<VALUE, CHARMAP, ALLOC>;
    using hdr_type     = typename impl_t::hdr_type;
    using slots_type   = typename impl_t::slots_type;
    using bitmask_type = typename impl_t::bitmask_type;
    using compact_type = typename impl_t::compact_type;

    impl_t impl_v;

public:
    using key_type       = typename impl_t::key_type;
    using mapped_type    = typename impl_t::mapped_type;
    using size_type      = typename impl_t::size_type;
    using allocator_type = typename impl_t::allocator_type;

    static constexpr bool IS_BITMAP = slots_type::IS_BITMAP;

    // bool_ref: proxy for mutable access to packed boolean values.
    // Parallels std::vector<bool>::reference.
    struct bool_ref {
        uint64_t* word;
        uint8_t   bit;
        operator bool() const noexcept { return (*word >> bit) & 1; }
        bool_ref& operator=(bool v) noexcept {
            if (v) *word |=  (uint64_t(1) << bit);
            else   *word &= ~(uint64_t(1) << bit);
            return *this;
        }
        bool_ref& operator=(const bool_ref& o) noexcept {
            return *this = static_cast<bool>(o);
        }
    };

    using mapped_ref       = std::conditional_t<IS_BITMAP, bool_ref, VALUE&>;
    using const_mapped_ref = std::conditional_t<IS_BITMAP, bool, const VALUE&>;

    kstrie() = default;
    ~kstrie() = default;
    kstrie(kstrie&&) noexcept = default;
    kstrie& operator=(kstrie&&) noexcept = default;
    kstrie(const kstrie&) = default;
    kstrie& operator=(const kstrie&) = default;

    void swap(kstrie& o) noexcept { impl_v.swap(o.impl_v); }
    friend void swap(kstrie& a, kstrie& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------
    // Capacity
    // ------------------------------------------------------------------

    [[nodiscard]] bool      empty()        const noexcept { return impl_v.empty(); }
    [[nodiscard]] size_type size()         const noexcept { return impl_v.size(); }
    [[nodiscard]] size_type memory_usage() const noexcept { return impl_v.memory_usage(); }
    [[nodiscard]] size_type max_size()     const noexcept { return impl_v.max_size(); }
    [[nodiscard]] allocator_type get_allocator() const noexcept { return impl_v.get_allocator(); }

    // ------------------------------------------------------------------
    // Lookup
    // ------------------------------------------------------------------

    bool contains(std::string_view key) const { return impl_v.contains(key); }
    size_type count(std::string_view key) const { return impl_v.count(key); }

    // ------------------------------------------------------------------
    // const_iterator — live, bidirectional, parent-walk navigation.
    // Invalidated by any mutation (same as std::unordered_map).
    // operator* returns references: key by const ref to cached key_v,
    // value by const ref into the trie node.
    // ------------------------------------------------------------------

    class const_iterator {
        friend class kstrie;

        uint64_t*   leaf_v       = nullptr;  // compact node (null = end)
        uint16_t    pos_v        = 0;        // slot index in L/F/O/values
        std::string key_v;                   // cached reconstructed key
        size_t      prefix_len_v = 0;        // key_v length before entry suffix
        impl_t*     impl_p       = nullptr;  // for --end() and root access

        // End sentinel
        explicit const_iterator(impl_t* impl) : impl_p(impl) {}

        // Positioned
        const_iterator(impl_t* impl, uint64_t* leaf, uint16_t pos,
                       std::string key, size_t pfx_len)
            : leaf_v(leaf), pos_v(pos), key_v(std::move(key)),
              prefix_len_v(pfx_len), impl_p(impl) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const std::string, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::pair<const std::string&, const VALUE&>;

        struct arrow_proxy {
            std::pair<const std::string&, const VALUE&> p;
            const auto* operator->() const noexcept { return &p; }
        };
        using pointer = arrow_proxy;

        const_iterator() = default;

        reference operator*() const {
            hdr_type h = hdr_type::from_node(leaf_v);
            const auto* vb = h.get_compact_slots(leaf_v);
            return {key_v, *slots_type::load_value(vb, pos_v)};
        }

        arrow_proxy operator->() const { return {**this}; }

        const_iterator& operator++() {
            hdr_type h = hdr_type::from_node(leaf_v);
            if (pos_v + 1 < h.count) {
                // Hot path: advance within leaf
                ++pos_v;
                rebuild_suffix();
                return *this;
            }
            // Cold path: walk to next leaf
            walk(kstrie_detail::dir_t::FWD);
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        const_iterator& operator--() {
            if (!leaf_v) {
                // --end(): walk to last entry
                walk_to_edge(kstrie_detail::dir_t::BWD);
                return *this;
            }
            if (pos_v > 0) {
                // Hot path: retreat within leaf
                --pos_v;
                rebuild_suffix();
                return *this;
            }
            // Cold path: walk to prev leaf
            walk(kstrie_detail::dir_t::BWD);
            return *this;
        }

        const_iterator operator--(int) {
            const_iterator tmp = *this;
            --*this;
            return tmp;
        }

        bool operator==(const const_iterator& o) const noexcept {
            if (!leaf_v && !o.leaf_v) return true;
            if (!leaf_v || !o.leaf_v) return false;
            return leaf_v == o.leaf_v && pos_v == o.pos_v;
        }
        bool operator!=(const const_iterator& o) const noexcept {
            return !(*this == o);
        }

    private:
        // Rebuild suffix portion of key_v from current leaf_v/pos_v.
        void rebuild_suffix() {
            hdr_type h = hdr_type::from_node(leaf_v);
            const uint8_t* L = compact_type::lengths(leaf_v, h);
            const uint8_t* F = compact_type::firsts(leaf_v, h);
            const kstrie_detail::ks_offset_type* O = compact_type::offsets(leaf_v, h);
            const uint8_t* B = compact_type::keysuffix(leaf_v, h);

            key_v.resize(prefix_len_v);
            uint8_t klen = L[pos_v];
            if (klen > 0) [[likely]] {
                key_v.push_back(static_cast<char>(CHARMAP::from_index(F[pos_v])));
                if (klen > 1) [[likely]]
                    append_unmapped(key_v, B + O[pos_v], klen - 1);
            }
        }

        // Walk to edge (min/max) of entire trie. Used by --end().
        void walk_to_edge(kstrie_detail::dir_t dir) {
            uint64_t* root = impl_p->get_root_mut();
            if (root == compact_type::sentinel()) return;  // stay at end
            key_v.clear();
            edge_entry(root, dir);
        }

        // Descend from node to min (FWD) or max (BWD) entry.
        // Appends skip bytes and dispatch bytes to key_v.
        // Sets leaf_v, pos_v, prefix_len_v.
        void edge_entry(uint64_t* node, kstrie_detail::dir_t dir) {
            using dir_t = kstrie_detail::dir_t;
            while (true) {
                hdr_type h = hdr_type::from_node(node);

                // Append skip bytes
                if (h.has_skip()) [[unlikely]]
                    append_unmapped(key_v, hdr_type::get_skip(node, h),
                                   h.skip_bytes());

                if (h.is_compact()) {
                    // Reached leaf
                    leaf_v = node;
                    if (dir == dir_t::FWD)
                        pos_v = 0;
                    else
                        pos_v = static_cast<uint16_t>(h.count - 1);
                    prefix_len_v = key_v.size();
                    rebuild_suffix();
                    return;
                }

                // Bitmask: descend to min or max child
                if (dir == dir_t::FWD) {
                    // EOS first (sorts before all bytes)
                    uint64_t* eos = bitmask_type::eos_child(node, h);
                    if (eos != compact_type::sentinel()) {
                        node = eos;
                        continue;
                    }
                    // First byte child
                    const auto* bm = bitmask_type::get_bitmap(node, h);
                    int idx = bm->find_next_set(0);
                    if (idx < 0) [[unlikely]] return;  // empty bitmask
                    uint8_t byte = static_cast<uint8_t>(idx);
                    key_v.push_back(static_cast<char>(
                        CHARMAP::from_index(byte)));
                    int slot = bm->count_below(byte);
                    node = bitmask_type::child_by_slot(node, h, slot);
                } else {
                    // Last byte child first
                    const auto* bm = bitmask_type::get_bitmap(node, h);
                    constexpr int MAX_BIT =
                        static_cast<int>(CHARMAP::BITMAP_WORDS) * 64 - 1;
                    int idx = bm->find_prev_set(MAX_BIT);
                    if (idx >= 0) {
                        uint8_t byte = static_cast<uint8_t>(idx);
                        key_v.push_back(static_cast<char>(
                            CHARMAP::from_index(byte)));
                        int slot = bm->count_below(byte);
                        node = bitmask_type::child_by_slot(node, h, slot);
                        continue;
                    }
                    // No byte children — try EOS
                    uint64_t* eos = bitmask_type::eos_child(node, h);
                    if (eos != compact_type::sentinel()) {
                        node = eos;
                        continue;
                    }
                    return;  // empty
                }
            }
        }

        // Walk from current leaf to next/prev entry via parent chain.
        void walk(kstrie_detail::dir_t dir) {
            using dir_t = kstrie_detail::dir_t;
            using namespace kstrie_detail;

            hdr_type lh = hdr_type::from_node(leaf_v);
            uint64_t* parent = compact_type::get_parent(leaf_v);
            uint16_t byte = compact_type::get_parent_byte(leaf_v, lh);

            // Depth in key_v at the dispatch byte level above current leaf
            size_t depth = prefix_len_v - lh.skip_bytes();

            while (byte != ROOT_PARENT_BYTE) {
                hdr_type ph = hdr_type::from_node(parent);
                const auto* bm = bitmask_type::get_bitmap(parent, ph);

                if (dir == dir_t::FWD) {
                    // Forward: find next sibling after current byte
                    int sib = -1;
                    if (byte == EOS_PARENT_BYTE) {
                        // Was at EOS → first byte child
                        sib = bm->find_next_set(0);
                    } else {
                        sib = bm->find_next_set(byte + 1);
                    }
                    if (sib >= 0) {
                        // Found — truncate key, append new byte, descend
                        if (byte != EOS_PARENT_BYTE)
                            depth -= 1;  // remove old dispatch byte
                        key_v.resize(depth);
                        uint8_t sb = static_cast<uint8_t>(sib);
                        key_v.push_back(static_cast<char>(
                            CHARMAP::from_index(sb)));
                        int slot = bm->count_below(sb);
                        edge_entry(bitmask_type::child_by_slot(
                            parent, ph, slot), dir_t::FWD);
                        return;
                    }
                } else {
                    // Backward: find prev sibling before current byte
                    if (byte != EOS_PARENT_BYTE) {
                        int sib = bm->find_prev_set(byte - 1);
                        if (sib >= 0) {
                            depth -= 1;  // remove old dispatch byte
                            key_v.resize(depth);
                            uint8_t sb = static_cast<uint8_t>(sib);
                            key_v.push_back(static_cast<char>(
                                CHARMAP::from_index(sb)));
                            int slot = bm->count_below(sb);
                            edge_entry(bitmask_type::child_by_slot(
                                parent, ph, slot), dir_t::BWD);
                            return;
                        }
                        // No prev byte child — check EOS
                        uint64_t* eos = bitmask_type::eos_child(parent, ph);
                        if (eos != compact_type::sentinel()) {
                            depth -= 1;  // remove old dispatch byte
                            key_v.resize(depth);
                            edge_entry(eos, dir_t::BWD);
                            return;
                        }
                    }
                    // Was at EOS or no EOS → go up
                }

                // Go up one level
                if (byte != EOS_PARENT_BYTE)
                    depth -= 1;  // remove dispatch byte
                depth -= ph.skip_bytes();  // remove parent's skip
                byte = bitmask_type::get_parent_byte(parent);
                parent = bitmask_type::get_parent(parent);
            }

            // Reached root — no more entries
            leaf_v = nullptr;
            pos_v  = 0;
        }
    };

    using iterator               = const_iterator;
    using reverse_iterator       = std::reverse_iterator<const_iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // ------------------------------------------------------------------
    // Iterator access
    // ------------------------------------------------------------------

    const_iterator begin() const {
        uint64_t* root = const_cast<impl_t&>(impl_v).get_root_mut();
        if (root == compact_type::sentinel()) return end();
        const_iterator it(const_cast<impl_t*>(&impl_v));
        it.edge_entry(root, kstrie_detail::dir_t::FWD);
        return it;
    }

    const_iterator end() const {
        return const_iterator(const_cast<impl_t*>(&impl_v));
    }

    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() const { return reverse_iterator(end()); }
    reverse_iterator rend() const { return reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return rbegin(); }
    const_reverse_iterator crend() const { return rend(); }
    // ------------------------------------------------------------------
    // Modifiers
    // ------------------------------------------------------------------

    std::pair<const_iterator, bool> insert(std::string_view key, const VALUE& value) {
        auto r = impl_v.insert_for_iter(key, value,
                     kstrie_detail::insert_mode::INSERT);
        bool inserted = (r.outcome == kstrie_detail::insert_outcome::INSERTED);
        return {make_iter_from_result(r, key), inserted};
    }

    std::pair<const_iterator, bool> insert_or_assign(std::string_view key,
                                                      const VALUE& value) {
        auto r = impl_v.insert_for_iter(key, value,
                     kstrie_detail::insert_mode::UPSERT);
        bool inserted = (r.outcome == kstrie_detail::insert_outcome::INSERTED);
        return {make_iter_from_result(r, key), inserted};
    }

    template <typename... Args>
    std::pair<const_iterator, bool> emplace(std::string_view key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        return insert(key, v);
    }

    template <typename... Args>
    std::pair<const_iterator, bool> try_emplace(std::string_view key,
                                                 Args&&... args) {
        // Check existence first to avoid constructing value unnecessarily
        auto r = impl_v.find_for_iter(key);
        if (r.leaf) {
            size_t pfx = key.size() - compact_type::lengths(r.leaf,
                hdr_type::from_node(r.leaf))[r.pos];
            return {const_iterator(const_cast<impl_t*>(&impl_v),
                                   r.leaf, r.pos, std::string(key), pfx),
                    false};
        }
        VALUE v(std::forward<Args>(args)...);
        return insert(key, v);
    }

    size_type erase(std::string_view key) { return impl_v.erase(key); }

    const_iterator erase(const_iterator pos) {
        if (pos == end()) return end();
        // Save the next key before erasing — erase invalidates all iterators.
        const_iterator next = pos;
        ++next;
        bool had_next = (next != end());
        std::string next_key;
        if (had_next) next_key = next.key_v;
        impl_v.erase(pos.key_v);
        if (!had_next) return end();
        // Re-find the next key (leaf pointers may have changed)
        auto r = impl_v.find_for_iter(next_key);
        if (!r.leaf) return end();
        return make_iter_from_result(
            kstrie_detail::insert_result{nullptr,
                kstrie_detail::insert_outcome::FOUND, r.leaf, r.pos},
            next_key);
    }

    const_iterator erase(const_iterator first, const_iterator last) {
        // Collect keys, then erase — iterators invalidate on mutation
        std::vector<std::string> keys;
        for (auto it = first; it != last; ++it)
            keys.push_back((*it).first);
        for (auto& k : keys)
            impl_v.erase(k);
        if (last == end()) return end();
        // Re-find last's key (it may have shifted)
        auto r = impl_v.find_for_iter(last.key_v);
        if (!r.leaf) return end();
        size_t pfx = last.key_v.size() - compact_type::lengths(r.leaf,
            hdr_type::from_node(r.leaf))[r.pos];
        return const_iterator(const_cast<impl_t*>(&impl_v),
                              r.leaf, r.pos, last.key_v, pfx);
    }

    void clear() noexcept { impl_v.clear(); }

    // ------------------------------------------------------------------
    // Element access
    // ------------------------------------------------------------------

    const VALUE& at(std::string_view key) const {
        const VALUE* v = impl_v.find(key);
        if (!v) throw std::out_of_range("kstrie::at");
        return *v;
    }

    // operator[]: find or default-insert, return mutable reference.
    // For bool: returns bool_ref proxy. For others: returns VALUE&.
    mapped_ref operator[](std::string_view key) {
        auto r = impl_v.insert_for_iter(key, VALUE{},
                     kstrie_detail::insert_mode::INSERT);
        hdr_type h = hdr_type::from_node(r.leaf);
        auto* vb = h.get_compact_slots(r.leaf);
        if constexpr (IS_BITMAP) {
            constexpr size_t LOG2_BPW = slots_type::LOG2_BPW;
            constexpr size_t WORD_BIT_MASK = slots_type::WORD_BIT_MASK;
            return bool_ref{vb + (r.pos >> LOG2_BPW),
                            static_cast<uint8_t>(r.pos & WORD_BIT_MASK)};
        } else {
            return *slots_type::load_value(vb, r.pos);
        }
    }


    // ------------------------------------------------------------------
    // Lookup
    // ------------------------------------------------------------------

    const_iterator find(std::string_view key) const {
        auto r = impl_v.find_for_iter(key);
        if (!r.leaf) return end();
        return const_iterator(const_cast<impl_t*>(&impl_v),
                              r.leaf, r.pos, std::string(key), r.prefix_len);
    }

    // ------------------------------------------------------------------
    // Ordered lookup
    // ------------------------------------------------------------------

    const_iterator lower_bound(std::string_view key) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        const_iterator it(const_cast<impl_t*>(&impl_v));
        find_ge_iter(impl_v.get_root(), mapped, len, 0, it);
        return it;
    }

    const_iterator upper_bound(std::string_view key) const {
        auto it = lower_bound(key);
        if (it == end()) return it;
        if (it.key_v == key) ++it;
        return it;
    }

    std::pair<const_iterator, const_iterator>
    equal_range(std::string_view key) const {
        return {lower_bound(key), upper_bound(key)};
    }

    std::pair<const_iterator, const_iterator>
    prefix(std::string_view pfx) const {
        auto [k1, v1, k2, v2] = iter_prefix_bounds(pfx);
        const_iterator first = end(), last = end();
        if (v1) {
            auto r1 = impl_v.find_for_iter(k1);
            if (r1.leaf)
                first = const_iterator(const_cast<impl_t*>(&impl_v),
                                       r1.leaf, r1.pos, std::move(k1), r1.prefix_len);
        }
        if (v2) {
            auto r2 = impl_v.find_for_iter(k2);
            if (r2.leaf)
                last = const_iterator(const_cast<impl_t*>(&impl_v),
                                      r2.leaf, r2.pos, std::move(k2), r2.prefix_len);
        }
        return {first, last};
    }

    // ------------------------------------------------------------------
    // Prefix operations — bulk subtree queries
    // ------------------------------------------------------------------

    size_t prefix_count(std::string_view pfx) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());
        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(
            raw, len, stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
        size_t result = impl_v.prefix_count_impl(mapped, len);
        return result;
    }

    template<typename F>
    void prefix_walk(std::string_view pfx, F&& fn) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());
        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(
            raw, len, stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
        impl_v.prefix_walk_impl(mapped, len, pfx, std::forward<F>(fn));
    }

    std::vector<std::pair<std::string, VALUE>>
    prefix_vector(std::string_view pfx) const {
        std::vector<std::pair<std::string, VALUE>> result;
        prefix_walk(pfx, [&](std::string_view key, const VALUE& val) {
            result.emplace_back(std::string(key), val);
        });
        return result;
    }

    kstrie prefix_copy(std::string_view pfx) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());
        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(
            raw, len, stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
        kstrie result;
        auto r = impl_v.prefix_clone(mapped, len, result.impl_v.get_mem());
        if (r.cloned != impl_v.get_sentinel()) {
            if (r.path_len > 0)
                r.cloned = result.impl_v.reskip_with_prefix(
                    r.cloned, mapped, r.path_len);
            result.impl_v.set_root(r.cloned, r.count);
        }
        return result;
    }

    size_t prefix_erase(std::string_view pfx) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());
        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(
            raw, len, stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
        size_t result = impl_v.prefix_erase(mapped, len);
        return result;
    }

    kstrie prefix_split(std::string_view pfx) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());
        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(
            raw, len, stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
        auto r = impl_v.prefix_split_impl(mapped, len);
        kstrie result;
        if (r.stolen != impl_v.get_sentinel()) {
            if (r.path_len > 0)
                r.stolen = impl_v.reskip_with_prefix(
                    r.stolen, mapped, r.path_len);
            result.impl_v.set_root(r.stolen, r.count);
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Comparison
    // ------------------------------------------------------------------

    bool operator==(const kstrie& o) const {
        if (impl_v.size() != o.impl_v.size()) return false;
        auto a = begin(), b = o.begin();
        while (a != end()) {
            auto [ak, av] = *a;
            auto [bk, bv] = *b;
            if (ak != bk || av != bv) return false;
            ++a; ++b;
        }
        return true;
    }

    bool operator!=(const kstrie& o) const { return !(*this == o); }

private:


    // Construct iterator from insert_result + original key.
    const_iterator make_iter_from_result(
            const kstrie_detail::insert_result& r,
            std::string_view key) const {
        if (!r.leaf) return end();
        hdr_type lh = hdr_type::from_node(r.leaf);
        size_t pfx = key.size() - compact_type::lengths(r.leaf, lh)[r.pos];
        return const_iterator(const_cast<impl_t*>(&impl_v),
                              r.leaf, r.pos, std::string(key), pfx);
    }

    // ------------------------------------------------------------------
    // Unmap helper
    // ------------------------------------------------------------------

    static void append_unmapped(std::string& out, const uint8_t* data,
                                uint32_t len) {
        impl_t::append_unmapped(out, data, len);
    }

    const VALUE* find_min_impl(const uint64_t* node, std::string& out) const {
        if (node == impl_v.get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(out, hdr_type::get_skip(node, h), sb);
        }

        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[0];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[0], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[0], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), 0);
        }

        // Bitmask: check eos first, then first child
        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != impl_v.get_sentinel()) {
            return find_min_impl(eos, out);
        }
        const auto* bm = bitmask_type::get_bitmap(node, h);
        int idx = bm->find_next_set(0);
        if (idx < 0) [[unlikely]] return nullptr;
        uint8_t byte = static_cast<uint8_t>(idx);
        out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
        int slot = bm->count_below(byte);
        return find_min_impl(bitmask_type::child_by_slot(node, h, slot), out);
    }
    // ------------------------------------------------------------------
    // find_ge_iter — single-walk lower_bound constructing iterator directly
    // Returns true if positioned, false if no entry >= key.
    // ------------------------------------------------------------------

    bool find_ge_iter(const uint64_t* node,
                      const uint8_t* mapped, uint32_t key_len,
                      uint32_t consumed,
                      const_iterator& it) const {
        if (node == impl_v.get_sentinel()) [[unlikely]] return false;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            const uint8_t* skip = hdr_type::get_skip(node, h);
            uint32_t remaining = key_len - consumed;
            uint32_t cmp_len = std::min(sb, remaining);

            for (uint32_t i = 0; i < cmp_len; ++i) {
                if (skip[i] < mapped[consumed + i])
                    return false;
                if (skip[i] > mapped[consumed + i]) {
                    append_unmapped(it.key_v, skip, sb);
                    consumed += sb;
                    goto take_min;
                }
                it.key_v.push_back(static_cast<char>(
                    CHARMAP::from_index(skip[i])));
            }

            if (remaining < sb) [[unlikely]] {
                for (uint32_t j = cmp_len; j < sb; ++j)
                    it.key_v.push_back(static_cast<char>(
                        CHARMAP::from_index(skip[j])));
                consumed += sb;
                goto take_min;
            }

            consumed += sb;
        }

        if (h.is_compact()) [[unlikely]] {
            auto [found, pos] = compact_type::find_pos(
                node, h, mapped + consumed, key_len - consumed);
            if (pos >= h.count) [[unlikely]] return false;
            it.leaf_v = const_cast<uint64_t*>(node);
            it.pos_v  = static_cast<uint16_t>(pos);
            it.prefix_len_v = it.key_v.size();
            it.rebuild_suffix();
            return true;
        }

        if (consumed == key_len) [[unlikely]]
            goto take_min;

        {
            uint8_t byte = mapped[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);

            if (child != impl_v.get_sentinel()) {
                size_t save_len = it.key_v.size();
                it.key_v.push_back(static_cast<char>(
                    CHARMAP::from_index(byte)));

                if (find_ge_iter(child, mapped, key_len, consumed, it))
                    [[likely]] return true;
                it.key_v.resize(save_len);
            }

            const auto* bm = bitmask_type::get_bitmap(node, h);
            int next_idx = bm->find_next_set(byte + 1);
            if (next_idx < 0) [[unlikely]] return false;

            uint8_t next_byte = static_cast<uint8_t>(next_idx);
            it.key_v.push_back(static_cast<char>(
                CHARMAP::from_index(next_byte)));
            int slot = bm->count_below(next_byte);
            it.edge_entry(bitmask_type::child_by_slot(node, h, slot),
                          kstrie_detail::dir_t::FWD);
            return true;
        }

    take_min:
        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return false;
            it.leaf_v = const_cast<uint64_t*>(node);
            it.pos_v  = 0;
            it.prefix_len_v = it.key_v.size();
            it.rebuild_suffix();
            return true;
        }
        // Bitmask take_min: eos first, then first byte child
        {
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos != impl_v.get_sentinel()) {
                it.edge_entry(eos, kstrie_detail::dir_t::FWD);
                return true;
            }
            const auto* bm = bitmask_type::get_bitmap(node, h);
            int idx = bm->find_next_set(0);
            if (idx < 0) [[unlikely]] return false;
            uint8_t byte = static_cast<uint8_t>(idx);
            it.key_v.push_back(static_cast<char>(
                CHARMAP::from_index(byte)));
            int slot = bm->count_below(byte);
            it.edge_entry(bitmask_type::child_by_slot(node, h, slot),
                          kstrie_detail::dir_t::FWD);
            return true;
        }
    }


    // ------------------------------------------------------------------
    // iter_prefix_bounds
    // ------------------------------------------------------------------

    struct prefix_result {
        std::string  k1;
        const VALUE* v1;
        std::string  k2;
        const VALUE* v2;
    };

    prefix_result iter_prefix_bounds(std::string_view pfx) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(pfx.data());
        uint32_t len = static_cast<uint32_t>(pfx.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = kstrie_detail::get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        struct right_turn {
            const uint64_t* bm_node;
            hdr_type        h;
            int             next_idx;
            std::string     prefix;
        };

        std::string path;
        right_turn best_rt{};
        bool have_rt = false;

        const uint64_t* node = impl_v.get_root();
        uint32_t consumed = 0;

        while (consumed < len) {
            if (node == impl_v.get_sentinel()) [[unlikely]] goto not_found;
            hdr_type h = hdr_type::from_node(node);

            {
                if (h.has_skip()) [[unlikely]] {
                    uint32_t sb = h.skip_bytes();
                    const uint8_t* skip = hdr_type::get_skip(node, h);
                    uint32_t remaining = len - consumed;
                    uint32_t cmp_len = std::min(sb, remaining);

                    for (uint32_t i = 0; i < cmp_len; ++i) {
                        if (skip[i] != mapped[consumed + i])
                            goto not_found;
                        path.push_back(static_cast<char>(
                            CHARMAP::from_index(skip[i])));
                    }

                    if (remaining <= sb) {
                        for (uint32_t j = cmp_len; j < sb; ++j)
                            path.push_back(static_cast<char>(
                                CHARMAP::from_index(skip[j])));
                        consumed += sb;
                        goto subtree_found;
                    }

                    consumed += sb;
                }
            }

            if (h.is_compact()) [[unlikely]]
                goto subtree_found;

            {
                uint8_t byte = mapped[consumed++];
                const auto* bm = bitmask_type::get_bitmap(node, h);

                int next_sib = bm->find_next_set(byte + 1);
                if (next_sib >= 0) {
                    best_rt.bm_node  = node;
                    best_rt.h        = h;
                    best_rt.next_idx = next_sib;
                    best_rt.prefix   = path;
                    have_rt = true;
                }

                uint64_t* child = bitmask_type::dispatch(node, h, byte);
                if (child == impl_v.get_sentinel()) [[unlikely]] goto not_found;

                path.push_back(static_cast<char>(
                    CHARMAP::from_index(byte)));
                node = child;
            }
        }

    subtree_found:
        {
            hdr_type h = hdr_type::from_node(node);

            std::string k1 = path;
            const VALUE* v1;

            if (h.is_compact() && consumed < len) [[unlikely]] {
                v1 = find_prefix_first_in_compact(
                    node, h, mapped + consumed, len - consumed, k1);
            } else {
                v1 = find_min_impl_tail(node, h, k1);
            }

            if (!v1) {
                return {{}, nullptr, {}, nullptr};
            }

            std::string k2;
            const VALUE* v2 = nullptr;

            if (h.is_compact() && consumed < len) [[unlikely]] {
                v2 = find_prefix_past_in_compact(
                    node, h, mapped + consumed, len - consumed, path, k2);
            } else if (have_rt) {
                k2 = best_rt.prefix;
                uint8_t rt_byte = static_cast<uint8_t>(best_rt.next_idx);
                k2.push_back(static_cast<char>(CHARMAP::from_index(rt_byte)));
                const auto* bm = bitmask_type::get_bitmap(
                    best_rt.bm_node, best_rt.h);
                int slot = bm->count_below(rt_byte);
                v2 = find_min_impl(
                    bitmask_type::child_by_slot(
                        best_rt.bm_node, best_rt.h, slot), k2);
            }

            return {std::move(k1), v1, std::move(k2), v2};
        }

    not_found:
        return {{}, nullptr, {}, nullptr};
    }

    // ------------------------------------------------------------------
    // Compact prefix helpers
    // ------------------------------------------------------------------

    const VALUE* find_prefix_first_in_compact(
            const uint64_t* node, const hdr_type& h,
            const uint8_t* suffix, uint32_t suffix_len,
            std::string& out) const {
        const uint8_t*  L  = compact_type::lengths(node, h);
        const uint8_t*  F  = compact_type::firsts(node, h);
        const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t*  B  = compact_type::keysuffix(node, h);
        for (int i = 0; i < h.count; ++i) {
            uint8_t klen = L[i];
            uint8_t tmp[256];
            if (klen > 0) [[likely]] {
                tmp[0] = F[i];
                if (klen > 1) [[likely]] std::memcpy(tmp + 1, B + O[i], klen - 1);
            }
            if (klen >= suffix_len &&
                std::memcmp(tmp, suffix, suffix_len) == 0) {
                if (klen > 0) [[likely]] {
                    append_unmapped(out, &F[i], 1);
                    if (klen > 1) [[likely]] append_unmapped(out, B + O[i], klen - 1);
                }
                return slots_type::load_value(h.get_compact_slots(node), i);
            }
            if (klen >= suffix_len) {
                int cmp = std::memcmp(tmp, suffix, suffix_len);
                if (cmp > 0) return nullptr;
            }
        }
        return nullptr;
    }

    const VALUE* find_prefix_past_in_compact(
            const uint64_t* node, const hdr_type& h,
            const uint8_t* suffix, uint32_t suffix_len,
            const std::string& base_path,
            std::string& out) const {
        const uint8_t*  L  = compact_type::lengths(node, h);
        const uint8_t*  F  = compact_type::firsts(node, h);
        const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t*  B  = compact_type::keysuffix(node, h);
        bool in_prefix = false;
        for (int i = 0; i < h.count; ++i) {
            uint8_t klen = L[i];
            uint8_t tmp[256];
            if (klen > 0) [[likely]] {
                tmp[0] = F[i];
                if (klen > 1) [[likely]] std::memcpy(tmp + 1, B + O[i], klen - 1);
            }
            bool matches = (klen >= suffix_len &&
                            std::memcmp(tmp, suffix, suffix_len) == 0);
            if (matches) {
                in_prefix = true;
            } else if (in_prefix) {
                out = base_path;
                if (klen > 0) [[likely]] {
                    append_unmapped(out, &F[i], 1);
                    if (klen > 1) [[likely]] append_unmapped(out, B + O[i], klen - 1);
                }
                return slots_type::load_value(h.get_compact_slots(node), i);
            }
        }
        return nullptr;
    }

    const VALUE* find_min_impl_tail(const uint64_t* node, const hdr_type& h,
                                    std::string& out) const {
        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[0];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[0], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[0], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), 0);
        }
        // Bitmask: eos first, then first child
        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != impl_v.get_sentinel())
            return find_min_impl(eos, out);
        const auto* bm = bitmask_type::get_bitmap(node, h);
        int idx = bm->find_next_set(0);
        if (idx < 0) [[unlikely]] return nullptr;
        uint8_t byte = static_cast<uint8_t>(idx);
        out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
        return find_min_impl(
            bitmask_type::child_by_slot(node, h, bm->count_below(byte)), out);
    }
};

} // namespace gteitelbaum

#endif // KSTRIE_HPP
