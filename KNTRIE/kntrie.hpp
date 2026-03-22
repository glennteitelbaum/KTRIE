#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>
#include <initializer_list>

namespace gteitelbaum {

template<typename KEY, typename VALUE, typename ALLOC = std::allocator<uint64_t>>
class kntrie {
    static_assert(std::is_integral_v<KEY> && sizeof(KEY) >= 2,
                  "KEY must be integral and at least 16 bits");

    using UK     = std::make_unsigned_t<KEY>;
    using impl_t = kntrie_detail::kntrie_impl<UK, VALUE, ALLOC>;

    static constexpr int KEY_BITS = sizeof(KEY) * CHAR_BIT;
    static constexpr UK  SIGN_BIT = std::is_signed_v<KEY>
        ? (UK(1) << (KEY_BITS - 1)) : UK(0);

    static UK to_unsigned(KEY k) noexcept {
        return static_cast<UK>(k) ^ SIGN_BIT;
    }
    static KEY from_unsigned(UK u) noexcept {
        return static_cast<KEY>(u ^ SIGN_BIT);
    }

public:
    using key_type        = KEY;
    using mapped_type     = VALUE;
    using value_type      = std::pair<const KEY, VALUE>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using allocator_type  = ALLOC;

    // ==================================================================
    // Iterator — live, bidirectional
    //
    // Points into the trie structure. operator*() returns a pair with a
    // reference to the value, not a copy. Invalidated by any modification
    // to the container (same as std::unordered_map).
    // ==================================================================

    class iterator {
        friend class kntrie;
        impl_t*   impl_v = nullptr;
        uint64_t* leaf_v = nullptr;
        uint16_t  pos_v  = 0;

        iterator(impl_t* p, uint64_t* leaf, uint16_t pos)
            : impl_v(p), leaf_v(leaf), pos_v(pos) {}

        iterator(impl_t* p, kntrie_detail::leaf_pos_t lp)
            : impl_v(p), leaf_v(lp.node), pos_v(lp.pos) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::pair<const KEY, VALUE&>;

        struct arrow_proxy {
            std::pair<const KEY, VALUE&> p;
            auto* operator->() noexcept { return &p; }
        };
        using pointer = arrow_proxy;

        iterator() = default;

        reference operator*() const noexcept {
            KEY k = impl_v->key_at_pos(leaf_v, pos_v);
            VALUE& v = impl_t::value_ref_at_pos(leaf_v, pos_v);
            return {from_unsigned(to_unsigned(k)), v};
        }

        arrow_proxy operator->() const noexcept {
            return {**this};
        }

        iterator& operator++() {
            auto r = impl_v->advance_pos(leaf_v, pos_v, kntrie_detail::dir_t::FWD);
            leaf_v = r.node;
            pos_v  = r.pos;
            return *this;
        }
        iterator operator++(int) { auto t = *this; ++(*this); return t; }

        iterator& operator--() {
            if (!leaf_v) {
                auto r = impl_v->edge_pos(kntrie_detail::dir_t::BWD);
                leaf_v = r.node; pos_v = r.pos;
            } else {
                auto r = impl_v->advance_pos(leaf_v, pos_v, kntrie_detail::dir_t::BWD);
                leaf_v = r.node; pos_v = r.pos;
            }
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
            impl_.insert(to_unsigned(k), v);
    }

    kntrie& operator=(const kntrie& o) {
        if (this != &o) {
            impl_.clear();
            for (auto [k, v] : o)
                impl_.insert(to_unsigned(k), v);
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

    VALUE& at(const KEY& key) {
        auto r = impl_.find_with_pos(to_unsigned(key));
        if (!r.found) throw std::out_of_range("kntrie::at");
        return impl_t::value_ref_at_pos(r.node, r.pos);
    }
    const VALUE& at(const KEY& key) const {
        auto r = impl_.find_with_pos(to_unsigned(key));
        if (!r.found) throw std::out_of_range("kntrie::at");
        return impl_t::value_cref_at_pos(r.node, r.pos);
    }

    VALUE& operator[](const KEY& key) {
        auto r = impl_.insert_with_pos(to_unsigned(key), VALUE{});
        return impl_t::value_ref_at_pos(r.leaf, r.pos);
    }

    // ==================================================================
    // Modifiers — insert family
    // ==================================================================

    std::pair<iterator, bool> insert(const value_type& kv) {
        auto r = impl_.insert_with_pos(to_unsigned(kv.first), kv.second);
        return {iterator(&impl_, r.leaf, r.pos), r.inserted};
    }

    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        auto r = impl_.insert_with_pos(to_unsigned(key), value);
        return {iterator(&impl_, r.leaf, r.pos), r.inserted};
    }

    std::pair<iterator, bool> insert(value_type&& kv) {
        auto r = impl_.insert_with_pos(to_unsigned(kv.first), std::move(kv.second));
        return {iterator(&impl_, r.leaf, r.pos), r.inserted};
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
        auto r = impl_.upsert_with_pos(to_unsigned(key), value);
        return {iterator(&impl_, r.leaf, r.pos), r.inserted};
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(std::move(kv));
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const KEY& key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        auto r = impl_.insert_with_pos(to_unsigned(key), std::move(v));
        return {iterator(&impl_, r.leaf, r.pos), r.inserted};
    }

    // ==================================================================
    // Modifiers — erase
    // ==================================================================

    size_type erase(const KEY& key) {
        return impl_.erase(to_unsigned(key)) ? 1 : 0;
    }

    iterator erase(iterator pos) {
        auto next = pos; ++next;
        impl_.erase_at(pos.leaf_v, pos.pos_v);
        return next;
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
        return impl_.contains(to_unsigned(key));
    }
    size_type count(const KEY& key) const noexcept {
        return contains(key) ? 1 : 0;
    }

    iterator find(const KEY& key) {
        auto r = impl_.find_with_pos(to_unsigned(key));
        if (!r.found) return end();
        return iterator(&impl_, r.node, r.pos);
    }
    const_iterator find(const KEY& key) const {
        auto r = impl_.find_with_pos(to_unsigned(key));
        if (!r.found) return end();
        return const_iterator(const_cast<impl_t*>(&impl_), r.node, r.pos);
    }

    iterator lower_bound(const KEY& key) {
        auto r = impl_.lower_bound_pos(to_unsigned(key));
        if (!r.found) return end();
        return iterator(&impl_, r.node, r.pos);
    }

    iterator upper_bound(const KEY& key) {
        auto r = impl_.upper_bound_pos(to_unsigned(key));
        if (!r.found) return end();
        return iterator(&impl_, r.node, r.pos);
    }

    std::pair<iterator, iterator> equal_range(const KEY& key) {
        return {lower_bound(key), upper_bound(key)};
    }

    // ==================================================================
    // Observers
    // ==================================================================

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
        return iterator(&impl_, impl_.edge_pos(kntrie_detail::dir_t::FWD));
    }
    iterator end() noexcept {
        return iterator(&impl_, nullptr, 0);
    }
    const_iterator begin() const noexcept {
        return const_iterator(const_cast<impl_t*>(&impl_),
                              const_cast<impl_t&>(impl_).edge_pos(kntrie_detail::dir_t::FWD));
    }
    const_iterator end() const noexcept {
        return const_iterator(const_cast<impl_t*>(&impl_), nullptr, 0);
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

    size_t memory_usage() const noexcept { return impl_.memory_usage(); }
    auto   debug_stats() const noexcept  { return impl_.debug_stats(); }
    auto   debug_root_info() const       { return impl_.debug_root_info(); }
    const uint64_t* debug_root() const noexcept { return impl_.debug_root(); }

    const impl_t& impl() const noexcept { return impl_; }

private:
    impl_t impl_;
};

} // namespace gteitelbaum

#endif // KNTRIE_HPP
