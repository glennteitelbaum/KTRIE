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

    kstrie(kstrie&& o) noexcept : impl_v(std::move(o.impl_v)) { fix_end(); }
    kstrie& operator=(kstrie&& o) noexcept {
        impl_v = std::move(o.impl_v);
        fix_end();
        return *this;
    }
    kstrie(const kstrie& o) : impl_v(o.impl_v) { fix_end(); }
    kstrie& operator=(const kstrie& o) {
        impl_v = o.impl_v;
        fix_end();
        return *this;
    }

    void swap(kstrie& o) noexcept {
        impl_v.swap(o.impl_v);
        fix_end();
        o.fix_end();
    }
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
    // Key reconstruction is lazy — only built on dereference via
    // lazy_key proxy. Value-only iteration has zero key overhead.
    // ------------------------------------------------------------------

    class const_iterator {
        friend class kstrie;

        uint64_t*  leaf_v   = nullptr;  // compact node (null = end)
        uint16_t   pos_v    = 0;        // slot index
        mutable char*   key_buf  = nullptr;  // owned buffer
        mutable size_t  key_len  = 0;
        mutable size_t  key_cap  = 0;
        impl_t*    impl_p  = nullptr;   // for --end() and root access

        // End sentinel
        explicit const_iterator(impl_t* impl) : impl_p(impl) {}

        // Positioned (lazy — no key built)
        const_iterator(impl_t* impl, uint64_t* leaf, uint16_t pos)
            : leaf_v(leaf), pos_v(pos), impl_p(impl) {}

        // Free key buffer. len/cap are stale but never read —
        // ensure_key() overwrites all three when key_buf is set.
        void invalidate_key() const noexcept {
            delete[] key_buf;
            key_buf = nullptr;
        }

        // Build key on demand by walking from leaf to root via prepend
        void ensure_key() const {
            if (key_buf) return;  // already built

            kstrie_detail::fast_string fs;

            // Step 1: leaf suffix + leaf skip
            hdr_type lh = hdr_type::from_node(leaf_v);
            const auto& pfx = compact_type::get_prefix(leaf_v, lh);
            const uint8_t* base = reinterpret_cast<const uint8_t*>(leaf_v)
                                + hdr_type::COMPACT_ARRAYS_OFF;
            const uint8_t* L = base;
            const uint8_t* F = base + pfx.cap;
            const kstrie_detail::ks_offset_type* O =
                reinterpret_cast<const kstrie_detail::ks_offset_type*>(F + pfx.cap);
            const uint8_t* B = reinterpret_cast<const uint8_t*>(leaf_v)
                             + pfx.skip_data_off + lh.skip_bytes();

            const uint8_t* skip = hdr_type::get_skip(leaf_v, lh);
            size_t skip_len = lh.skip_bytes();
            uint8_t klen = L[pos_v];

            if (klen == 0) [[unlikely]]
                fs.prepend(skip, skip_len);
            else if (klen == 1)
                fs.prepend(skip, skip_len,
                           CHARMAP::from_index(F[pos_v]));
            else
                fs.prepend(skip, skip_len,
                           CHARMAP::from_index(F[pos_v]),
                           B + O[pos_v], klen - 1);

            // Step 2: walk up from leaf to root
            // Leaf is always compact. Parents are always bitmask.
            uint64_t* node = compact_type::get_parent(leaf_v);
            uint16_t nb = compact_type::get_parent_byte(leaf_v, lh);

            while (node) {
                hdr_type h = hdr_type::from_node(node);
                const uint8_t* nsk = hdr_type::get_skip(node, h);
                size_t nsk_len = h.skip_bytes();

                if (nb == kstrie_detail::EOS_PARENT_BYTE) [[unlikely]]
                    fs.prepend(nsk, nsk_len);
                else
                    fs.prepend(nsk, nsk_len,
                               CHARMAP::from_index(static_cast<uint8_t>(nb)));

                nb = bitmask_type::get_parent_byte(node);
                node = bitmask_type::get_parent(node);
            }

            // Transfer buffer ownership from fast_string to iterator
            key_buf = fs.buf_pv;
            key_len = fs.len_v;
            key_cap = fs.cap_v;
            fs.buf_pv = nullptr;  // prevent fast_string from interfering
        }

    public:
        // -----------------------------------------------------------
        // lazy_key proxy — defers key construction until use
        // -----------------------------------------------------------

        struct lazy_key {
            const const_iterator* it_p;

            operator std::string() const {
                it_p->ensure_key();
                return std::string(it_p->key_buf, it_p->key_len);
            }

            friend bool operator==(const lazy_key& a, std::string_view b) {
                a.it_p->ensure_key();
                return std::string_view(a.it_p->key_buf, a.it_p->key_len) == b;
            }
            friend bool operator<(const lazy_key& a, std::string_view b) {
                a.it_p->ensure_key();
                return std::string_view(a.it_p->key_buf, a.it_p->key_len) < b;
            }
            friend bool operator<(std::string_view a, const lazy_key& b) {
                b.it_p->ensure_key();
                return a < std::string_view(b.it_p->key_buf, b.it_p->key_len);
            }
            friend bool operator>(const lazy_key& a, std::string_view b) { return b < a; }
            friend bool operator>(std::string_view a, const lazy_key& b) { return b < a; }
            friend bool operator<=(const lazy_key& a, std::string_view b) { return !(b < a); }
            friend bool operator>=(const lazy_key& a, std::string_view b) { return !(a < b); }
            friend bool operator<=(std::string_view a, const lazy_key& b) { return !(b < a); }
            friend bool operator>=(std::string_view a, const lazy_key& b) { return !(a < b); }
            friend bool operator!=(const lazy_key& a, std::string_view b) { return !(a == b); }
        };

        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<std::string, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::pair<lazy_key, const VALUE&>;

        struct arrow_proxy {
            std::pair<lazy_key, const VALUE&> p;
            const auto* operator->() const noexcept { return &p; }
        };
        using pointer = arrow_proxy;

        const_iterator() = default;

        ~const_iterator() { delete[] key_buf; }

        // Move — steals buffer pointer
        const_iterator(const_iterator&& o) noexcept
            : leaf_v(o.leaf_v), pos_v(o.pos_v),
              key_buf(o.key_buf), key_len(o.key_len), key_cap(o.key_cap),
              impl_p(o.impl_p) {
            o.key_buf = nullptr;
            o.leaf_v = nullptr;
        }

        const_iterator& operator=(const_iterator&& o) noexcept {
            if (this != &o) {
                delete[] key_buf;
                leaf_v  = o.leaf_v;
                pos_v   = o.pos_v;
                key_buf = o.key_buf;
                key_len = o.key_len;
                key_cap = o.key_cap;
                impl_p  = o.impl_p;
                o.key_buf = nullptr;
                o.leaf_v = nullptr;
            }
            return *this;
        }

        // Copy — deep-copies buffer only if key was built
        const_iterator(const const_iterator& o)
            : leaf_v(o.leaf_v), pos_v(o.pos_v),
              key_len(o.key_len), key_cap(o.key_cap),
              impl_p(o.impl_p) {
            if (o.key_buf) {
                key_buf = new char[o.key_cap];
                std::memcpy(key_buf, o.key_buf, o.key_len);
            }
        }

        const_iterator& operator=(const const_iterator& o) {
            if (this != &o) {
                delete[] key_buf;
                leaf_v  = o.leaf_v;
                pos_v   = o.pos_v;
                key_len = o.key_len;
                key_cap = o.key_cap;
                impl_p  = o.impl_p;
                if (o.key_buf) {
                    key_buf = new char[o.key_cap];
                    std::memcpy(key_buf, o.key_buf, o.key_len);
                } else {
                    key_buf = nullptr;
                }
            }
            return *this;
        }

        reference operator*() const {
            hdr_type h = hdr_type::from_node(leaf_v);
            const auto* vb = h.get_compact_slots(leaf_v);
            return {lazy_key{this}, *slots_type::load_value(vb, pos_v)};
        }

        arrow_proxy operator->() const { return {**this}; }

        const_iterator& operator++() {
            invalidate_key();
            hdr_type h = hdr_type::from_node(leaf_v);
            if (pos_v + 1 < h.count) {
                ++pos_v;
                return *this;
            }
            walk(kstrie_detail::dir_t::FWD);
            return *this;
        }

        const_iterator& operator--() {
            invalidate_key();
            if (!leaf_v) {
                walk_to_edge(kstrie_detail::dir_t::BWD);
                return *this;
            }
            if (pos_v > 0) {
                --pos_v;
                return *this;
            }
            walk(kstrie_detail::dir_t::BWD);
            return *this;
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
        void walk_to_edge(kstrie_detail::dir_t dir) {
            uint64_t* root = impl_p->get_root_mut();
            if (root == compact_type::sentinel()) return;
            edge_entry(root, dir);
        }

        // Pure descent — no key ops
        void edge_entry(uint64_t* node, kstrie_detail::dir_t dir) {
            using dir_t = kstrie_detail::dir_t;
            while (true) {
                hdr_type h = hdr_type::from_node(node);

                if (h.is_compact()) {
                    leaf_v = node;
                    if (dir == dir_t::FWD)
                        pos_v = 0;
                    else
                        pos_v = static_cast<uint16_t>(h.count - 1);
                    return;
                }

                if (dir == dir_t::FWD) {
                    uint64_t* eos = bitmask_type::eos_child(node, h);
                    if (eos != compact_type::sentinel()) {
                        node = eos;
                        continue;
                    }
                    const auto* bm = bitmask_type::get_bitmap(node, h);
                    int idx = bm->find_next_set(0);
                    if (idx < 0) [[unlikely]] return;
                    int slot = bm->count_below(static_cast<uint8_t>(idx));
                    node = bitmask_type::child_by_slot(node, h, slot);
                } else {
                    const auto* bm = bitmask_type::get_bitmap(node, h);
                    constexpr int MAX_BIT =
                        static_cast<int>(CHARMAP::BITMAP_WORDS) * 64 - 1;
                    int idx = bm->find_prev_set(MAX_BIT);
                    if (idx >= 0) {
                        int slot = bm->count_below(static_cast<uint8_t>(idx));
                        node = bitmask_type::child_by_slot(node, h, slot);
                        continue;
                    }
                    uint64_t* eos = bitmask_type::eos_child(node, h);
                    if (eos != compact_type::sentinel()) {
                        node = eos;
                        continue;
                    }
                    return;
                }
            }
        }

        // Pure navigation — no key ops
        void walk(kstrie_detail::dir_t dir) {
            using dir_t = kstrie_detail::dir_t;
            using namespace kstrie_detail;

            hdr_type lh = hdr_type::from_node(leaf_v);
            uint64_t* parent = compact_type::get_parent(leaf_v);
            uint16_t byte = compact_type::get_parent_byte(leaf_v, lh);

            while (byte != ROOT_PARENT_BYTE) {
                hdr_type ph = hdr_type::from_node(parent);
                const auto* bm = bitmask_type::get_bitmap(parent, ph);

                if (dir == dir_t::FWD) {
                    int sib = -1;
                    if (byte == EOS_PARENT_BYTE)
                        sib = bm->find_next_set(0);
                    else
                        sib = bm->find_next_set(byte + 1);
                    if (sib >= 0) {
                        int slot = bm->count_below(static_cast<uint8_t>(sib));
                        edge_entry(bitmask_type::child_by_slot(
                            parent, ph, slot), dir_t::FWD);
                        return;
                    }
                } else {
                    if (byte != EOS_PARENT_BYTE) {
                        int sib = bm->find_prev_set(byte - 1);
                        if (sib >= 0) {
                            int slot = bm->count_below(static_cast<uint8_t>(sib));
                            edge_entry(bitmask_type::child_by_slot(
                                parent, ph, slot), dir_t::BWD);
                            return;
                        }
                        uint64_t* eos = bitmask_type::eos_child(parent, ph);
                        if (eos != compact_type::sentinel()) {
                            edge_entry(eos, dir_t::BWD);
                            return;
                        }
                    }
                }

                byte = bitmask_type::get_parent_byte(parent);
                parent = bitmask_type::get_parent(parent);
            }

            leaf_v = nullptr;
            pos_v  = 0;
        }
    };

    using iterator               = const_iterator;
    using reverse_iterator       = std::reverse_iterator<const_iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

private:
    mutable const_iterator end_v{const_cast<impl_t*>(&impl_v)};

    void fix_end() noexcept { end_v.impl_p = const_cast<impl_t*>(&impl_v); }

public:

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

    const const_iterator& end() const noexcept { return end_v; }

    const_iterator cbegin() const { return begin(); }
    const const_iterator& cend() const noexcept { return end_v; }

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
        return {make_iter_from_result(r), inserted};
    }

    std::pair<const_iterator, bool> insert_or_assign(std::string_view key,
                                                      const VALUE& value) {
        auto r = impl_v.insert_for_iter(key, value,
                     kstrie_detail::insert_mode::UPSERT);
        bool inserted = (r.outcome == kstrie_detail::insert_outcome::INSERTED);
        return {make_iter_from_result(r), inserted};
    }

    template <typename... Args>
    std::pair<const_iterator, bool> emplace(std::string_view key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        return insert(key, v);
    }

    template <typename... Args>
    std::pair<const_iterator, bool> try_emplace(std::string_view key,
                                                 Args&&... args) {
        auto r = impl_v.find_for_iter(key);
        if (r.leaf)
            return {const_iterator(const_cast<impl_t*>(&impl_v),
                                   r.leaf, r.pos), false};
        VALUE v(std::forward<Args>(args)...);
        return insert(key, v);
    }

    size_type erase(std::string_view key) { return impl_v.erase(key); }

    const_iterator erase(const const_iterator& pos) {
        if (pos == end()) return end();
        // Build key from iterator position, erase, find next
        std::string key = (*pos).first;
        impl_v.erase(key);
        return lower_bound(key);
    }

    const_iterator erase(const const_iterator& first, const const_iterator& last) {
        if (first == last) {
            if (last == end()) return end();
            std::string lk = (*last).first;
            return lower_bound(lk);
        }
        std::string first_key = (*first).first;
        std::string last_key;
        bool have_last = (last != end());
        if (have_last) last_key = (*last).first;

        std::vector<std::string> keys;
        for (auto it = lower_bound(first_key); it != end(); ++it) {
            std::string k = (*it).first;
            if (have_last && k >= last_key) break;
            keys.push_back(std::move(k));
        }
        for (auto& k : keys)
            impl_v.erase(k);
        if (!have_last) return end();
        return lower_bound(last_key);
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
        return const_iterator(const_cast<impl_t*>(&impl_v), r.leaf, r.pos);
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
        // Build key lazily for comparison
        if ((*it).first == key) ++it;
        return it;
    }

    std::pair<const_iterator, const_iterator>
    equal_range(std::string_view key) const {
        return {lower_bound(key), upper_bound(key)};
    }

    std::pair<const_iterator, const_iterator>
    prefix(std::string_view pfx) const {
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
        };

        right_turn best_rt{};
        bool have_rt = false;

        const uint64_t* node = impl_v.get_root();
        uint32_t consumed = 0;

        while (consumed < len) {
            if (node == impl_v.get_sentinel()) [[unlikely]] return {end(), end()};
            hdr_type h = hdr_type::from_node(node);

            if (h.has_skip()) [[unlikely]] {
                uint32_t sb = h.skip_bytes();
                const uint8_t* skip = hdr_type::get_skip(node, h);
                uint32_t remaining = len - consumed;
                uint32_t cmp_len = std::min(sb, remaining);

                for (uint32_t i = 0; i < cmp_len; ++i) {
                    if (skip[i] != mapped[consumed + i])
                        return {end(), end()};
                }

                if (remaining <= sb) {
                    consumed += sb;
                    goto subtree_found;
                }

                consumed += sb;
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
                    have_rt = true;
                }

                uint64_t* child = bitmask_type::dispatch(node, h, byte);
                if (child == impl_v.get_sentinel()) [[unlikely]]
                    return {end(), end()};

                node = child;
            }
        }

    subtree_found:
        {
            hdr_type h = hdr_type::from_node(node);

            // --- Build first iterator (lazy — just set leaf/pos) ---
            const_iterator first(const_cast<impl_t*>(&impl_v));

            if (h.is_compact() && consumed < len) [[unlikely]] {
                int pos = find_prefix_first_pos(
                    node, h, mapped + consumed, len - consumed);
                if (pos < 0) return {end(), end()};
                first.leaf_v = const_cast<uint64_t*>(node);
                first.pos_v = static_cast<uint16_t>(pos);
            } else {
                first.edge_entry(const_cast<uint64_t*>(node),
                                 kstrie_detail::dir_t::FWD);
                if (!first.leaf_v) return {end(), end()};
            }

            // --- Build last (past-end) iterator ---
            const_iterator last(const_cast<impl_t*>(&impl_v));

            if (h.is_compact() && consumed < len) [[unlikely]] {
                int past = find_prefix_past_pos(
                    node, h, mapped + consumed, len - consumed);
                if (past >= 0) {
                    last.leaf_v = const_cast<uint64_t*>(node);
                    last.pos_v = static_cast<uint16_t>(past);
                }
            } else if (have_rt) {
                const auto* bm = bitmask_type::get_bitmap(
                    best_rt.bm_node, best_rt.h);
                int slot = bm->count_below(
                    static_cast<uint8_t>(best_rt.next_idx));
                last.edge_entry(bitmask_type::child_by_slot(
                    best_rt.bm_node, best_rt.h, slot),
                    kstrie_detail::dir_t::FWD);
            }

            return {std::move(first), std::move(last)};
        }
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


    // Construct iterator from insert_result (lazy — no key built).
    const_iterator make_iter_from_result(
            const kstrie_detail::insert_result& r) const {
        if (!r.leaf) return end();
        return const_iterator(const_cast<impl_t*>(&impl_v), r.leaf, r.pos);
    }

    // ------------------------------------------------------------------
    // Unmap helper
    // ------------------------------------------------------------------

    template<typename StringOut>
    static void append_unmapped(StringOut& out, const uint8_t* data,
                                uint32_t len) {
        impl_t::append_unmapped(out, data, len);
    }

    // ------------------------------------------------------------------
    // find_ge_iter — single-walk lower_bound, sets leaf_v/pos_v only.
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
                    consumed += sb;
                    goto take_min;
                }
            }

            if (remaining < sb) [[unlikely]] {
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
            return true;
        }

        if (consumed == key_len) [[unlikely]]
            goto take_min;

        {
            uint8_t byte = mapped[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);

            if (child != impl_v.get_sentinel()) {
                if (find_ge_iter(child, mapped, key_len, consumed, it))
                    [[likely]] return true;
            }

            const auto* bm = bitmask_type::get_bitmap(node, h);
            int next_idx = bm->find_next_set(byte + 1);
            if (next_idx < 0) [[unlikely]] return false;

            int slot = bm->count_below(static_cast<uint8_t>(next_idx));
            it.edge_entry(bitmask_type::child_by_slot(node, h, slot),
                          kstrie_detail::dir_t::FWD);
            return true;
        }

    take_min:
        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return false;
            it.leaf_v = const_cast<uint64_t*>(node);
            it.pos_v  = 0;
            return true;
        }
        {
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos != impl_v.get_sentinel()) {
                it.edge_entry(eos, kstrie_detail::dir_t::FWD);
                return true;
            }
            const auto* bm = bitmask_type::get_bitmap(node, h);
            int idx = bm->find_next_set(0);
            if (idx < 0) [[unlikely]] return false;
            int slot = bm->count_below(static_cast<uint8_t>(idx));
            it.edge_entry(bitmask_type::child_by_slot(node, h, slot),
                          kstrie_detail::dir_t::FWD);
            return true;
        }
    }


    // find_prefix_first_pos: position of first entry matching prefix suffix.
    // Returns -1 if none found.
    static int find_prefix_first_pos(const uint64_t* node, const hdr_type& h,
                                      const uint8_t* suffix,
                                      uint32_t suffix_len) noexcept {
        const uint8_t* L = compact_type::lengths(node, h);
        const uint8_t* F = compact_type::firsts(node, h);
        const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B = compact_type::keysuffix(node, h);
        for (int i = 0; i < h.count; ++i) {
            uint8_t klen = L[i];
            if (klen < suffix_len) continue;
            uint8_t tmp[256];
            if (klen > 0) [[likely]] {
                tmp[0] = F[i];
                if (klen > 1) [[likely]] std::memcpy(tmp + 1, B + O[i], klen - 1);
            }
            if (std::memcmp(tmp, suffix, suffix_len) == 0)
                return i;
            if (std::memcmp(tmp, suffix, suffix_len) > 0)
                return -1;  // past where it would be
        }
        return -1;
    }

    // find_prefix_past_pos: position of first entry AFTER all prefix-matching entries.
    // Returns -1 if no such entry (all remaining entries match, or none match).
    static int find_prefix_past_pos(const uint64_t* node, const hdr_type& h,
                                     const uint8_t* suffix,
                                     uint32_t suffix_len) noexcept {
        const uint8_t* L = compact_type::lengths(node, h);
        const uint8_t* F = compact_type::firsts(node, h);
        const kstrie_detail::ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B = compact_type::keysuffix(node, h);
        bool in_prefix = false;
        for (int i = 0; i < h.count; ++i) {
            uint8_t klen = L[i];
            bool matches = false;
            if (klen >= suffix_len) {
                uint8_t tmp[256];
                if (klen > 0) [[likely]] {
                    tmp[0] = F[i];
                    if (klen > 1) [[likely]] std::memcpy(tmp + 1, B + O[i], klen - 1);
                }
                matches = (std::memcmp(tmp, suffix, suffix_len) == 0);
            }
            if (matches) {
                in_prefix = true;
            } else if (in_prefix) {
                return i;
            }
        }
        return -1;
    }
};

} // namespace gteitelbaum

#endif // KSTRIE_HPP
