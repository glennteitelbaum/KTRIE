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
    using VT     = kntrie_detail::value_traits<VALUE, ALLOC>;

    // val_v in iter_entry_t points at a slot: inline types store VALUE
    // directly, non-inline store VALUE*. Dereference accordingly.
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
    using KO     = kntrie_detail::key_ops<UK>;
    using IK     = typename KO::IK;

    static constexpr int KEY_BITS = sizeof(KEY) * CHAR_BIT;
    static constexpr int IK_BITS  = KO::IK_BITS;
    static constexpr int U64_BITS = 64;
    static constexpr bool IS_BOOL = std::is_same_v<VALUE, bool>;

    static constexpr UK  SIGN_BIT = std::is_signed_v<KEY>
        ? (UK(1) << (KEY_BITS - 1)) : UK(0);

    static UK to_unsigned(KEY k) noexcept {
        return static_cast<UK>(k) ^ SIGN_BIT;
    }
    static KEY from_unsigned(UK u) noexcept {
        return static_cast<KEY>(u ^ SIGN_BIT);
    }

    // Convert left-aligned ik from iter_entry_t to KEY
    static KEY ik_to_key(uint64_t ik) noexcept {
        IK internal = static_cast<IK>(ik >> (U64_BITS - IK_BITS));
        return from_unsigned(KO::to_key(internal));
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
    // Caches key and value pointer from fn ptrs. operator*() returns
    // cached pair directly — no header reads, no type dispatch.
    // Invalidated by any modification to the trie (more restrictive
    // than std::map or std::unordered_map — structural reorganization
    // can affect nodes in distant subtrees).
    // No impl pointer — parent walk uses node parent pointers.
    // ==================================================================

    class iterator {
        friend class kntrie;
        uint64_t* leaf_v = nullptr;
        uint16_t  pos_v  = 0;       // slot
        uint16_t  bit_v  = 0;       // byte value (bitmap), unused (compact)
        uint64_t  ik_v   = 0;
        void*     val_v  = nullptr;

        iterator(kntrie_detail::iter_entry_t e)
            : leaf_v(e.leaf), pos_v(e.pos), bit_v(e.bit),
              ik_v(e.ik), val_v(e.val) {}

        // For insert returning iterator (leaf+pos known but no cached entry)
        iterator(uint64_t* leaf, uint16_t pos, uint16_t bit, uint64_t ik, void* val)
            : leaf_v(leaf), pos_v(pos), bit_v(bit), ik_v(ik), val_v(val) {}

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
            KEY k = ik_to_key(ik_v);
            if constexpr (IS_BOOL) {
                constexpr unsigned BITS_PER_WORD = 64;
                auto* base = static_cast<uint64_t*>(val_v);
                return {k, kntrie_detail::bool_ref{
                    base + pos_v / BITS_PER_WORD,
                    static_cast<uint8_t>(pos_v % BITS_PER_WORD)}};
            } else {
                return {k, deref_val(val_v)};
            }
        }

        arrow_proxy operator->() const noexcept {
            return {**this};
        }

        // One path: adv_fn → if miss, walk parent chain → edge_fn
        iterator& operator++() {
            using namespace kntrie_detail;
            auto fn = get_find_adv(leaf_v);
            auto e = fn(leaf_v, pos_v, bit_v, ik_v, val_v, dir_t::FWD);
            if (e.found) {
                leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
                ik_v = e.ik; val_v = e.val;
                return *this;
            }
            auto e2 = impl_t::walk_from_leaf(leaf_v, dir_t::FWD);
            leaf_v = e2.leaf; pos_v = e2.pos; bit_v = e2.bit;
            ik_v = e2.ik; val_v = e2.val;
            return *this;
        }
        iterator operator++(int) { auto t = *this; ++(*this); return t; }

        iterator& operator--() {
            using namespace kntrie_detail;
            if (!leaf_v) {
                // End sentinel: val_v holds impl* for reaching last element
                auto* impl = static_cast<impl_t*>(val_v);
                if (!impl) return *this;
                auto e = impl->edge_entry(dir_t::BWD);
                if (!e.leaf) return *this;  // empty container
                leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
                ik_v = e.ik; val_v = e.val;
                return *this;
            }
            auto fn = get_find_adv(leaf_v);
            auto e = fn(leaf_v, pos_v, bit_v, ik_v, val_v, dir_t::BWD);
            if (e.found) {
                leaf_v = e.leaf; pos_v = e.pos; bit_v = e.bit;
                ik_v = e.ik; val_v = e.val;
                return *this;
            }
            auto e2 = impl_t::walk_from_leaf(leaf_v, dir_t::BWD);
            leaf_v = e2.leaf; pos_v = e2.pos; bit_v = e2.bit;
            ik_v = e2.ik; val_v = e2.val;
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

    mapped_ref at(const KEY& key) {
        auto r = impl_.find_entry(to_unsigned(key));
        if (!r.found) throw std::out_of_range("kntrie::at");
        if constexpr (IS_BOOL) {
            constexpr unsigned BPW = 64;
            auto* base = static_cast<uint64_t*>(r.val);
            return kntrie_detail::bool_ref{base + r.pos / BPW,
                                            static_cast<uint8_t>(r.pos % BPW)};
        } else {
            return deref_val(r.val);
        }
    }
    const_mapped_ref at(const KEY& key) const {
        auto r = impl_.find_entry(to_unsigned(key));
        if (!r.found) throw std::out_of_range("kntrie::at");
        if constexpr (IS_BOOL) {
            constexpr unsigned BPW = 64;
            auto* base = static_cast<uint64_t*>(r.val);
            return (base[r.pos / BPW] >> (r.pos % BPW)) & 1;
        } else {
            return deref_val_const(r.val);
        }
    }

    mapped_ref operator[](const KEY& key) {
        auto r = impl_.insert_with_pos(to_unsigned(key), VALUE{});
        auto e = impl_.find_entry(to_unsigned(key));
        if constexpr (IS_BOOL) {
            constexpr unsigned BPW = 64;
            auto* base = static_cast<uint64_t*>(e.val);
            return kntrie_detail::bool_ref{base + e.pos / BPW,
                                            static_cast<uint8_t>(e.pos % BPW)};
        } else {
            return deref_val(e.val);
        }
    }

    // ==================================================================
    // Modifiers — insert family
    // ==================================================================

    std::pair<iterator, bool> insert(const value_type& kv) {
        UK uk = to_unsigned(kv.first);
        auto r = impl_.insert_with_pos(uk, kv.second);
        auto e = impl_.find_entry(uk);
        return {iterator(e), r.inserted};
    }

    std::pair<iterator, bool> insert(const KEY& key, const VALUE& value) {
        UK uk = to_unsigned(key);
        auto r = impl_.insert_with_pos(uk, value);
        auto e = impl_.find_entry(uk);
        return {iterator(e), r.inserted};
    }

    std::pair<iterator, bool> insert(value_type&& kv) {
        UK uk = to_unsigned(kv.first);
        auto r = impl_.insert_with_pos(uk, std::move(kv.second));
        auto e = impl_.find_entry(uk);
        return {iterator(e), r.inserted};
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
        UK uk = to_unsigned(key);
        auto r = impl_.upsert_with_pos(uk, value);
        auto e = impl_.find_entry(uk);
        return {iterator(e), r.inserted};
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        value_type kv(std::forward<Args>(args)...);
        return insert(std::move(kv));
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const KEY& key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        UK uk = to_unsigned(key);
        auto r = impl_.insert_with_pos(uk, std::move(v));
        auto e = impl_.find_entry(uk);
        return {iterator(e), r.inserted};
    }

    // ==================================================================
    // Modifiers — erase
    // ==================================================================

    size_type erase(const KEY& key) {
        return impl_.erase(to_unsigned(key)) ? 1 : 0;
    }

    iterator erase(iterator pos) {
        auto e = impl_.erase_with_next(pos.ik_v);
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
        return impl_.contains(to_unsigned(key));
    }
    size_type count(const KEY& key) const noexcept {
        return contains(key) ? 1 : 0;
    }

    iterator find(const KEY& key) {
        auto r = impl_.find_entry(to_unsigned(key));
        if (!r.found) return end();
        return iterator(r);
    }
    const_iterator find(const KEY& key) const {
        auto r = impl_.find_entry(to_unsigned(key));
        if (!r.found) return end();
        return const_iterator(r);
    }

    iterator lower_bound(const KEY& key) {
        auto r = impl_.lower_bound_entry(to_unsigned(key));
        if (!r.found) return end();
        return iterator(r);
    }

    iterator upper_bound(const KEY& key) {
        auto r = impl_.upper_bound_entry(to_unsigned(key));
        if (!r.found) return end();
        return iterator(r);
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
