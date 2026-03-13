#pragma once

#include "kstrie_impl.hpp"
#include <iterator>
#include <optional>
#include <vector>

namespace gteitelbaum {

// ============================================================================
// kstrie -- user-facing trie class
//
// Inherits engine (find/insert/erase) from kstrie_impl.
// Adds bidirectional iterator, ordered traversal, prefix queries.
// ============================================================================

template <typename VALUE,
          typename CHARMAP = identity_char_map,
          typename ALLOC   = std::allocator<uint64_t>>
class kstrie : public kstrie_impl<VALUE, CHARMAP, ALLOC> {
    using base         = kstrie_impl<VALUE, CHARMAP, ALLOC>;
    using hdr_type     = typename base::hdr_type;
    using slots_type   = typename base::slots_type;
    using bitmask_type = typename base::bitmask_type;
    using compact_type = typename base::compact_type;

public:
    using typename base::key_type;
    using typename base::mapped_type;
    using typename base::size_type;
    using typename base::allocator_type;

    // Inherit constructors
    using base::base;

    // ------------------------------------------------------------------
    // const_iterator -- bidirectional, stores VALUE copy
    // ------------------------------------------------------------------

    class const_iterator {
        const kstrie* trie_ = nullptr;
        std::string   key_;
        VALUE         value_{};
        bool          at_end_ = true;

        const_iterator(const kstrie* t, std::string k, const VALUE& v)
            : trie_(t), key_(std::move(k)), value_(v), at_end_(false) {}

        static const_iterator make_end(const kstrie* t) {
            const_iterator it;
            it.trie_   = t;
            it.at_end_ = true;
            return it;
        }

        friend class kstrie;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const std::string, VALUE>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const value_type*;

        using reference = std::pair<std::string, VALUE>;

        const_iterator() = default;

        reference operator*() const { return {key_, value_}; }

        const std::string& key() const { return key_; }
        const VALUE& value() const { return value_; }

        const_iterator& operator++() {
            auto [k, v] = trie_->iter_next(key_);
            if (v) { key_ = std::move(k); value_ = *v; at_end_ = false; }
            else   { at_end_ = true; }
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator tmp = *this;
            ++*this;
            return tmp;
        }

        const_iterator& operator--() {
            if (at_end_) {
                auto [k, v] = trie_->iter_max(trie_->get_root());
                if (v) { key_ = std::move(k); value_ = *v; at_end_ = false; }
            } else {
                auto [k, v] = trie_->iter_prev(key_);
                if (v) { key_ = std::move(k); value_ = *v; }
                else   { at_end_ = true; }
            }
            return *this;
        }

        const_iterator operator--(int) {
            const_iterator tmp = *this;
            --*this;
            return tmp;
        }

        bool operator==(const const_iterator& o) const {
            if (at_end_ && o.at_end_) return true;
            if (at_end_ || o.at_end_) return false;
            return key_ == o.key_;
        }

        bool operator!=(const const_iterator& o) const { return !(*this == o); }
    };

    using iterator               = const_iterator;
    using reverse_iterator       = std::reverse_iterator<const_iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // ------------------------------------------------------------------
    // Iterator access
    // ------------------------------------------------------------------

    const_iterator begin() const {
        auto [k, v] = iter_min(this->get_root());
        if (!v) return end();
        return {this, std::move(k), *v};
    }

    const_iterator end() const { return const_iterator::make_end(this); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() const { return reverse_iterator(end()); }
    reverse_iterator rend() const { return reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return rbegin(); }
    const_reverse_iterator crend() const { return rend(); }

    // ------------------------------------------------------------------
    // Ordered lookup
    // ------------------------------------------------------------------

    const_iterator lower_bound(std::string_view key) const {
        auto [k, v] = iter_lower_bound(key);
        if (!v) return end();
        return {this, std::move(k), *v};
    }

    const_iterator upper_bound(std::string_view key) const {
        auto [k, v] = iter_upper_bound(key);
        if (!v) return end();
        return {this, std::move(k), *v};
    }

    std::pair<const_iterator, const_iterator>
    equal_range(std::string_view key) const {
        return {lower_bound(key), upper_bound(key)};
    }

    std::pair<const_iterator, const_iterator>
    prefix(std::string_view pfx) const {
        auto [k1, v1, k2, v2] = iter_prefix_bounds(pfx);
        const_iterator first = v1 ? const_iterator{this, std::move(k1), *v1}
                                  : end();
        const_iterator last  = v2 ? const_iterator{this, std::move(k2), *v2}
                                  : end();
        return {first, last};
    }

    const_iterator find_iter(std::string_view key) const {
        const VALUE* v = this->find(key);
        if (!v) return end();
        return {this, std::string(key), *v};
    }

    // ------------------------------------------------------------------
    // Iterator-based modifiers
    // ------------------------------------------------------------------

    const_iterator erase(const_iterator pos) {
        if (pos == end()) return end();
        auto [next_key, next_val] = iter_next(pos.key_);
        std::optional<VALUE> saved;
        if (next_val) saved = *next_val;
        base::erase(pos.key_);
        if (!saved) return end();
        return {this, std::move(next_key), *saved};
    }

    // Bring base erase(string_view) into scope (hidden by overload above)
    using base::erase;

    const_iterator erase(const_iterator first, const_iterator last) {
        std::vector<std::string> keys;
        for (auto it = first; it != last; ++it)
            keys.push_back(it.key());
        for (auto& k : keys)
            base::erase(k);
        if (last == end()) return end();
        auto [k, v] = iter_lower_bound(last.key());
        if (!v) return end();
        return {this, std::move(k), *v};
    }

    template <typename... Args>
    std::pair<const_iterator, bool> emplace(std::string_view key, Args&&... args) {
        VALUE v(std::forward<Args>(args)...);
        bool inserted = this->insert(key, v);
        auto [k, vp] = iter_lower_bound(key);
        return {const_iterator{this, std::move(k), *vp}, inserted};
    }

    template <typename... Args>
    std::pair<const_iterator, bool> try_emplace(std::string_view key, Args&&... args) {
        const VALUE* existing = this->find(key);
        if (existing)
            return {const_iterator{this, std::string(key), *existing}, false};
        VALUE v(std::forward<Args>(args)...);
        this->insert(key, v);
        return {const_iterator{this, std::string(key), v}, true};
    }

    // ------------------------------------------------------------------
    // Comparison
    // ------------------------------------------------------------------

    bool operator==(const kstrie& o) const {
        if (this->size() != o.size()) return false;
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

    // ------------------------------------------------------------------
    // Unmap helper
    // ------------------------------------------------------------------

    static void append_unmapped(std::string& out, const uint8_t* data,
                                uint32_t len) {
        for (uint32_t i = 0; i < len; ++i)
            out.push_back(static_cast<char>(CHARMAP::from_index(data[i])));
    }

    // ------------------------------------------------------------------
    // iter_min / iter_max
    // ------------------------------------------------------------------

    std::pair<std::string, const VALUE*>
    iter_min(const uint64_t* node) const {
        std::string result;
        const VALUE* val = find_min_impl(node, result);
        return {std::move(result), val};
    }

    std::pair<std::string, const VALUE*>
    iter_max(const uint64_t* node) const {
        std::string result;
        const VALUE* val = find_max_impl(node, result);
        return {std::move(result), val};
    }

    const VALUE* find_min_impl(const uint64_t* node, std::string& out) const {
        if (node == this->get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(out, hdr_type::get_skip(node, h), sb);
        }

        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
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
        if (eos != this->get_sentinel()) {
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

    const VALUE* find_max_impl(const uint64_t* node, std::string& out) const {
        if (node == this->get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(out, hdr_type::get_skip(node, h), sb);
        }

        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            int last = h.count - 1;
            uint8_t klen = L[last];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[last], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[last], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), last);
        }

        // Bitmask: last child, then eos
        {
            const auto* bm = bitmask_type::get_bitmap(node, h);
            constexpr int MAX_BIT = CHARMAP::BITMAP_WORDS * 64 - 1;
            int idx = bm->find_prev_set(MAX_BIT);
            if (idx >= 0) [[likely]] {
                uint8_t byte = static_cast<uint8_t>(idx);
                out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
                int slot = bm->count_below(byte);
                return find_max_impl(
                    bitmask_type::child_by_slot(node, h, slot), out);
            }
        }

        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != this->get_sentinel())
            return find_max_impl(eos, out);

        return nullptr;
    }

    // ------------------------------------------------------------------
    // iter_next -- successor
    // ------------------------------------------------------------------

    std::pair<std::string, const VALUE*>
    iter_next(const std::string& current) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(current.data());
        uint32_t len = static_cast<uint32_t>(current.size());

        uint8_t stack_buf[256];
        auto [mapped, heap_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));

        std::string result;
        const VALUE* val = find_next_impl(this->get_root(), mapped, len,
                                          0, result);
        delete[] heap_buf;
        return {std::move(result), val};
    }

    const VALUE* find_next_impl(const uint64_t* node,
                                const uint8_t* mapped, uint32_t key_len,
                                uint32_t consumed,
                                std::string& out) const {
        if (node == this->get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(out, hdr_type::get_skip(node, h), sb);
            consumed += sb;
        }

        if (h.is_compact()) [[unlikely]] {
            auto [found, pos] = compact_type::find_pos(
                node, h, mapped + consumed, key_len - consumed);
            int next = pos + 1;
            if (next >= h.count) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[next];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[next], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[next], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), next);
        }

        // Bitmask
        if (consumed == key_len) [[unlikely]] {
            // EOS is "at" this node; successor is first bitmap child
            const auto* bm = bitmask_type::get_bitmap(node, h);
            int idx = bm->find_next_set(0);
            if (idx < 0) return nullptr;
            uint8_t byte = static_cast<uint8_t>(idx);
            out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
            return find_min_impl(bitmask_type::child_by_slot(node, h,
                                 bm->count_below(byte)), out);
        }

        {
            uint8_t byte = mapped[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);
            if (child != this->get_sentinel()) {
                size_t prefix_len = out.size();
                out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
                const VALUE* val = find_next_impl(child, mapped, key_len,
                                                  consumed, out);
                if (val) [[likely]] return val;
                out.resize(prefix_len);
            }

            const auto* bm = bitmask_type::get_bitmap(node, h);
            int next_idx = bm->find_next_set(byte + 1);
            if (next_idx < 0) [[unlikely]] return nullptr;

            uint8_t next_byte = static_cast<uint8_t>(next_idx);
            out.push_back(static_cast<char>(CHARMAP::from_index(next_byte)));
            return find_min_impl(bitmask_type::child_by_slot(node, h,
                                 bm->count_below(next_byte)), out);
        }
    }

    // ------------------------------------------------------------------
    // iter_prev -- predecessor
    // ------------------------------------------------------------------

    std::pair<std::string, const VALUE*>
    iter_prev(const std::string& current) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(current.data());
        uint32_t len = static_cast<uint32_t>(current.size());

        uint8_t stack_buf[256];
        auto [mapped, heap_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));

        std::string result;
        const VALUE* val = find_prev_impl(this->get_root(), mapped, len,
                                          0, result);
        delete[] heap_buf;
        return {std::move(result), val};
    }

    const VALUE* find_prev_impl(const uint64_t* node,
                                const uint8_t* mapped, uint32_t key_len,
                                uint32_t consumed,
                                std::string& out) const {
        if (node == this->get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(out, hdr_type::get_skip(node, h), sb);
            consumed += sb;
        }

        if (h.is_compact()) [[unlikely]] {
            auto [found, pos] = compact_type::find_pos(
                node, h, mapped + consumed, key_len - consumed);
            int prev = pos - 1;
            if (prev < 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[prev];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[prev], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[prev], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), prev);
        }

        // Bitmask
        {
            const auto* bm = bitmask_type::get_bitmap(node, h);
            if (consumed == key_len) [[unlikely]] {
                // Key exhausted at bitmask — key IS the eos. No predecessor here.
                return nullptr;
            }
            uint8_t byte = mapped[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);
            if (child != this->get_sentinel()) {
                size_t prefix_len = out.size();
                out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
                const VALUE* val = find_prev_impl(child, mapped, key_len,
                                                  consumed, out);
                if (val) [[likely]] return val;
                out.resize(prefix_len);
            }
            // Find prev set bit before byte
            int prev_idx = bm->find_prev_set(byte - 1);
            if (prev_idx >= 0) {
                uint8_t prev_byte = static_cast<uint8_t>(prev_idx);
                out.push_back(static_cast<char>(CHARMAP::from_index(prev_byte)));
                return find_max_impl(bitmask_type::child_by_slot(node, h,
                                     bm->count_below(prev_byte)), out);
            }
            // No prev child — check eos
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos != this->get_sentinel())
                return find_max_impl(eos, out);
            return nullptr;
        }
    }

    // ------------------------------------------------------------------
    // iter_lower_bound -- first key >= target
    // ------------------------------------------------------------------

    std::pair<std::string, const VALUE*>
    iter_lower_bound(std::string_view key) const {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, heap_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));

        std::string result;
        const VALUE* val = find_ge_impl(this->get_root(), mapped, len,
                                        0, result);
        delete[] heap_buf;
        return {std::move(result), val};
    }

    const VALUE* find_ge_impl(const uint64_t* node,
                              const uint8_t* mapped, uint32_t key_len,
                              uint32_t consumed,
                              std::string& out) const {
        if (node == this->get_sentinel()) [[unlikely]] return nullptr;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            const uint8_t* skip = hdr_type::get_skip(node, h);
            uint32_t remaining = key_len - consumed;
            uint32_t cmp_len = std::min(sb, remaining);

            for (uint32_t i = 0; i < cmp_len; ++i) {
                if (skip[i] < mapped[consumed + i])
                    return nullptr;
                if (skip[i] > mapped[consumed + i]) {
                    append_unmapped(out, skip, sb);
                    consumed += sb;
                    goto take_min;
                }
                out.push_back(static_cast<char>(CHARMAP::from_index(skip[i])));
            }

            if (remaining < sb) [[unlikely]] {
                for (uint32_t j = cmp_len; j < sb; ++j)
                    out.push_back(static_cast<char>(CHARMAP::from_index(skip[j])));
                consumed += sb;
                goto take_min;
            }

            consumed += sb;
        }

        if (h.is_compact()) [[unlikely]] {
            auto [found, pos] = compact_type::find_pos(
                node, h, mapped + consumed, key_len - consumed);
            if (pos >= h.count) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[pos];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[pos], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[pos], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), pos);
        }

        if (consumed == key_len) [[unlikely]]
            goto take_min;

        {
            uint8_t byte = mapped[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);

            if (child != this->get_sentinel()) {
                size_t prefix_len = out.size();
                out.push_back(static_cast<char>(CHARMAP::from_index(byte)));

                const VALUE* val = find_ge_impl(child, mapped, key_len,
                                                consumed, out);
                if (val) [[likely]] return val;
                out.resize(prefix_len);
            }

            const auto* bm = bitmask_type::get_bitmap(node, h);
            int next_idx = bm->find_next_set(byte + 1);
            if (next_idx < 0) [[unlikely]] return nullptr;

            uint8_t next_byte = static_cast<uint8_t>(next_idx);
            out.push_back(static_cast<char>(CHARMAP::from_index(next_byte)));
            return find_min_impl(bitmask_type::child_by_slot(node, h,
                                 bm->count_below(next_byte)), out);
        }

    take_min:
        if (h.is_compact()) [[unlikely]] {
            if (h.count == 0) [[unlikely]] return nullptr;
            const uint8_t*  L = compact_type::lengths(node, h);
            const uint8_t*  F = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B = compact_type::keysuffix(node, h);
            uint8_t klen = L[0];
            if (klen > 0) [[likely]] {
                append_unmapped(out, &F[0], 1);
                if (klen > 1) [[likely]] append_unmapped(out, B + O[0], klen - 1);
            }
            return slots_type::load_value(h.get_compact_slots(node), 0);
        }
        // Bitmask take_min: eos first, then first child
        {
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos != this->get_sentinel())
                return find_min_impl(eos, out);
            const auto* bm = bitmask_type::get_bitmap(node, h);
            int idx = bm->find_next_set(0);
            if (idx < 0) [[unlikely]] return nullptr;
            uint8_t byte = static_cast<uint8_t>(idx);
            out.push_back(static_cast<char>(CHARMAP::from_index(byte)));
            return find_min_impl(bitmask_type::child_by_slot(node, h,
                                 bm->count_below(byte)), out);
        }
    }

    // ------------------------------------------------------------------
    // iter_upper_bound -- first key > target
    // ------------------------------------------------------------------

    std::pair<std::string, const VALUE*>
    iter_upper_bound(std::string_view key) const {
        auto [k, v] = iter_lower_bound(key);
        if (!v) return {{}, nullptr};
        if (k == key)
            return iter_next(k);
        return {std::move(k), v};
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
        auto [mapped, heap_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));

        struct right_turn {
            const uint64_t* bm_node;
            hdr_type        h;
            int             next_idx;
            std::string     prefix;
        };

        std::string path;
        right_turn best_rt{};
        bool have_rt = false;

        const uint64_t* node = this->get_root();
        uint32_t consumed = 0;

        while (consumed < len) {
            if (node == this->get_sentinel()) [[unlikely]] goto not_found;
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
                if (child == this->get_sentinel()) [[unlikely]] goto not_found;

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
                delete[] heap_buf;
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

            delete[] heap_buf;
            return {std::move(k1), v1, std::move(k2), v2};
        }

    not_found:
        delete[] heap_buf;
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
        const ks_offset_type* O = compact_type::offsets(node, h);
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
        const ks_offset_type* O = compact_type::offsets(node, h);
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
            const ks_offset_type* O = compact_type::offsets(node, h);
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
        if (eos != this->get_sentinel())
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
