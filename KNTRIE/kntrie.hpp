#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>
#include <initializer_list>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<std::uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY> && sizeof(KEY) >= 2,
                  "KEY must be integral and at least 16 bits");

    using KO     = kntrie_detail::key_ops<KEY>;
    using UK     = typename KO::UK;
    using impl_t = kntrie_detail::kntrie_impl<UK, VALUE, ALLOC>;
    using VT     = kntrie_detail::value_traits<VALUE, ALLOC>;

    // Normalized VALUE for ops (same as impl uses)
    using NORM_V = kntrie_detail::normalized_ops_value_t<VALUE>;
    using BO     = kntrie_detail::bitmask_ops<UK, NORM_V, ALLOC>;
    using CO     = kntrie_detail::compact_ops<UK, NORM_V, ALLOC>;

    static constexpr bool IS_BOOL = std::is_same_v<VALUE, bool>;

    static VALUE& deref_val(void* vp) noexcept {
        if constexpr (VT::IS_INLINE)
            return *static_cast<VALUE*>(vp);
        else
            return **static_cast<VALUE**>(vp);
    }
    static const VALUE& deref_val_const(const void* vp) noexcept {
        if constexpr (VT::IS_INLINE)
            return *static_cast<const VALUE*>(vp);
        else
            return **static_cast<VALUE* const*>(vp);
    }

public:
    using key_type        = KEY;
    using mapped_type     = VALUE;
    using value_type      = std::pair<const KEY, VALUE>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type  = ALLOC;

    using mapped_ref       = std::conditional_t<IS_BOOL,
                                kntrie_detail::bool_ref, VALUE&>;
    using const_mapped_ref = std::conditional_t<IS_BOOL,
                                bool, const VALUE&>;

    // ==================================================================
    // Iterator — live, bidirectional
    //
    // Stores stored-form key (UK).  operator*() converts to user KEY.
    // Advance via impl_t::advance_pos — no function pointers.
    // ==================================================================

    class iterator {
        friend class kntrie;
        std::uint64_t* leaf_v = nullptr;
        std::uint16_t  pos_v  = 0;
        std::uint16_t  bit_v  = 0;
        UK             key_v  = UK{};
        void*          val_v  = nullptr;

        iterator(kntrie_detail::iter_entry_t<UK> e)
            : leaf_v(e.leaf), pos_v(e.pos), bit_v(e.bit),
              key_v(e.key), val_v(e.val) {}

        // End sentinel: leaf_v=null, val_v stashes impl* for --end()
        explicit iterator(impl_t* impl)
            : val_v(static_cast<void*>(impl)) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::pair<const KEY, mapped_ref>;

        struct arrow_proxy {
            reference p;
            auto* operator->() noexcept { return &p; }
        };
        using pointer = arrow_proxy;

        iterator() = default;

        reference operator*() const noexcept {
            KEY k = KO::to_user(key_v);
            if constexpr (IS_BOOL) {
                auto* base = static_cast<std::uint64_t*>(val_v);
                return {k, kntrie_detail::bool_ref{
                    base + pos_v / kntrie_detail::U64_BITS,
                    static_cast<std::uint8_t>(pos_v % kntrie_detail::U64_BITS)}};
            } else {
                return {k, deref_val(val_v)};
            }
        }

        arrow_proxy operator->() const noexcept {
            return {**this};
        }

        iterator& operator++() {
            using namespace kntrie_detail;
            auto* hdr = get_header(leaf_v);

            if (!hdr->is_bitmap()) [[likely]] {
                // Compact hot path: pos+1, grab key directly
                std::uint16_t next = pos_v + 1;
                if (next < hdr->entries()) [[likely]] {
                    pos_v = next;
                    key_v = CO::keys(leaf_v, COMPACT_HEADER_U64)[next];
                    if constexpr (!IS_BOOL)
                        val_v = static_cast<char*>(val_v) + sizeof(typename CO::VST);
                    return *this;
                }
            } else {
                // Bitmap hot path: next set bit, replace last byte
                auto r = BO::bm(leaf_v, BITMAP_LEAF_HEADER_U64).next_bit_after(
                    static_cast<std::uint8_t>(bit_v));
                if (r.found) {
                    bit_v = r.idx;
                    key_v = (key_v & ~UK(0xFF)) | UK(r.idx);
                    if constexpr (IS_BOOL) {
                        pos_v = static_cast<std::uint16_t>(r.idx);
                    } else {
                        pos_v++;
                        val_v = static_cast<char*>(val_v) + sizeof(typename CO::VST);
                    }
                    return *this;
                }
            }
            // Leaf exhausted — walk to next
            auto e = impl_t::walk_from_leaf(leaf_v, dir_t::FWD);
            leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
            key_v = e.key; val_v = e.val;
            return *this;
        }
        iterator operator++(int) { auto t = *this; ++(*this); return t; }

        iterator& operator--() {
            using namespace kntrie_detail;
            if (!leaf_v) {
                auto* impl = static_cast<impl_t*>(val_v);
                if (!impl) return *this;
                auto e = impl->edge_entry(dir_t::BWD);
                if (!e.leaf) return *this;
                leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
                key_v = e.key; val_v = e.val;
                return *this;
            }
            auto* hdr = get_header(leaf_v);

            if (!hdr->is_bitmap()) [[likely]] {
                if (pos_v > 0) [[likely]] {
                    std::uint16_t prev = pos_v - 1;
                    pos_v = prev;
                    key_v = CO::keys(leaf_v, COMPACT_HEADER_U64)[prev];
                    if constexpr (!IS_BOOL)
                        val_v = static_cast<char*>(val_v) - sizeof(typename CO::VST);
                    return *this;
                }
            } else {
                auto r = BO::bm(leaf_v, BITMAP_LEAF_HEADER_U64).prev_bit_before(
                    static_cast<std::uint8_t>(bit_v));
                if (r.found) {
                    bit_v = r.idx;
                    key_v = (key_v & ~UK(0xFF)) | UK(r.idx);
                    if constexpr (IS_BOOL) {
                        pos_v = static_cast<std::uint16_t>(r.idx);
                    } else {
                        pos_v--;
                        val_v = static_cast<char*>(val_v) - sizeof(typename CO::VST);
                    }
                    return *this;
                }
            }
            auto e = impl_t::walk_from_leaf(leaf_v, dir_t::BWD);
            leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
            key_v = e.key; val_v = e.val;
            return *this;
        }
        iterator operator--(int) { auto t = *this; --(*this); return t; }

        bool operator==(const iterator& o) const noexcept {
            return leaf_v == o.leaf_v && pos_v == o.pos_v;
        }
        bool operator!=(const iterator& o) const noexcept {
            return !(*this == o);
        }
    };

    using const_iterator         = iterator;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = reverse_iterator;

    // ==================================================================
    // Construction / Destruction
    // ==================================================================

    kntrie() = default;
    ~kntrie() = default;

    kntrie(kntrie&& o) noexcept : impl_(std::move(o.impl_)) {}

    kntrie& operator=(kntrie&& o) noexcept {
        impl_ = std::move(o.impl_);
        return *this;
    }

    kntrie(const kntrie& o) {
        for (auto [k, v] : o)
            impl_.insert(KO::to_stored(k), v);
    }

    kntrie& operator=(const kntrie& o) {
        if (this != &o) {
            impl_.clear();
            for (auto [k, v] : o)
                impl_.insert(KO::to_stored(k), v);
        }
        return *this;
    }

    void swap(kntrie& o) noexcept { impl_.swap(o.impl_); }
    friend void swap(kntrie& a, kntrie& b) noexcept { a.swap(b); }

    // ==================================================================
    // Capacity
    // ==================================================================

    [[nodiscard]] bool      empty() const noexcept { return impl_.empty(); }
    [[nodiscard]] size_type size()  const noexcept { return impl_.size(); }
    [[nodiscard]] size_type max_size() const noexcept {
        return std::allocator_traits<ALLOC>::max_size(impl_.get_allocator());
    }
    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return impl_.get_allocator();
    }

    // ==================================================================
    // Element access
    // ==================================================================

    mapped_ref at(const KEY& key) {
        auto r = impl_.find_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] throw std::out_of_range("kntrie::at");
        if constexpr (IS_BOOL) {
            auto* base = static_cast<std::uint64_t*>(r.val);
            return kntrie_detail::bool_ref{base + r.pos / kntrie_detail::U64_BITS,
                                            static_cast<std::uint8_t>(r.pos % kntrie_detail::U64_BITS)};
        } else {
            return deref_val(r.val);
        }
    }
    const_mapped_ref at(const KEY& key) const {
        auto r = impl_.find_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] throw std::out_of_range("kntrie::at");
        if constexpr (IS_BOOL) {
            auto* base = static_cast<std::uint64_t*>(r.val);
            return (base[r.pos / kntrie_detail::U64_BITS] >> (r.pos % kntrie_detail::U64_BITS)) & 1;
        } else {
            return deref_val_const(r.val);
        }
    }

    mapped_ref operator[](const KEY& key) {
        auto r = impl_.insert_with_pos(KO::to_stored(key), VALUE{});
        auto e = impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(key));
        if constexpr (IS_BOOL) {
            auto* base = static_cast<std::uint64_t*>(e.val);
            return kntrie_detail::bool_ref{base + e.pos / kntrie_detail::U64_BITS,
                                            static_cast<std::uint8_t>(e.pos % kntrie_detail::U64_BITS)};
        } else {
            return deref_val(e.val);
        }
    }

    // ==================================================================
    // Modifiers — insert family
    // ==================================================================

    std::pair<iterator, bool> insert(const value_type& kv) {
        auto r = impl_.insert_with_pos(KO::to_stored(kv.first), kv.second);
        return {iterator(impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(kv.first))), r.inserted};
    }

    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        auto r = impl_.insert_with_pos(KO::to_stored(key), value);
        return {iterator(impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(key))), r.inserted};
    }

    std::pair<iterator, bool> insert(value_type&& kv) {
        auto r = impl_.insert_with_pos(KO::to_stored(kv.first), std::move(kv.second));
        return {iterator(impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(kv.first))), r.inserted};
    }

    iterator insert(const_iterator, const value_type& kv) {
        return insert(kv).first;
    }

    template<typename InputIt>
    requires (!std::is_integral_v<InputIt>)
    void insert(InputIt first, InputIt last) {
        for (; first != last; ++first) insert(*first);
    }

    void insert(std::initializer_list<value_type> il) {
        for (auto& kv : il) insert(kv);
    }

    std::pair<iterator, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        auto r = impl_.upsert_with_pos(KO::to_stored(key), value);
        return {iterator(impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(key))), r.inserted};
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(std::move(kv));
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const KEY& key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        auto r = impl_.insert_with_pos(KO::to_stored(key), std::move(v));
        return {iterator(impl_.entry_from_pos(r.leaf, r.pos, KO::to_stored(key))), r.inserted};
    }

    // ==================================================================
    // Modifiers — erase
    // ==================================================================

    size_type erase(const KEY& key) {
        return impl_.erase(KO::to_stored(key)) ? 1 : 0;
    }

    iterator erase(iterator pos) {
        auto e = impl_.erase_with_next(pos.key_v);
        if (!e.found) return end();
        return iterator(e);
    }

    iterator erase(iterator first, iterator last) {
        while (first != last) first = erase(first);
        return last;
    }

    void clear() noexcept { impl_.clear(); }

    // ==================================================================
    // Lookup
    // ==================================================================

    bool contains(const KEY& key) const noexcept {
        return impl_.contains(KO::to_stored(key));
    }
    size_type count(const KEY& key) const noexcept {
        return contains(key) ? 1 : 0;
    }

    iterator find(const KEY& key) {
        auto r = impl_.find_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] return end();
        return iterator(r);
    }
    const_iterator find(const KEY& key) const {
        auto r = impl_.find_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] return end();
        return const_iterator(r);
    }

    iterator lower_bound(const KEY& key) {
        auto r = impl_.lower_bound_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] return end();
        return iterator(r);
    }

    iterator upper_bound(const KEY& key) {
        auto r = impl_.upper_bound_entry(KO::to_stored(key));
        if (!r.found) [[unlikely]] return end();
        return iterator(r);
    }

    std::pair<iterator, iterator> equal_range(const KEY& key) {
        return {lower_bound(key), upper_bound(key)};
    }

    // ==================================================================
    // Observers
    // ==================================================================

    static constexpr UK SIGN_BIT = std::is_signed_v<KEY>
        ? (UK(1) << (sizeof(KEY) * CHAR_BIT - 1)) : UK(0);

    struct key_compare {
        bool operator()(const KEY& a, const KEY& b) const noexcept {
            return (static_cast<UK>(a) ^ SIGN_BIT) < (static_cast<UK>(b) ^ SIGN_BIT);
        }
    };
    key_compare key_comp() const noexcept { return {}; }

    struct value_compare {
        bool operator()(const value_type& a, const value_type& b) const noexcept {
            return key_compare{}(a.first, b.first);
        }
    };
    value_compare value_comp() const noexcept { return {}; }

    // ==================================================================
    // Iterators
    // ==================================================================

    iterator begin() noexcept {
        return iterator(impl_.edge_entry(kntrie_detail::dir_t::FWD));
    }
    iterator end() noexcept {
        return iterator{&impl_};
    }
    const_iterator begin() const noexcept {
        return const_iterator(const_cast<impl_t&>(impl_).edge_entry(
            kntrie_detail::dir_t::FWD));
    }
    const_iterator end() const noexcept {
        return const_iterator{const_cast<impl_t*>(&impl_)};
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    reverse_iterator rbegin()  noexcept { return reverse_iterator(end()); }
    reverse_iterator rend()    noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crend()   const noexcept { return const_reverse_iterator(begin()); }

    // ==================================================================
    // Debug / Stats
    // ==================================================================

    std::size_t memory_usage() const noexcept { return impl_.memory_usage(); }
    auto   debug_stats() const noexcept  { return impl_.debug_stats(); }
    const std::uint64_t* debug_root() const noexcept { return impl_.debug_root(); }

    const impl_t& impl() const noexcept { return impl_; }

private:
    impl_t impl_;
};

} // namespace gteitelbaum

#endif // KNTRIE_HPP
