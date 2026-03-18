#ifndef KNTRIE_HPP
#define KNTRIE_HPP

#include "kntrie_impl.hpp"
#include <stdexcept>
#include <iterator>
#include <vector>

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
    // Iterator — snapshot-based, bidirectional
    //
    // IMPORTANT: Iterators are snapshots. operator*() returns a copy of
    // the key/value pair, not a reference into the trie. Modifications
    // via insert_or_assign do not update existing iterators. Each ++/--
    // re-descends from the root. Concurrent reads are safe; concurrent
    // read+write or write+write requires external synchronization.
    // ==================================================================

    class const_iterator {
        friend class kntrie;
        const impl_t* parent_v = nullptr;
        UK    ukey_v{};
        VALUE value_v{};
        bool  is_valid_v = false;

        const_iterator(const impl_t* p, UK uk, VALUE v, bool valid)
            : parent_v(p), ukey_v(uk), value_v(v), is_valid_v(valid) {}

        static const_iterator from_result(const impl_t* p,
                                           const typename impl_t::iter_result_t& r) {
            return const_iterator(p, r.key, r.value, r.found);
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const KEY, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = void;
        using reference         = std::pair<const KEY, VALUE>;

        const_iterator() = default;

        KEY          key()   const noexcept { return from_unsigned(ukey_v); }
        const VALUE& value() const noexcept { return value_v; }

        std::pair<const KEY, VALUE> operator*() const noexcept {
            return {from_unsigned(ukey_v), value_v};
        }

        const_iterator& operator++() {
            auto r = parent_v->iter_next(ukey_v);
            ukey_v  = r.key;
            value_v = r.value;
            is_valid_v = r.found;
            return *this;
        }

        const_iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        const_iterator& operator--() {
            auto r = is_valid_v ? parent_v->iter_prev(ukey_v)
                                : parent_v->iter_last();
            ukey_v     = r.key;
            value_v    = r.value;
            is_valid_v = r.found;
            return *this;
        }

        const_iterator operator--(int) {
            auto tmp = *this;
            --(*this);
            return tmp;
        }

        bool operator==(const const_iterator& o) const noexcept {
            if (!is_valid_v && !o.is_valid_v) return true;
            if (is_valid_v != o.is_valid_v) return false;
            return ukey_v == o.ukey_v;
        }

        bool operator!=(const const_iterator& o) const noexcept {
            return !(*this == o);
        }
    };

    using iterator               = const_iterator;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator       = const_reverse_iterator;

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
    // Size
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
    // Modifiers
    // ==================================================================

    std::pair<iterator, bool> insert(const value_type& kv) {
        UK uk = to_unsigned(kv.first);
        auto r = impl_.insert_ex(uk, kv.second);
        if (r.inserted) return {const_iterator(&impl_, uk, kv.second, true), true};
        return {const_iterator(&impl_, uk, *r.existing, true), false};
    }
    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        UK uk = to_unsigned(key);
        auto r = impl_.insert_ex(uk, value);
        if (r.inserted) return {const_iterator(&impl_, uk, value, true), true};
        return {const_iterator(&impl_, uk, *r.existing, true), false};
    }
    std::pair<bool, bool> insert_or_assign(const KEY& key, const VALUE& value) {
        return impl_.insert_or_assign(to_unsigned(key), value);
    }
    std::pair<bool, bool> assign(const KEY& key, const VALUE& value) {
        return impl_.assign(to_unsigned(key), value);
    }

    // Single-walk read-modify-write with dup propagation.
    // fn is void(VALUE&) — modifies the value in place.
    // Returns true if key existed and was modified.
    template<typename F>
    bool modify(const KEY& key, F&& fn) {
        return impl_.modify_existing(to_unsigned(key), [&fn](auto& slot) {
            fn(reinterpret_cast<VALUE&>(slot));
        });
    }

    // With default: if key exists, apply fn(value&), return true.
    // If missing, insert default_val as-is, return false.
    // Single walk — fn is never called on the default.
    template<typename F>
    bool modify(const KEY& key, F&& fn, const VALUE& default_val) {
        return impl_.modify_or_insert(to_unsigned(key), [&fn](auto& slot) {
            fn(reinterpret_cast<VALUE&>(slot));
        }, default_val);
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(kv);
    }

    // try_emplace: INSERT=true, ASSIGN=false — same as insert
    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const KEY& key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        UK uk = to_unsigned(key);
        auto r = impl_.insert_ex(uk, v);
        if (r.inserted) return {const_iterator(&impl_, uk, v, true), true};
        return {const_iterator(&impl_, uk, *r.existing, true), false};
    }

    iterator insert(const_iterator, const value_type& kv) { return insert(kv).first; }

    template<typename InputIt>
    requires (!std::is_integral_v<InputIt>)
    void insert(InputIt first, InputIt last) {
        for (; first != last; ++first) insert(*first);
    }

    void clear() noexcept { impl_.clear(); }
    size_type erase(const KEY& key) { return impl_.erase(to_unsigned(key)) ? 1 : 0; }

    iterator erase(const_iterator pos) {
        KEY k = pos.key();
        auto next = pos;
        ++next;
        impl_.erase(to_unsigned(k));
        return next;
    }

    iterator erase(const_iterator first, const_iterator last) {
        while (first != last) {
            KEY k = first.key();
            ++first;
            impl_.erase(to_unsigned(k));
        }
        return last;
    }

    // Single-walk conditional erase: find key, test fn(const VALUE&),
    // erase only if predicate returns true.
    // Returns true if erased, false if not found or predicate failed.
    template<typename F>
    bool erase_when(const KEY& key, F&& fn) {
        return impl_.erase_when(to_unsigned(key), [&fn](const auto& slot) {
            return fn(reinterpret_cast<const VALUE&>(slot));
        });
    }

    // ==================================================================
    // Lookup
    // ==================================================================

    bool contains(const KEY& key) const noexcept { return impl_.contains(to_unsigned(key)); }
    size_type count(const KEY& key) const noexcept { return contains(key) ? 1 : 0; }

    // at() is intentionally absent. The kntrie stores values in compact nodes
    // with dup-slot padding — multiple physical copies of each value exist for
    // in-place mutation. Returning VALUE& would allow writes that update one
    // copy but not the others, corrupting the dup invariant. Use find()
    // (returns snapshot iterator) or operator[] for writes.

    // ==================================================================
    // value_proxy — returned by operator[], routes writes through insert
    // ==================================================================

    class value_proxy {
        friend class kntrie;
        kntrie& trie_r;
        UK      key_v;
        value_proxy(kntrie& t, UK k) : trie_r(t), key_v(k) {}
    public:
        value_proxy& operator=(const VALUE& v) {
            trie_r.impl_.insert_or_assign(key_v, v);
            return *this;
        }
        operator VALUE() const {
            const VALUE* p = trie_r.impl_.find_value(key_v);
            if (p) return *p;
            trie_r.impl_.insert(key_v, VALUE{});
            return *trie_r.impl_.find_value(key_v);
        }
    };

    value_proxy operator[](const KEY& key) {
        return value_proxy{*this, to_unsigned(key)};
    }

    // ==================================================================
    // Iterators
    // ==================================================================

    const_iterator begin() const noexcept {
        return const_iterator::from_result(&impl_, impl_.iter_first());
    }
    const_iterator end() const noexcept {
        const_iterator it;
        it.parent_v = &impl_;
        return it;
    }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend()   const noexcept { return end(); }

    const_reverse_iterator rbegin()  const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()    const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    const_reverse_iterator crend()   const noexcept { return rend(); }

    const_iterator find(const KEY& key) const noexcept {
        UK uk = to_unsigned(key);
        const VALUE* v = impl_.find_value(uk);
        if (!v) return end();
        return const_iterator(&impl_, uk, *v, true);
    }

    const_iterator lower_bound(const KEY& k) const noexcept {
        UK uk = to_unsigned(k);
        const VALUE* v = impl_.find_value(uk);
        if (v) return const_iterator(&impl_, uk, *v, true);
        auto r = impl_.iter_next(uk);
        return const_iterator::from_result(&impl_, r);
    }

    const_iterator upper_bound(const KEY& k) const noexcept {
        auto r = impl_.iter_next(to_unsigned(k));
        return const_iterator::from_result(&impl_, r);
    }

    std::pair<const_iterator, const_iterator> equal_range(const KEY& k) const noexcept {
        return {lower_bound(k), upper_bound(k)};
    }

    // ==================================================================
    // Debug / Stats
    // ==================================================================

    size_t memory_usage() const noexcept { return impl_.memory_usage(); }
    auto   debug_stats() const noexcept  { return impl_.debug_stats(); }
    auto   debug_root_info() const       { return impl_.debug_root_info(); }
    const uint64_t* debug_root() const noexcept { return impl_.debug_root(); }

    const impl_t& impl() const noexcept { return impl_; }

private:
    const VALUE* find_value(const KEY& key) const noexcept { return impl_.find_value(to_unsigned(key)); }

    impl_t impl_;
};

} // namespace gteitelbaum

#endif // KNTRIE_HPP
