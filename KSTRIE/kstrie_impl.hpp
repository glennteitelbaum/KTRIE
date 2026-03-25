#ifndef KSTRIE_IMPL_HPP
#define KSTRIE_IMPL_HPP

#include "kstrie_bitmask.hpp"
#include "kstrie_compact.hpp"
#include "kstrie_support.hpp"
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace gteitelbaum::kstrie_detail {

// ============================================================================
// kstrie_impl -- engine trie class
// ============================================================================

template <typename VALUE,
          typename CHARMAP = identity_char_map,
          typename ALLOC   = std::allocator<uint64_t>>
class kstrie_impl {
public:
    using key_type       = std::string;
    using mapped_type    = VALUE;
    using size_type      = std::size_t;
    using allocator_type = ALLOC;
    using char_map_type  = CHARMAP;

    using hdr_type     = node_header<VALUE, CHARMAP, ALLOC>;
    using mem_type     = kstrie_memory<ALLOC>;
    using slots_type   = kstrie_slots<VALUE>;
    using skip_type    = kstrie_skip<VALUE, CHARMAP, ALLOC>;
    using bitmask_type = kstrie_bitmask<VALUE, CHARMAP, ALLOC>;
    using compact_type = kstrie_compact<VALUE, CHARMAP, ALLOC>;

    // Key length is structurally bounded: each trie level consumes one byte,
    // COMPACT_KEYSUFFIX_LIMIT bounds leaf storage, and L[] is uint8_t.
    // Memory exhaustion prevents keys anywhere near uint32_t::max.
    // string_view::size() → uint32_t casts throughout are safe by this invariant.
    static_assert(COMPACT_KEYSUFFIX_LIMIT <= std::numeric_limits<uint32_t>::max(),
                  "keysuffix limit exceeds uint32_t key length representation");

private:
    uint64_t* root_v = compact_type::sentinel();
    size_type size_v{};
    mem_type  mem_v{};

    void init_empty_root() {
        root_v = compact_type::sentinel();
    }

    // Set root and mark parent_byte = ROOT_PARENT_BYTE.
    void set_root(uint64_t* node) {
        root_v = node;
        if (!node || node == compact_type::sentinel()) return;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_compact()) {
            compact_type::set_parent(node, nullptr);
            compact_type::set_parent_byte(node, h, ROOT_PARENT_BYTE);
        } else {
            bitmask_type::set_parent(node, nullptr);
            bitmask_type::set_parent_byte(node, ROOT_PARENT_BYTE);
        }
    }

    void destroy_tree(uint64_t* node) {
        if (!node || node == compact_type::sentinel()) return;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_compact()) {
            auto* vb = h.get_compact_slots(node);
            slots_type::destroy_values(vb, 0, h.count, mem_v.alloc_v);
        } else {
            // bitmask: recurse all children + eos
            uint64_t* cs_base = bitmask_type::child_slots(node);
            for (uint16_t i = 0; i < h.count; ++i)
                destroy_tree(slots_type::load_child(cs_base, i));
            destroy_tree(bitmask_type::eos_child(node, h));
        }
        mem_v.free_node(node);
    }

    size_type memory_usage_impl(const uint64_t* node) const noexcept {
        if (!node || node == compact_type::sentinel()) return 0;
        hdr_type h = hdr_type::from_node(node);
        size_type total = h.alloc_u64 * U64_BYTES;
        if (h.is_bitmap()) {
            const uint64_t* cs = bitmask_type::child_slots(node);
            for (uint16_t i = 0; i < h.count; ++i)
                total += memory_usage_impl(slots_type::load_child(cs, i));
            total += memory_usage_impl(bitmask_type::eos_child(node, h));
        }
        // compact: leaf-only, no recursion
        return total;
    }

    // ------------------------------------------------------------------
    // count_subtree — recursive entry count
    // ------------------------------------------------------------------

    size_t count_subtree(const uint64_t* node) const {
        if (!node || node == compact_type::sentinel()) return 0;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_compact()) return h.count;
        size_t n = 0;
        const uint64_t* cs = bitmask_type::child_slots(node);
        for (uint16_t i = 0; i < h.count; ++i)
            n += count_subtree(slots_type::load_child(cs, i));
        n += count_subtree(bitmask_type::eos_child(node, h));
        return n;
    }

    // ------------------------------------------------------------------
    // clone_tree_into — allocates through dest mem
    // ------------------------------------------------------------------

    static uint64_t* clone_tree_into(const uint64_t* node, mem_type& dest) {
        if (!node || node == compact_type::sentinel())
            return const_cast<uint64_t*>(node);
        hdr_type h = hdr_type::from_node(node);
        size_t nu = h.alloc_u64;
        uint64_t* copy = dest.alloc_node(nu);
        std::memcpy(copy, node, nu * U64_BYTES);
        if (h.is_compact()) {
            auto* dsb = hdr_type::from_node(copy).get_compact_slots(copy);
            const auto* ssb = h.get_compact_slots(node);
            for (uint16_t i = 0; i < h.count; ++i) {
                const VALUE* vp = slots_type::load_value(ssb, i);
                dsb[i] = {};
                slots_type::store_value(dsb, i, *vp, dest.alloc_v);
            }
        } else {
            uint64_t* new_cs = bitmask_type::child_slots(copy);
            const uint64_t* old_cs = bitmask_type::child_slots(node);
            for (uint16_t i = 0; i < h.count; ++i) {
                uint64_t* child = slots_type::load_child(old_cs, i);
                slots_type::store_child(new_cs, i,
                    clone_tree_into(child, dest));
            }
            hdr_type& ch = hdr_type::from_node(copy);
            uint64_t* old_eos = bitmask_type::eos_child(node, h);
            bitmask_type::set_eos_child(copy, ch,
                clone_tree_into(old_eos, dest));
            // Re-link all children to point to the new copy as parent
            bitmask_type::link_all_children(copy);
        }
        return copy;
    }

    // ------------------------------------------------------------------
    // unwind_child — shared by prefix_erase and prefix_split
    // ------------------------------------------------------------------

    prefix_erase_result unwind_child(
            uint64_t* node, hdr_type h, uint8_t byte,
            uint64_t* child, uint64_t* new_child,
            uint64_t old_child_tail, size_t N) {

        if (new_child != child) {
            if (new_child != compact_type::sentinel())
                bitmask_type::replace_child(node, h, byte, new_child);
            else
                node = bitmask_type::remove_child(node, h, byte);
        }
        node[NODE_TOTAL_TAIL] -= old_child_tail
                               - node_tail_total(new_child)
                               + N * h.skip_bytes();
        h = hdr_type::from_node(node);
        if (h.count == 0 && !bitmask_type::has_eos(node, h)) {
            mem_v.free_node(node);
            return {compact_type::sentinel(), N};
        }
        if (node[NODE_TOTAL_TAIL] <= COMPACT_KEYSUFFIX_LIMIT) {
            uint64_t* collapsed = collapse_to_compact(node);
            if (collapsed != node) return {collapsed, N};
        }
        return {node, N};
    }

    // ------------------------------------------------------------------
    // prefix_walk_subtree — recursive walk, reconstructs keys
    // ------------------------------------------------------------------

    template<typename F>
    void prefix_walk_subtree(const uint64_t* node,
                             std::string& path, F&& fn) const {
        if (!node || node == compact_type::sentinel()) return;
        hdr_type h = hdr_type::from_node(node);
        size_t base = path.size();

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            append_unmapped(path, hdr_type::get_skip(node, h), sb);
        }

        if (h.is_compact()) [[unlikely]] {
            const uint8_t* L  = compact_type::lengths(node, h);
            const uint8_t* Fb = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t* B  = compact_type::keysuffix(node, h);
            const auto*    sb = h.get_compact_slots(node);
            for (uint16_t i = 0; i < h.count; ++i) {
                size_t eb = path.size();
                uint8_t klen = L[i];
                if (klen > 0) [[likely]] {
                    append_unmapped(path, &Fb[i], 1);
                    if (klen > 1) [[likely]]
                        append_unmapped(path, B + O[i], klen - 1);
                }
                fn(std::string_view(path),
                   *slots_type::load_value(sb, i));
                path.resize(eb);
            }
            path.resize(base);
            return;
        }

        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != compact_type::sentinel())
            prefix_walk_subtree(eos, path, fn);

        const auto* bm = bitmask_type::get_bitmap(node, h);
        int idx = bm->find_next_set(0);
        while (idx >= 0) {
            path.push_back(static_cast<char>(
                CHARMAP::from_index(static_cast<uint8_t>(idx))));
            int slot = bm->count_below(static_cast<uint8_t>(idx));
            prefix_walk_subtree(
                bitmask_type::child_by_slot(node, h, slot),
                path, fn);
            path.pop_back();
            idx = bm->find_next_set(idx + 1);
        }
        path.resize(base);
    }

    // ------------------------------------------------------------------
    // prefix_walk_filtered — compact entries matching remaining prefix
    // ------------------------------------------------------------------

    template<typename F>
    void prefix_walk_filtered(const uint64_t* node, const hdr_type& h,
                              const uint8_t* rem, uint32_t rlen,
                              std::string& path, F&& fn) const {
        const uint8_t* L  = compact_type::lengths(node, h);
        const uint8_t* Fb = compact_type::firsts(node, h);
        const ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B  = compact_type::keysuffix(node, h);
        const auto*    sb = h.get_compact_slots(node);
        for (uint16_t i = 0; i < h.count; ++i) {
            if (L[i] < rlen) continue;
            if (Fb[i] > rem[0]) break;
            if (Fb[i] < rem[0]) continue;
            if (rlen > 1 &&
                std::memcmp(B + O[i], rem + 1, rlen - 1) != 0) continue;
            // Suffix[0..rlen) already in path — append only beyond
            size_t eb = path.size();
            uint8_t klen = L[i];
            uint32_t tail_skip = rlen > 0 ? rlen - 1 : 0;
            uint32_t tail_len  = (klen > 0 ? klen - 1 : 0);
            if (tail_len > tail_skip)
                append_unmapped(path, B + O[i] + tail_skip,
                                tail_len - tail_skip);
            fn(std::string_view(path),
               *slots_type::load_value(sb, i));
            path.resize(eb);
        }
    }

    // ------------------------------------------------------------------
    // prefix_erase_node — recursive
    // ------------------------------------------------------------------

    prefix_erase_result prefix_erase_node(
            uint64_t* node, const uint8_t* pfx,
            uint32_t pfx_len, uint32_t consumed) {
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            const uint8_t* skip = hdr_type::get_skip(node, h);
            uint32_t remaining = pfx_len - consumed;
            if (remaining <= sb) {
                if (std::memcmp(skip, pfx + consumed, remaining) != 0)
                    return {node, 0};
                size_t n = count_subtree(node);
                destroy_tree(node);
                return {compact_type::sentinel(), n};
            }
            if (std::memcmp(skip, pfx + consumed, sb) != 0)
                return {node, 0};
            consumed += sb;
            h = hdr_type::from_node(node);
        }

        if (consumed >= pfx_len) {
            size_t n = count_subtree(node);
            destroy_tree(node);
            return {compact_type::sentinel(), n};
        }

        if (h.is_compact()) [[unlikely]]
            return prefix_erase_compact(node, h, pfx, pfx_len, consumed);

        uint8_t byte = pfx[consumed++];
        uint64_t* child = bitmask_type::dispatch(node, h, byte);
        if (child == compact_type::sentinel()) return {node, 0};

        uint64_t old_child_tail = node_tail_total(child);
        auto r = prefix_erase_node(child, pfx, pfx_len, consumed);
        if (r.erased == 0) return {node, 0};
        return unwind_child(node, h, byte, child, r.node,
                            old_child_tail, r.erased);
    }

    // ------------------------------------------------------------------
    // prefix_erase_compact — filter compact leaf
    // ------------------------------------------------------------------

    prefix_erase_result prefix_erase_compact(
            uint64_t* node, hdr_type h,
            const uint8_t* pfx, uint32_t pfx_len,
            uint32_t consumed) {

        uint32_t rlen = pfx_len - consumed;
        const uint8_t* rem = pfx + consumed;
        const uint8_t* L  = compact_type::lengths(node, h);
        const uint8_t* Fb = compact_type::firsts(node, h);
        const ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B  = compact_type::keysuffix(node, h);
        auto* sb = h.get_compact_slots(node);

        uint32_t skip_len = h.skip_bytes();
        uint8_t skip_buf[hdr_type::SKIP_MAX];
        if (skip_len > 0)
            std::memcpy(skip_buf, hdr_type::get_skip(node, h), skip_len);

        uint16_t entry_count = h.count;
        auto kept = std::make_unique<build_entry[]>(entry_count);
        auto kept_keys = std::make_unique<uint8_t[]>(
            static_cast<size_t>(entry_count) * COMPACT_ENTRY_KEY_MAX);
        size_t koff = 0;
        uint16_t nkept = 0;
        size_t nerased = 0;

        for (uint16_t i = 0; i < entry_count; ++i) {
            bool match = (L[i] >= rlen && Fb[i] == rem[0] &&
                (rlen <= 1 ||
                 std::memcmp(B + O[i], rem + 1, rlen - 1) == 0));

            if (match) {
                slots_type::destroy_value(sb, i, mem_v.alloc_v);
                ++nerased;
            } else {
                uint8_t klen = L[i];
                uint8_t* dst = kept_keys.get() + koff;
                if (klen > 0) {
                    dst[0] = Fb[i];
                    if (klen > 1)
                        std::memcpy(dst + 1, B + O[i], klen - 1);
                }
                kept[nkept] = {dst, klen, slots_type::load_raw(sb, i)};
                koff += klen;
                ++nkept;
            }
        }

        if (nerased == 0) return {node, 0};
        mem_v.free_node(node);

        if (nkept == 0) return {compact_type::sentinel(), nerased};

        uint64_t* nn = compact_type::build_compact(
            mem_v, static_cast<uint8_t>(skip_len),
            skip_len > 0 ? skip_buf : nullptr, kept.get(), nkept);
        return {nn, nerased};
    }

    // ------------------------------------------------------------------
    // prefix_clone_node — const, allocates into dest
    // ------------------------------------------------------------------

    prefix_clone_result prefix_clone_node(
            const uint64_t* node, const uint8_t* pfx,
            uint32_t pfx_len, uint32_t consumed,
            mem_type& dest) const {
        uint32_t entry_consumed = consumed;  // before this node
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            const uint8_t* skip = hdr_type::get_skip(node, h);
            uint32_t remaining = pfx_len - consumed;
            if (remaining <= sb) {
                if (std::memcmp(skip, pfx + consumed, remaining) != 0)
                    return {compact_type::sentinel(), 0, 0};
                size_t n = count_subtree(node);
                uint64_t* cloned = clone_tree_into(node, dest);
                return {cloned, n, entry_consumed};
            }
            if (std::memcmp(skip, pfx + consumed, sb) != 0)
                return {compact_type::sentinel(), 0, 0};
            consumed += sb;
            h = hdr_type::from_node(node);
        }

        if (consumed >= pfx_len) {
            size_t n = count_subtree(node);
            uint64_t* cloned = clone_tree_into(node, dest);
            return {cloned, n, entry_consumed};
        }

        if (h.is_compact()) [[unlikely]]
            return prefix_clone_compact(node, h, pfx, pfx_len,
                                        consumed, entry_consumed, dest);

        uint8_t byte = pfx[consumed++];
        const uint64_t* child = bitmask_type::dispatch(node, h, byte);
        if (child == compact_type::sentinel())
            return {compact_type::sentinel(), 0, 0};

        return prefix_clone_node(child, pfx, pfx_len, consumed, dest);
    }

    // ------------------------------------------------------------------
    // prefix_clone_compact
    // ------------------------------------------------------------------

    prefix_clone_result prefix_clone_compact(
            const uint64_t* node, const hdr_type& h,
            const uint8_t* pfx, uint32_t pfx_len,
            uint32_t consumed, uint32_t entry_consumed,
            mem_type& dest) const {

        uint32_t rlen = pfx_len - consumed;
        const uint8_t* rem = pfx + consumed;
        const uint8_t* L  = compact_type::lengths(node, h);
        const uint8_t* Fb = compact_type::firsts(node, h);
        const ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B  = compact_type::keysuffix(node, h);
        const auto* sb = h.get_compact_slots(node);

        uint16_t entry_count = h.count;
        auto matches = std::make_unique<build_entry[]>(entry_count);
        auto match_keys = std::make_unique<uint8_t[]>(
            static_cast<size_t>(entry_count) * COMPACT_ENTRY_KEY_MAX);
        size_t moff = 0;
        uint16_t nmatch = 0;

        for (uint16_t i = 0; i < entry_count; ++i) {
            if (L[i] < rlen) continue;
            if (Fb[i] > rem[0]) break;
            if (Fb[i] < rem[0]) continue;
            if (rlen > 1 &&
                std::memcmp(B + O[i], rem + 1, rlen - 1) != 0) continue;

            uint8_t klen = L[i];
            uint8_t* dst = match_keys.get() + moff;
            if (klen > 0) {
                dst[0] = Fb[i];
                if (klen > 1)
                    std::memcpy(dst + 1, B + O[i], klen - 1);
            }
            uint64_t raw = slots_type::load_raw(sb, i);
            if constexpr (!slots_type::IS_TRIVIAL) {
                const VALUE* vp = slots_type::load_value(sb, i);
                raw = slots_type::make_raw(*vp, dest.alloc_v);
            }
            matches[nmatch] = {dst, klen, raw};
            moff += klen;
            ++nmatch;
        }

        if (nmatch == 0) return {compact_type::sentinel(), 0, 0};

        uint32_t skip_len = h.skip_bytes();
        const uint8_t* skip_data = skip_len > 0
            ? hdr_type::get_skip(node, h) : nullptr;

        uint64_t* nn = compact_type::build_compact(
            dest, static_cast<uint8_t>(skip_len), skip_data,
            matches.get(), nmatch);
        return {nn, nmatch, entry_consumed};
    }

    // ------------------------------------------------------------------
    // prefix_split_node — recursive, fused steal+erase
    // ------------------------------------------------------------------

    prefix_split_result prefix_split_node(
            uint64_t* node, const uint8_t* pfx,
            uint32_t pfx_len, uint32_t consumed) {
        uint32_t entry_consumed = consumed;  // before this node
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            const uint8_t* skip = hdr_type::get_skip(node, h);
            uint32_t remaining = pfx_len - consumed;
            if (remaining <= sb) {
                if (std::memcmp(skip, pfx + consumed, remaining) != 0)
                    return {node, compact_type::sentinel(), 0, 0};
                size_t n = count_subtree(node);
                return {compact_type::sentinel(), node, n, entry_consumed};
            }
            if (std::memcmp(skip, pfx + consumed, sb) != 0)
                return {node, compact_type::sentinel(), 0, 0};
            consumed += sb;
            h = hdr_type::from_node(node);
        }

        if (consumed >= pfx_len) {
            size_t n = count_subtree(node);
            return {compact_type::sentinel(), node, n, entry_consumed};
        }

        if (h.is_compact()) [[unlikely]]
            return prefix_split_compact(node, h, pfx, pfx_len,
                                        consumed, entry_consumed);

        uint8_t byte = pfx[consumed++];
        uint64_t* child = bitmask_type::dispatch(node, h, byte);
        if (child == compact_type::sentinel())
            return {node, compact_type::sentinel(), 0, 0};

        uint64_t old_child_tail = node_tail_total(child);
        auto r = prefix_split_node(child, pfx, pfx_len, consumed);
        if (r.count == 0) return {node, compact_type::sentinel(), 0, 0};

        auto er = unwind_child(node, h, byte, child, r.source,
                               old_child_tail, r.count);
        return {er.node, r.stolen, r.count, r.path_len};
    }

    // ------------------------------------------------------------------
    // prefix_split_compact — zero-copy ownership transfer
    // ------------------------------------------------------------------

    prefix_split_result prefix_split_compact(
            uint64_t* node, hdr_type h,
            const uint8_t* pfx, uint32_t pfx_len,
            uint32_t consumed, uint32_t entry_consumed) {

        uint32_t rlen = pfx_len - consumed;
        const uint8_t* rem = pfx + consumed;
        const uint8_t* L  = compact_type::lengths(node, h);
        const uint8_t* Fb = compact_type::firsts(node, h);
        const ks_offset_type* O = compact_type::offsets(node, h);
        const uint8_t* B  = compact_type::keysuffix(node, h);
        const auto* sb = h.get_compact_slots(node);

        uint32_t skip_len = h.skip_bytes();
        uint8_t skip_buf[hdr_type::SKIP_MAX];
        if (skip_len > 0)
            std::memcpy(skip_buf, hdr_type::get_skip(node, h), skip_len);

        uint16_t entry_count = h.count;
        auto kept = std::make_unique<build_entry[]>(entry_count);
        auto kept_keys = std::make_unique<uint8_t[]>(
            static_cast<size_t>(entry_count) * COMPACT_ENTRY_KEY_MAX);
        auto stolen = std::make_unique<build_entry[]>(entry_count);
        auto stolen_keys = std::make_unique<uint8_t[]>(
            static_cast<size_t>(entry_count) * COMPACT_ENTRY_KEY_MAX);
        size_t koff = 0, soff = 0;
        uint16_t nkept = 0, nstolen = 0;

        for (uint16_t i = 0; i < entry_count; ++i) {
            bool match = (L[i] >= rlen && Fb[i] == rem[0] &&
                (rlen <= 1 ||
                 std::memcmp(B + O[i], rem + 1, rlen - 1) == 0));

            uint8_t klen = L[i];
            uint64_t raw = slots_type::load_raw(sb, i);

            if (match) {
                uint8_t* dst = stolen_keys.get() + soff;
                if (klen > 0) {
                    dst[0] = Fb[i];
                    if (klen > 1) std::memcpy(dst + 1, B + O[i], klen - 1);
                }
                stolen[nstolen] = {dst, klen, raw};
                soff += klen;
                ++nstolen;
            } else {
                uint8_t* dst = kept_keys.get() + koff;
                if (klen > 0) {
                    dst[0] = Fb[i];
                    if (klen > 1) std::memcpy(dst + 1, B + O[i], klen - 1);
                }
                kept[nkept] = {dst, klen, raw};
                koff += klen;
                ++nkept;
            }
        }

        if (nstolen == 0)
            return {node, compact_type::sentinel(), 0, 0};

        mem_v.free_node(node);

        uint64_t* stolen_node = compact_type::build_compact(
            mem_v, static_cast<uint8_t>(skip_len),
            skip_len > 0 ? skip_buf : nullptr,
            stolen.get(), nstolen);

        uint64_t* source_node = compact_type::sentinel();
        if (nkept > 0)
            source_node = compact_type::build_compact(
                mem_v, static_cast<uint8_t>(skip_len),
                skip_len > 0 ? skip_buf : nullptr,
                kept.get(), nkept);

        return {source_node, stolen_node, nstolen, entry_consumed};
    }

    // ------------------------------------------------------------------
    // find_inner -- hot loop for trie traversal
    // ------------------------------------------------------------------

    const VALUE* find_inner(const uint8_t* mapped, uint32_t key_len) const noexcept {
        const uint64_t* node = root_v;
        uint32_t consumed = 0;
        hdr_type h;

        for (;;) {
            h = hdr_type::from_node(node);
            if (h.has_skip()) [[unlikely]] {
                if (!skip_type::match_skip_unchecked(node, h, mapped, key_len, consumed))
                    [[unlikely]] return nullptr;
            }
            if (!h.is_bitmap()) [[unlikely]] break;
            if (consumed == key_len) [[unlikely]] {
                node = bitmask_type::eos_child(node, h);
                h = hdr_type::from_node(node);
                break;
            }
            node = bitmask_type::dispatch(node, h, mapped[consumed++]);
        }
        // Cold path: compact leaf node
        return compact_type::find(node, h, mapped + consumed, key_len - consumed);
    }

    // ------------------------------------------------------------------
    // find_impl -- wrapper: mapping + heap ownership
    // ------------------------------------------------------------------

    const VALUE* find_impl(const uint8_t* key_data, uint32_t key_len) const noexcept {
        if constexpr (CHARMAP::IS_IDENTITY) {
            return find_inner(key_data, key_len);
        } else {
            uint8_t stack_buf[256];
            auto [mapped, raw_buf] = get_mapped<CHARMAP>(key_data, key_len,
                                                  stack_buf, sizeof(stack_buf));
            std::unique_ptr<uint8_t[]> heap_guard(raw_buf);
            const VALUE* result = find_inner(mapped, key_len);
            return result;
        }
    }


public:
    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    kstrie_impl() { init_empty_root(); }
    ~kstrie_impl() { if (root_v) destroy_tree(root_v); }

    kstrie_impl(kstrie_impl&& o) noexcept
        : root_v(o.root_v), size_v(o.size_v), mem_v(std::move(o.mem_v)) {
        o.root_v = compact_type::sentinel();
        o.size_v = 0;
    }

    kstrie_impl& operator=(kstrie_impl&& o) noexcept {
        if (this != &o) {
            if (root_v) destroy_tree(root_v);
            root_v = o.root_v;
            size_v = o.size_v;
            mem_v  = std::move(o.mem_v);
            o.root_v = compact_type::sentinel();
            o.size_v = 0;
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Deep copy
    // ------------------------------------------------------------------

    kstrie_impl(const kstrie_impl& o) : size_v(o.size_v) {
        if (!o.root_v) {
            root_v = compact_type::sentinel();
        } else {
            set_root(clone_tree(o.root_v));
        }
    }

    kstrie_impl& operator=(const kstrie_impl& o) {
        if (this != &o) {
            kstrie_impl tmp(o);
            swap(tmp);
        }
        return *this;
    }

private:
    uint64_t* clone_tree(const uint64_t* node) {
        if (!node || node == compact_type::sentinel()) return const_cast<uint64_t*>(node);
        hdr_type h = hdr_type::from_node(node);

        size_t nu = h.alloc_u64;
        uint64_t* copy = mem_v.alloc_node(nu);
        std::memcpy(copy, node, nu * U64_BYTES);

        if (h.is_compact()) {
            auto* sb = hdr_type::from_node(copy).get_compact_slots(copy);
            for (uint16_t i = 0; i < h.count; ++i) {
                const VALUE* vp = slots_type::load_value(
                    h.get_compact_slots(node), i);
                sb[i] = {};
                slots_type::store_value(sb, i, *vp, mem_v.alloc_v);
            }
        } else {
            // bitmask: recurse children + eos, no leaf cloning
            uint64_t* new_cs = bitmask_type::child_slots(copy);
            const uint64_t* old_cs = bitmask_type::child_slots(node);
            for (uint16_t i = 0; i < h.count; ++i) {
                uint64_t* child = slots_type::load_child(old_cs, i);
                slots_type::store_child(new_cs, i, clone_tree(child));
            }
            hdr_type& ch = hdr_type::from_node(copy);
            uint64_t* old_eos = bitmask_type::eos_child(node, h);
            bitmask_type::set_eos_child(copy, ch, clone_tree(old_eos));
            // Re-link all children to point to the new copy as parent
            bitmask_type::link_all_children(copy);
        }
        return copy;
    }

public:
    // ------------------------------------------------------------------
    // Capacity
    // ------------------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return size_v == 0; }
    [[nodiscard]] size_type size() const noexcept { return size_v; }
    [[nodiscard]] size_type memory_usage() const noexcept {
        return sizeof(*this) + memory_usage_impl(root_v);
    }

    // ------------------------------------------------------------------
    // Root access (for kstrie iterator layer)
    // ------------------------------------------------------------------

    [[nodiscard]] const uint64_t* get_root() const noexcept { return root_v; }
    [[nodiscard]] uint64_t* get_root_mut() noexcept { return root_v; }
    [[nodiscard]] uint64_t* get_sentinel() const noexcept { return compact_type::sentinel(); }

    // find_for_iter: find key, return leaf + position for iterator construction.
    // Returns {nullptr, 0} on miss.
    struct iter_find_result {
        uint64_t* leaf = nullptr;
        uint16_t  pos  = 0;
        size_t    prefix_len = 0;  // key bytes consumed before leaf suffix
    };

    iter_find_result find_for_iter(std::string_view key) const noexcept {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        return find_leaf_pos(const_cast<uint64_t*>(root_v), mapped, len, 0);
    }

    // find_leaf_pos: locate a key within a subtree, returning leaf + pos.
    // Used by find_for_iter (from root) and by insert paths after
    // split/rebuild/promote (from subtree root).
    static iter_find_result find_leaf_pos(uint64_t* node,
                                           const uint8_t* mapped,
                                           uint32_t key_len,
                                           uint32_t consumed) noexcept {
        hdr_type h;
        for (;;) {
            if (node == compact_type::sentinel()) return {};
            h = hdr_type::from_node(node);
            if (h.has_skip()) [[unlikely]] {
                if (!skip_type::match_skip_fast(node, h, mapped, key_len, consumed))
                    return {};
            }
            if (!h.is_bitmap()) [[unlikely]] break;
            if (consumed == key_len) [[unlikely]] {
                node = bitmask_type::eos_child(node, h);
                if (node == compact_type::sentinel()) return {};
                h = hdr_type::from_node(node);
                break;
            }
            node = bitmask_type::dispatch(node, h, mapped[consumed++]);
        }
        auto [found, pos] = compact_type::find_pos(
            node, h, mapped + consumed, key_len - consumed);
        if (!found) return {};
        return {node, static_cast<uint16_t>(pos),
                static_cast<size_t>(key_len - compact_type::lengths(node, h)[pos])};
    }

    // ------------------------------------------------------------------
    // Lookup
    // ------------------------------------------------------------------

    const VALUE* find(std::string_view key) const {
        return find_impl(
            reinterpret_cast<const uint8_t*>(key.data()),
            static_cast<uint32_t>(key.size()));
    }

    // Mutable find() intentionally absent. Compact nodes use dup-slot
    // padding — returning VALUE* would allow writes that corrupt the
    // dup invariant. Use insert_or_assign() or assign() for writes.

    bool contains(std::string_view key) const {
        return find(key) != nullptr;
    }

    // ------------------------------------------------------------------
    // Modifiers
    // ------------------------------------------------------------------

    bool insert(std::string_view key, const VALUE& value) {
        return modify_impl(key, value, insert_mode::INSERT);
    }

    bool insert_or_assign(std::string_view key, const VALUE& value) {
        return modify_impl(key, value, insert_mode::UPSERT);
    }

    bool assign(std::string_view key, const VALUE& value) {
        return modify_impl(key, value, insert_mode::ASSIGN);
    }

    size_type erase(std::string_view key) {
        if (root_v == compact_type::sentinel()) return 0;

        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        erase_info r = erase_node(root_v, mapped, len, 0);

        if (r.status == erase_status::MISSING)
            return 0;

        if (r.status == erase_status::DONE) {
            set_root(r.leaf);
            size_v--;
            return 1;
        }

        // PENDING: entry found but not erased yet.
        // desc is remaining count after erase.
        // PENDING only reaches here from a compact root.
        // do_leaf_erase frees the old node and sets r.leaf to the rebuilt one.
        do_leaf_erase(r.leaf, r.pos);
        if (r.desc == 0) {
            // r.leaf is a 0-entry node; free it
            mem_v.free_node(r.leaf);
            root_v = compact_type::sentinel();
        } else {
            set_root(r.leaf);
        }
        size_v--;
        return 1;
    }


    template<typename StringOut>
    static void append_unmapped(StringOut& out,
                                const uint8_t* data, uint32_t len) {
        if (len == 0) [[unlikely]] return;
        if constexpr (CHARMAP::IS_IDENTITY) {
            out.append(reinterpret_cast<const char*>(data), len);
        } else {
            for (uint32_t i = 0; i < len; ++i)
                out.push_back(static_cast<char>(CHARMAP::from_index(data[i])));
        }
    }

    uint64_t* reskip_with_prefix(uint64_t* node,
                                  const uint8_t* prefix,
                                  uint32_t prefix_len) {
        hdr_type h = hdr_type::from_node(node);
        uint32_t old_skip = h.skip_bytes();
        uint32_t new_skip = prefix_len + old_skip;
        uint8_t buf[hdr_type::SKIP_MAX];
        std::memcpy(buf, prefix, prefix_len);
        if (old_skip > 0)
            std::memcpy(buf + prefix_len,
                        hdr_type::get_skip(node, h), old_skip);
        if (h.is_compact())
            return compact_type::reskip(node, h, mem_v,
                static_cast<uint8_t>(new_skip), buf);
        else
            return bitmask_type::reskip(node, h, mem_v,
                static_cast<uint8_t>(new_skip), buf);
    }

    mem_type& get_mem() noexcept { return mem_v; }

    void set_root(uint64_t* new_root, size_t new_size) {
        set_root(new_root);
        size_v = new_size;
    }

    // ------------------------------------------------------------------
    // prefix_count_impl
    // ------------------------------------------------------------------

    size_t prefix_count_impl(const uint8_t* mapped, uint32_t len) const {
        if (root_v == compact_type::sentinel()) return 0;
        if (len == 0) return size_v;
        const uint64_t* node = root_v;
        uint32_t consumed = 0;
        for (;;) {
            hdr_type h = hdr_type::from_node(node);
            if (h.has_skip()) [[unlikely]] {
                uint32_t sb = h.skip_bytes();
                const uint8_t* skip = hdr_type::get_skip(node, h);
                uint32_t remaining = len - consumed;
                if (remaining <= sb)
                    return std::memcmp(skip, mapped + consumed, remaining) == 0
                           ? count_subtree(node) : 0;
                if (std::memcmp(skip, mapped + consumed, sb) != 0) return 0;
                consumed += sb;
            }
            if (consumed >= len) return count_subtree(node);
            h = hdr_type::from_node(node);
            if (h.is_compact()) [[unlikely]] {
                const uint8_t* L  = compact_type::lengths(node, h);
                const uint8_t* Fb = compact_type::firsts(node, h);
                const ks_offset_type* O = compact_type::offsets(node, h);
                const uint8_t* B  = compact_type::keysuffix(node, h);
                uint32_t rlen = len - consumed;
                const uint8_t* rem = mapped + consumed;
                size_t count = 0;
                for (uint16_t i = 0; i < h.count; ++i) {
                    if (L[i] < rlen) continue;
                    if (Fb[i] > rem[0]) break;
                    if (Fb[i] < rem[0]) continue;
                    if (rlen > 1 &&
                        std::memcmp(B + O[i], rem + 1, rlen - 1) != 0)
                        continue;
                    ++count;
                }
                return count;
            }
            uint8_t byte = mapped[consumed++];
            node = bitmask_type::dispatch(node, h, byte);
            if (node == compact_type::sentinel()) return 0;
        }
    }

    // ------------------------------------------------------------------
    // prefix_walk_impl
    // ------------------------------------------------------------------

    template<typename F>
    void prefix_walk_impl(const uint8_t* mapped, uint32_t len,
                          std::string_view original_prefix, F&& fn) const {
        if (root_v == compact_type::sentinel()) return;
        std::string path(original_prefix);

        if (len == 0) {
            prefix_walk_subtree(root_v, path, std::forward<F>(fn));
            return;
        }

        const uint64_t* node = root_v;
        uint32_t consumed = 0;
        for (;;) {
            hdr_type h = hdr_type::from_node(node);
            if (h.has_skip()) [[unlikely]] {
                uint32_t sb = h.skip_bytes();
                const uint8_t* skip = hdr_type::get_skip(node, h);
                uint32_t remaining = len - consumed;
                if (remaining <= sb) {
                    if (std::memcmp(skip, mapped + consumed, remaining) != 0)
                        return;
                    for (uint32_t j = remaining; j < sb; ++j)
                        path.push_back(static_cast<char>(
                            CHARMAP::from_index(skip[j])));
                    consumed += sb;
                    h = hdr_type::from_node(node);
                    if (h.is_compact()) {
                        const uint8_t* L  = compact_type::lengths(node, h);
                        const uint8_t* Fb = compact_type::firsts(node, h);
                        const ks_offset_type* O =
                            compact_type::offsets(node, h);
                        const uint8_t* B  = compact_type::keysuffix(node, h);
                        const auto* s = h.get_compact_slots(node);
                        for (uint16_t i = 0; i < h.count; ++i) {
                            size_t eb = path.size();
                            uint8_t klen = L[i];
                            if (klen > 0) [[likely]] {
                                append_unmapped(path, &Fb[i], 1);
                                if (klen > 1) [[likely]]
                                    append_unmapped(path, B + O[i], klen - 1);
                            }
                            fn(std::string_view(path),
                               *slots_type::load_value(s, i));
                            path.resize(eb);
                        }
                    } else {
                        uint64_t* eos = bitmask_type::eos_child(node, h);
                        if (eos != compact_type::sentinel())
                            prefix_walk_subtree(eos, path, fn);
                        const auto* bm = bitmask_type::get_bitmap(node, h);
                        int idx = bm->find_next_set(0);
                        while (idx >= 0) {
                            path.push_back(static_cast<char>(
                                CHARMAP::from_index(
                                    static_cast<uint8_t>(idx))));
                            int slot = bm->count_below(
                                static_cast<uint8_t>(idx));
                            prefix_walk_subtree(
                                bitmask_type::child_by_slot(node, h, slot),
                                path, fn);
                            path.pop_back();
                            idx = bm->find_next_set(idx + 1);
                        }
                    }
                    return;
                }
                if (std::memcmp(skip, mapped + consumed, sb) != 0) return;
                consumed += sb;
            }
            h = hdr_type::from_node(node);
            if (consumed >= len) {
                // Skip already consumed — walk post-skip directly
                if (h.is_compact()) {
                    const uint8_t* L  = compact_type::lengths(node, h);
                    const uint8_t* Fb = compact_type::firsts(node, h);
                    const ks_offset_type* O =
                        compact_type::offsets(node, h);
                    const uint8_t* B  = compact_type::keysuffix(node, h);
                    const auto* s = h.get_compact_slots(node);
                    for (uint16_t i = 0; i < h.count; ++i) {
                        size_t eb = path.size();
                        uint8_t klen = L[i];
                        if (klen > 0) [[likely]] {
                            append_unmapped(path, &Fb[i], 1);
                            if (klen > 1) [[likely]]
                                append_unmapped(path, B + O[i], klen - 1);
                        }
                        fn(std::string_view(path),
                           *slots_type::load_value(s, i));
                        path.resize(eb);
                    }
                } else {
                    uint64_t* eos = bitmask_type::eos_child(node, h);
                    if (eos != compact_type::sentinel())
                        prefix_walk_subtree(eos, path, fn);
                    const auto* bm = bitmask_type::get_bitmap(node, h);
                    int idx = bm->find_next_set(0);
                    while (idx >= 0) {
                        path.push_back(static_cast<char>(
                            CHARMAP::from_index(
                                static_cast<uint8_t>(idx))));
                        int slot = bm->count_below(
                            static_cast<uint8_t>(idx));
                        prefix_walk_subtree(
                            bitmask_type::child_by_slot(node, h, slot),
                            path, fn);
                        path.pop_back();
                        idx = bm->find_next_set(idx + 1);
                    }
                }
                return;
            }
            if (h.is_compact()) [[unlikely]] {
                prefix_walk_filtered(node, h,
                    mapped + consumed, len - consumed,
                    path, std::forward<F>(fn));
                return;
            }
            uint8_t byte = mapped[consumed++];
            node = bitmask_type::dispatch(node, h, byte);
            if (node == compact_type::sentinel()) return;
        }
    }

    // ------------------------------------------------------------------
    // prefix_erase — public entry
    // ------------------------------------------------------------------

    size_t prefix_erase(const uint8_t* mapped, uint32_t len) {
        if (root_v == compact_type::sentinel()) return 0;
        if (len == 0) {
            size_t n = size_v;
            destroy_tree(root_v);
            init_empty_root();
            size_v = 0;
            return n;
        }
        auto r = prefix_erase_node(root_v, mapped, len, 0);
        set_root(r.node);
        size_v -= r.erased;
        return r.erased;
    }

    // ------------------------------------------------------------------
    // prefix_clone — const, allocates into dest_mem
    // ------------------------------------------------------------------

    prefix_clone_result prefix_clone(
            const uint8_t* mapped, uint32_t len,
            mem_type& dest) const {
        if (root_v == compact_type::sentinel())
            return {compact_type::sentinel(), 0, 0};
        if (len == 0) {
            size_t n = size_v;
            uint64_t* cloned = clone_tree_into(root_v, dest);
            return {cloned, n, 0};
        }
        return prefix_clone_node(root_v, mapped, len, 0, dest);
    }

    // ------------------------------------------------------------------
    // prefix_split_impl — steals subtree, fixes source
    // ------------------------------------------------------------------

    prefix_split_result prefix_split_impl(
            const uint8_t* mapped, uint32_t len) {
        if (root_v == compact_type::sentinel())
            return {root_v, compact_type::sentinel(), 0, 0};
        if (len == 0) {
            uint64_t* stolen = root_v;
            size_t n = size_v;
            init_empty_root();
            size_v = 0;
            return {root_v, stolen, n, 0};
        }
        auto r = prefix_split_node(root_v, mapped, len, 0);
        set_root(r.source);
        size_v -= r.count;
        return r;
    }

    void clear() noexcept {
        if (root_v) destroy_tree(root_v);
        init_empty_root();
        size_v = 0;
    }

    // ------------------------------------------------------------------
    // Utilities
    // ------------------------------------------------------------------

    size_type count(std::string_view key) const {
        return contains(key) ? 1 : 0;
    }

    void swap(kstrie_impl& o) noexcept {
        std::swap(root_v, o.root_v);
        std::swap(size_v, o.size_v);
        std::swap(mem_v, o.mem_v);
    }

    [[nodiscard]] size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max();
    }

    [[nodiscard]] allocator_type get_allocator() const noexcept {
        return mem_v.alloc_v;
    }

    mem_type& memory() noexcept { return mem_v; }

private:

    // ------------------------------------------------------------------
    // Erase internals
    // ------------------------------------------------------------------

    void do_leaf_erase(uint64_t*& leaf, int pos) {
        hdr_type& lh = hdr_type::from_node(leaf);
        compact_type::erase_in_place(leaf, lh, pos, mem_v);

        auto& p = compact_type::get_prefix(leaf, lh);
        if (compact_type::should_shrink(lh.count, p.cap))
            leaf = compact_type::shrink_compact(leaf, lh, mem_v);
    }

    uint64_t node_tail_total(const uint64_t* node) const {
        if (!node || node == compact_type::sentinel()) return 0;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_compact()) {
            // Each entry gains: 1 dispatch byte + skip bytes from this node
            return compact_type::get_prefix(node, h).keysuffix_used
                 + static_cast<uint64_t>(h.count) * (1 + h.skip_bytes());
        }
        return node[NODE_TOTAL_TAIL];
    }

    using build_entry = typename compact_type::build_entry;

    void collect_inner(const uint64_t* node,
                       uint8_t* prefix, uint32_t prefix_len,
                       build_entry* out, uint8_t* key_buf,
                       size_t& buf_off, uint32_t& ei,
                       const uint64_t* skip_leaf, int skip_pos) const {
        if (!node || node == compact_type::sentinel()) return;
        hdr_type h = hdr_type::from_node(node);

        if (h.has_skip()) [[unlikely]] {
            uint32_t sb = h.skip_bytes();
            std::memcpy(prefix + prefix_len,
                        hdr_type::get_skip(node, h), sb);
            prefix_len += sb;
        }

        collect_post_skip(node, h, prefix, prefix_len,
                          out, key_buf, buf_off, ei,
                          skip_leaf, skip_pos);
    }

    void collect_post_skip(const uint64_t* node, const hdr_type& h,
                           uint8_t* prefix, uint32_t prefix_len,
                           build_entry* out, uint8_t* key_buf,
                           size_t& buf_off, uint32_t& ei,
                           const uint64_t* skip_leaf, int skip_pos) const {
        if (h.is_compact()) {
            const uint8_t*  L  = compact_type::lengths(node, h);
            const uint8_t*  F  = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t*  B  = compact_type::keysuffix(node, h);
            const auto*     sb = h.get_compact_slots(node);
            for (uint16_t i = 0; i < h.count; ++i) {
                if (node != skip_leaf ||
                    static_cast<int>(i) != skip_pos) {
                    uint8_t  klen = L[i];
                    uint8_t* dst  = key_buf + buf_off;
                    if (prefix_len > 0)
                        std::memcpy(dst, prefix, prefix_len);
                    if (klen > 0) {
                        dst[prefix_len] = F[i];
                        if (klen > 1)
                            std::memcpy(dst + prefix_len + 1, B + O[i], klen - 1);
                    }
                    out[ei].key      = dst;
                    out[ei].key_len  = prefix_len + klen;
                    out[ei].raw_slot = slots_type::load_raw(sb, i);
                    buf_off += prefix_len + klen;
                    ei++;
                }
            }
            return;
        }

        // Bitmask: eos_child + child bitmap only (no leaves)
        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != compact_type::sentinel()) {
            collect_inner(eos, prefix, prefix_len,
                          out, key_buf, buf_off, ei, skip_leaf, skip_pos);
        }
        const auto* bm = bitmask_type::get_bitmap(node, h);
        int idx = bm->find_next_set(0);
        while (idx >= 0) {
            uint8_t byte = static_cast<uint8_t>(idx);
            prefix[prefix_len] = byte;
            int cs = bm->count_below(byte);
            uint64_t* child = bitmask_type::child_by_slot(node, h, cs);
            collect_inner(child, prefix, prefix_len + 1,
                          out, key_buf, buf_off, ei, skip_leaf, skip_pos);
            idx = bm->find_next_set(idx + 1);
        }
    }

    void free_subtree_nodes(uint64_t* node) {
        if (!node || node == compact_type::sentinel()) return;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_bitmap()) {
            for (uint16_t ci = 0; ci < h.count; ++ci)
                free_subtree_nodes(bitmask_type::child_by_slot(node, h, ci));
            free_subtree_nodes(bitmask_type::eos_child(node, h));
        }
        // compact: leaf-only, no children to free
        mem_v.free_node(node);
    }

    // ------------------------------------------------------------------
    // fill_layout — recursive walker for collapse_to_compact.
    // Fills stack-local L/F/O/keysuffix/values arrays directly.
    // prefix_buf[0..prefix_len-1]: bytes to prepend to all entries.
    // Returns false if keysuffix exceeds 256 bytes or entries exceed max.
    // ------------------------------------------------------------------

    static constexpr uint32_t MAX_COLLAPSE_ENTRIES = COMPACT_KEYSUFFIX_LIMIT + BITMASK_MAX_CHILDREN;

    bool fill_layout(const uint64_t* node, const hdr_type& h,
                     const uint8_t* prefix_buf, uint32_t prefix_len,
                     uint8_t* cL, uint8_t* cF, ks_offset_type* cO, uint8_t* cblob,
                     typename slots_type::value_base_t* cvals,
                     uint16_t& ei, uint16_t& blob_cursor,
                     int& chain_start) const {

        if (h.is_compact()) {
            const uint8_t* L  = compact_type::lengths(node, h);
            const uint8_t* F  = compact_type::firsts(node, h);
            const ks_offset_type* O = compact_type::offsets(node, h);
            const uint8_t* B  = compact_type::keysuffix(node, h);
            const auto*    sb = h.get_compact_slots(node);

            for (uint16_t i = 0; i < h.count; ++i) {
                if (ei >= MAX_COLLAPSE_ENTRIES) return false;

                uint8_t orig_klen = L[i];
                uint8_t orig_tail = orig_klen > 0 ? orig_klen - 1 : 0;
                uint32_t new_klen = prefix_len + orig_klen;
                if (new_klen > COMPACT_SUFFIX_LEN_MAX) return false;

                uint8_t new_fb;
                uint8_t new_tail_len;

                if (prefix_len > 0) {
                    new_fb = prefix_buf[0];
                    new_tail_len = static_cast<uint8_t>(new_klen - 1);
                } else if (orig_klen > 0) {
                    new_fb = F[i];
                    new_tail_len = orig_tail;
                } else {
                    new_fb = 0;
                    new_tail_len = 0;
                }

                cL[ei] = static_cast<uint8_t>(new_klen);
                cF[ei] = new_fb;
                slots_type::copy_values(cvals, ei, sb, i, 1);

                if (new_tail_len == 0) {
                    cO[ei] = static_cast<ks_offset_type>(blob_cursor);
                    ei++;
                    continue;
                }

                // Sharing: source sharing implies collapsed sharing
                // when prefix is the same (same compact child).
                bool shares_into_next = false;
                if (i + 1 < h.count) {
                    uint32_t next_new_klen = prefix_len + L[i+1];
                    if (next_new_klen > new_klen) {
                        if (prefix_len > 0) {
                            // Both have same prefix → same new F.
                            // Share if source entries share.
                            if (F[i] == F[i+1] &&
                                O[i] == O[i+1] &&
                                orig_klen < L[i+1])
                                shares_into_next = true;
                        } else {
                            if (O[i] == O[i+1] && F[i] == F[i+1])
                                shares_into_next = true;
                        }
                    }
                }

                if (shares_into_next) {
                    if (chain_start < 0) chain_start = static_cast<int>(ei);
                    ei++;
                    continue;
                }

                // Chain head or standalone: write keysuffix bytes
                if (blob_cursor + new_tail_len > COMPACT_KEYSUFFIX_LIMIT) return false;

                cO[ei] = static_cast<ks_offset_type>(blob_cursor);

                // Assemble tail: prefix[1..] + F[i] + B[O[i]..orig_tail]
                uint16_t off = blob_cursor;
                if (prefix_len > 1) {
                    std::memcpy(cblob + off, prefix_buf + 1, prefix_len - 1);
                    off += prefix_len - 1;
                }
                if (prefix_len > 0 && orig_klen > 0) {
                    cblob[off++] = F[i];
                }
                if (orig_tail > 0)
                    std::memcpy(cblob + off, B + O[i], orig_tail);

                // Backfill chain
                if (chain_start >= 0) {
                    for (int j = chain_start; j < static_cast<int>(ei); ++j)
                        if (cL[j] > 0) cO[j] = cO[ei];
                    chain_start = -1;
                }

                blob_cursor += new_tail_len;
                ei++;
            }
            return true;
        }

        // Bitmask node

        // EOS child
        uint64_t* eos = bitmask_type::eos_child(node, h);
        if (eos != compact_type::sentinel()) {
            hdr_type eh = hdr_type::from_node(eos);

            uint8_t eos_prefix[COMPACT_KEYSUFFIX_LIMIT];
            uint32_t eos_prefix_len = prefix_len;
            if (prefix_len > 0)
                std::memcpy(eos_prefix, prefix_buf, prefix_len);
            if (eh.has_skip()) {
                const uint8_t* sp = hdr_type::get_skip(eos, eh);
                std::memcpy(eos_prefix + eos_prefix_len, sp, eh.skip);
                eos_prefix_len += eh.skip;
            }

            if (!fill_layout(eos, eh,
                             eos_prefix_len > 0 ? eos_prefix : nullptr,
                             eos_prefix_len,
                             cL, cF, cO, cblob, cvals,
                             ei, blob_cursor, chain_start))
                return false;
        }

        // Children in byte order
        const auto* bm = bitmask_type::get_bitmap(node, h);
        int idx = bm->find_next_set(0);
        while (idx >= 0) {
            uint8_t byte = static_cast<uint8_t>(idx);
            int cs = bm->count_below(byte);
            uint64_t* child = slots_type::load_child(
                bitmask_type::child_slots(node), cs);
            hdr_type ch = hdr_type::from_node(child);

            uint8_t child_prefix[COMPACT_KEYSUFFIX_LIMIT];
            uint32_t child_prefix_len = prefix_len;
            if (prefix_len > 0)
                std::memcpy(child_prefix, prefix_buf, prefix_len);
            child_prefix[child_prefix_len++] = byte;
            if (ch.has_skip()) {
                const uint8_t* sp = hdr_type::get_skip(child, ch);
                std::memcpy(child_prefix + child_prefix_len, sp, ch.skip);
                child_prefix_len += ch.skip;
            }

            if (!fill_layout(child, ch,
                             child_prefix, child_prefix_len,
                             cL, cF, cO, cblob, cvals,
                             ei, blob_cursor, chain_start))
                return false;

            idx = bm->find_next_set(idx + 1);
        }
        return true;
    }

    uint64_t* collapse_to_compact(uint64_t* node) {
        uint8_t  cL[MAX_COLLAPSE_ENTRIES];
        uint8_t  cF[MAX_COLLAPSE_ENTRIES];
        ks_offset_type cO[MAX_COLLAPSE_ENTRIES];
        uint8_t  cblob[COMPACT_KEYSUFFIX_LIMIT];
        typename slots_type::value_base_t cvals[slots_type::value_array_size(MAX_COLLAPSE_ENTRIES)];

        uint16_t ei = 0;
        uint16_t blob_cursor = 0;
        int chain_start = -1;

        hdr_type h = hdr_type::from_node(node);

        const uint8_t* skip_data = nullptr;
        uint8_t skip_len = h.skip;
        if (h.has_skip()) skip_data = hdr_type::get_skip(node, h);

        // Handle top-level skip: becomes prefix for all entries
        uint8_t top_prefix[256];
        uint32_t top_prefix_len = 0;
        if (h.has_skip()) {
            std::memcpy(top_prefix, skip_data, skip_len);
            top_prefix_len = skip_len;
        }

        // For top-level bitmask: walk without the skip (it stays as node skip)
        // The skip belongs to the collapsed node, not to entries.
        // So we pass no prefix and let the bitmask walk add dispatch bytes.
        bool ok;
        if (h.is_compact()) {
            // Collapsing a single compact node — just shrink
            ok = fill_layout(node, h, nullptr, 0,
                             cL, cF, cO, cblob, cvals,
                             ei, blob_cursor, chain_start);
        } else {
            // Bitmask: walk children. Skip stays on the collapsed node.
            ok = fill_layout(node, h, nullptr, 0,
                             cL, cF, cO, cblob, cvals,
                             ei, blob_cursor, chain_start);
        }

        if (!ok) {
            // Keysuffix > COMPACT_KEYSUFFIX_LIMIT: collapse would produce the same bitmask structure.
            // Do nothing — trie is already correct.
            return node;
        }

        if (ei == 0) {
            free_subtree_nodes(node);
            return compact_type::sentinel();
        }

        // Alloc exact-sized node, memcpy in
        uint64_t* nn = compact_type::alloc_compact_ks(
            mem_v, skip_len, skip_data, ei, blob_cursor);
        hdr_type& nh = hdr_type::from_node(nn);

        std::memcpy(compact_type::lengths(nn, nh), cL, ei);
        std::memcpy(compact_type::firsts(nn, nh),  cF, ei);
        std::memcpy(compact_type::offsets(nn, nh), cO, ei * sizeof(ks_offset_type));
        if (blob_cursor > 0)
            std::memcpy(compact_type::keysuffix(nn, nh), cblob, blob_cursor);

        auto* nsb = nh.get_compact_slots(nn);
        slots_type::copy_values(nsb, 0, cvals, 0, ei);

        compact_type::get_prefix(nn, nh).keysuffix_used = blob_cursor;
        nh.count = ei;
        hdr_type::from_node(nn).count = ei;

        free_subtree_nodes(node);
        return nn;
    }

    erase_info erase_node(uint64_t* node, const uint8_t* key,
                          uint32_t key_len, uint32_t consumed) {
        hdr_type h = hdr_type::from_node(node);

        auto mr = skip_type::match_prefix(node, h, key, key_len, consumed);
        if (mr.status != skip_type::match_status::MATCHED)
            return {0, erase_status::MISSING, nullptr, 0};
        consumed = mr.consumed;

        // COMPACT: return PENDING with found position
        if (h.is_compact()) {
            auto [found, pos] = compact_type::find_pos(
                node, h, key + consumed, key_len - consumed);
            if (!found)
                return {0, erase_status::MISSING, nullptr, 0};
            return {static_cast<uint32_t>(h.count - 1),
                    erase_status::PENDING, node, pos};
        }

        // BITMASK

        // EOS case
        if (consumed == key_len) {
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos == compact_type::sentinel()) return {0, erase_status::MISSING, nullptr, 0};
            uint64_t old_eos_tail = node_tail_total(eos);
            // Recurse into eos child
            erase_info r = erase_node(eos, key, key_len, consumed);
            if (r.status == erase_status::MISSING) return r;
            // Handle result
            uint64_t* new_eos = eos;
            if (r.status == erase_status::PENDING) {
                do_leaf_erase(r.leaf, r.pos);
                hdr_type eh = hdr_type::from_node(r.leaf);
                if (eh.count == 0) {
                    mem_v.free_node(r.leaf);
                    bitmask_type::set_eos_child(node, h, compact_type::sentinel());
                    new_eos = compact_type::sentinel();
                } else {
                    bitmask_type::set_eos_child(node, h, r.leaf);
                    new_eos = r.leaf;
                }
            } else {
                new_eos = r.leaf;
                if (r.leaf != eos)
                    bitmask_type::set_eos_child(node, h, r.leaf);
            }
            node[NODE_TOTAL_TAIL] -= old_eos_tail - node_tail_total(new_eos)
                                  + h.skip_bytes();
            // Check if bitmask is empty
            if (h.count == 0 && !bitmask_type::has_eos(node, h)) {
                mem_v.free_node(node);
                return {0, erase_status::DONE, compact_type::sentinel(), 0};
            }
            // Try collapse
            if (node[NODE_TOTAL_TAIL] <= COMPACT_KEYSUFFIX_LIMIT) {
                uint64_t* collapsed = collapse_to_compact(node);
                if (collapsed != node)
                    return {0, erase_status::DONE, collapsed, 0};
            }
            return {0, erase_status::DONE, node, 0};
        }

        // Child lookup
        uint8_t byte = key[consumed++];
        uint64_t* child = bitmask_type::dispatch(node, h, byte);
        if (child == compact_type::sentinel()) return {0, erase_status::MISSING, nullptr, 0};

        uint64_t old_child_tail = node_tail_total(child);
        erase_info r = erase_node(child, key, key_len, consumed);

        if (r.status == erase_status::MISSING)
            return r;

        if (r.status == erase_status::DONE) {
            uint64_t* new_child = r.leaf;
            if (new_child != child) {
                if (new_child != compact_type::sentinel())
                    bitmask_type::replace_child(node, h, byte, new_child);
                else
                    node = bitmask_type::remove_child(node, h, byte);
            }
            node[NODE_TOTAL_TAIL] -= old_child_tail - node_tail_total(new_child)
                                  + h.skip_bytes();
            // Empty check
            h = hdr_type::from_node(node);
            if (h.count == 0 && !bitmask_type::has_eos(node, h)) {
                mem_v.free_node(node);
                return {0, erase_status::DONE, compact_type::sentinel(), 0};
            }
            // Try collapse
            if (node[NODE_TOTAL_TAIL] <= COMPACT_KEYSUFFIX_LIMIT) {
                uint64_t* collapsed = collapse_to_compact(node);
                if (collapsed != node)
                    return {0, erase_status::DONE, collapsed, 0};
            }
            return {0, erase_status::DONE, node, 0};
        }

        // PENDING from child: r.leaf is the compact leaf to erase from.
        // Check if this is the last entry in the entire subtree.
        // If count==1, eos==sentinel, and the child has 1 entry, propagate PENDING.
        if (h.count == 1 && !bitmask_type::has_eos(node, h)) {
            hdr_type ch = hdr_type::from_node(r.leaf);
            if (ch.count == 1) {
                // Last entry — propagate PENDING, caller handles the erase.
                return {0, erase_status::PENDING, r.leaf, r.pos};
            }
        }

        // Do the leaf erase.
        uint64_t old_leaf_tail = node_tail_total(r.leaf);
        do_leaf_erase(r.leaf, r.pos);
        uint64_t new_leaf_tail = node_tail_total(r.leaf);

        hdr_type ch = hdr_type::from_node(r.leaf);
        if (ch.count == 0) {
            mem_v.free_node(r.leaf);
            node = bitmask_type::remove_child(node, h, byte);
        } else if (r.leaf != child) {
            bitmask_type::replace_child(node, h, byte, r.leaf);
        }
        node[NODE_TOTAL_TAIL] -= old_leaf_tail - new_leaf_tail
                              + h.skip_bytes();

        // Empty check
        h = hdr_type::from_node(node);
        if (h.count == 0 && !bitmask_type::has_eos(node, h)) {
            mem_v.free_node(node);
            return {0, erase_status::DONE, compact_type::sentinel(), 0};
        }
        // Try collapse
        if (node[NODE_TOTAL_TAIL] <= COMPACT_KEYSUFFIX_LIMIT) {
            uint64_t* collapsed = collapse_to_compact(node);
            if (collapsed != node)
                return {0, erase_status::DONE, collapsed, 0};
        }
        return {0, erase_status::DONE, node, 0};
    }


    // ------------------------------------------------------------------
    // Insert internals
    // ------------------------------------------------------------------

    bool modify_impl(std::string_view key, const VALUE& value,
                     insert_mode mode) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        insert_result r = insert_node(root_v, mapped, len, value, 0, mode);
        set_root(r.node);

        if (r.outcome == insert_outcome::INSERTED) {
            size_v++;
            return true;
        }
        if (r.outcome == insert_outcome::UPDATED)
            return true;
        return false;
    }

public:
    // insert_for_iter: like modify_impl but returns insert_result with leaf+pos
    // for iterator construction. Re-finds via find_leaf_pos when leaf is null
    // (split/rebuild/promote paths).
    insert_result insert_for_iter(std::string_view key, const VALUE& value,
                                   insert_mode mode) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(key.data());
        uint32_t len = static_cast<uint32_t>(key.size());

        uint8_t stack_buf[256];
        auto [mapped, raw_buf] = get_mapped<CHARMAP>(raw, len,
                                              stack_buf, sizeof(stack_buf));
        std::unique_ptr<uint8_t[]> heap_guard(raw_buf);

        insert_result r = insert_node(root_v, mapped, len, value, 0, mode);
        set_root(r.node);

        if (r.outcome == insert_outcome::INSERTED)
            size_v++;

        // Re-find leaf+pos if not propagated (split/rebuild/promote)
        if (!r.leaf && r.outcome != insert_outcome::FOUND) {
            auto f = find_leaf_pos(root_v, mapped, len, 0);
            r.leaf = f.leaf;
            r.pos  = f.pos;
        }
        return r;
    }

private:
    insert_result insert_node(uint64_t* node, const uint8_t* key_data,
                               uint32_t key_len, const VALUE& value,
                               uint32_t consumed, insert_mode mode) {
        hdr_type h = hdr_type::from_node(node);

        auto mr = skip_type::match_prefix(node, h, key_data, key_len, consumed);

        if (h.is_compact())
            return compact_type::insert(node, h, key_data, key_len,
                                         value, consumed, mr, mode, mem_v);

        // --- Bitmask cases ---

        if (mr.status == skip_type::match_status::MISMATCH) {
            if (mode == insert_mode::ASSIGN)
                return {node, insert_outcome::FOUND};

            const uint8_t* skip_data = hdr_type::get_skip(node, h);
            uint32_t old_skip = h.skip_bytes();
            uint32_t match_len = mr.match_len;

            uint8_t skip_copy[256];
            std::memcpy(skip_copy, skip_data, old_skip);

            uint8_t old_byte = skip_copy[match_len];
            uint8_t new_byte = key_data[consumed + match_len];

            uint32_t new_old_skip = old_skip - match_len - 1;
            uint64_t* old_reskipped = bitmask_type::reskip(
                node, h, mem_v, static_cast<uint8_t>(new_old_skip),
                skip_copy + match_len + 1);

            uint32_t new_consumed = consumed + match_len + 1;
            uint64_t* leaf = add_child(key_data + new_consumed,
                                       key_len - new_consumed, value);

            uint8_t bucket_idx[2];
            uint64_t* children[2];
            if (old_byte < new_byte) {
                bucket_idx[0] = old_byte;  bucket_idx[1] = new_byte;
                children[0] = old_reskipped; children[1] = leaf;
            } else {
                bucket_idx[0] = new_byte;  bucket_idx[1] = old_byte;
                children[0] = leaf;        children[1] = old_reskipped;
            }
            uint64_t* parent = bitmask_type::create_with_children(
                mem_v, static_cast<uint8_t>(match_len), skip_copy,
                bucket_idx, children, 2);
            parent[NODE_TOTAL_TAIL] = node_tail_total(old_reskipped)
                                   + node_tail_total(leaf);

            return {parent, insert_outcome::INSERTED};
        }

        if (mr.status == skip_type::match_status::KEY_EXHAUSTED) {
            if (mode == insert_mode::ASSIGN)
                return {node, insert_outcome::FOUND};

            const uint8_t* skip_data = hdr_type::get_skip(node, h);
            uint32_t old_skip = h.skip_bytes();
            uint32_t match_len = mr.match_len;

            uint8_t skip_copy[256];
            std::memcpy(skip_copy, skip_data, old_skip);

            uint8_t old_byte = skip_copy[match_len];

            uint32_t new_old_skip = old_skip - match_len - 1;
            uint64_t* old_reskipped = bitmask_type::reskip(
                node, h, mem_v, static_cast<uint8_t>(new_old_skip),
                skip_copy + match_len + 1);

            uint8_t bucket_idx[1] = {old_byte};
            uint64_t* children[1] = {old_reskipped};
            uint64_t* parent = bitmask_type::create_with_children(
                mem_v, static_cast<uint8_t>(match_len), skip_copy,
                bucket_idx, children, 1);

            parent[NODE_TOTAL_TAIL] = node_tail_total(old_reskipped);

            // Create compact(1) with EOS value, set as eos_child
            uint64_t eos_raw = slots_type::make_raw(value, mem_v.alloc_v);
            typename compact_type::build_entry eos_be{nullptr, 0, eos_raw};
            uint64_t* eos_node = compact_type::build_compact(mem_v, 0, nullptr, &eos_be, 1);
            bitmask_type::set_eos_child(parent, hdr_type::from_node(parent), eos_node);
            // EOS node has keysuffix_used=0, no tail delta

            return {parent, insert_outcome::INSERTED};
        }

        // MATCHED
        consumed = mr.consumed;

        if (consumed == key_len) {
            // EOS at bitmask node
            uint64_t* eos = bitmask_type::eos_child(node, h);
            if (eos != compact_type::sentinel()) {
                // Recurse into existing EOS node
                uint64_t old_et = node_tail_total(eos);
                insert_result r = insert_node(eos, key_data, key_len, value, consumed, mode);
                if (r.node != eos || r.outcome == insert_outcome::INSERTED)
                    bitmask_type::set_eos_child(node, h, r.node);
                node[NODE_TOTAL_TAIL] += node_tail_total(r.node) - old_et;
                if (r.outcome == insert_outcome::INSERTED)
                    node[NODE_TOTAL_TAIL] += h.skip_bytes();
                return {node, r.outcome, r.leaf, r.pos};
            }
            if (mode == insert_mode::ASSIGN) return {node, insert_outcome::FOUND};
            // Create compact(1) with EOS entry
            uint64_t raw = slots_type::make_raw(value, mem_v.alloc_v);
            typename compact_type::build_entry be{nullptr, 0, raw};
            uint64_t* eos_node = compact_type::build_compact(mem_v, 0, nullptr, &be, 1);
            bitmask_type::set_eos_child(node, h, eos_node);
            node[NODE_TOTAL_TAIL] += node_tail_total(eos_node) + h.skip_bytes();
            return {node, insert_outcome::INSERTED};
        }

        {
            uint8_t byte = key_data[consumed++];
            uint64_t* child = bitmask_type::dispatch(node, h, byte);
            if (child == compact_type::sentinel()) {
                if (mode == insert_mode::ASSIGN) return {node, insert_outcome::FOUND};
                uint64_t* new_child = add_child(key_data + consumed,
                                                 key_len - consumed, value);
                node = bitmask_type::insert_child(node, h, mem_v, byte, new_child);
                node[NODE_TOTAL_TAIL] += node_tail_total(new_child) + h.skip_bytes();
                return {node, insert_outcome::INSERTED};
            }
            // Recurse
            uint64_t old_ct = node_tail_total(child);
            insert_result r = insert_node(child, key_data, key_len, value,
                                           consumed, mode);
            // Always re-store + re-link on INSERTED: the recursive call may
            // free the old child and the allocator can reuse the same address
            // for a completely different node, making r.node == child even
            // though the node was rebuilt. Address comparison is unsound.
            if (r.node != child || r.outcome == insert_outcome::INSERTED)
                bitmask_type::replace_child(node, h, byte, r.node);
            node[NODE_TOTAL_TAIL] += node_tail_total(r.node) - old_ct;
            if (r.outcome == insert_outcome::INSERTED)
                node[NODE_TOTAL_TAIL] += h.skip_bytes();
            return {node, r.outcome, r.leaf, r.pos};
        }
    }

    uint64_t* add_child(const uint8_t* suffix, uint32_t suffix_len,
                        const VALUE& value) {
        if (suffix_len > hdr_type::SKIP_MAX) {
            constexpr uint32_t step = hdr_type::SKIP_MAX;
            uint8_t dispatch = suffix[step];
            uint64_t* child = add_child(suffix + step + 1,
                                         suffix_len - step - 1, value);
            uint64_t* bm_node = bitmask_type::create_with_children(
                mem_v, static_cast<uint8_t>(step), suffix,
                &dispatch, &child, 1);
            bm_node[NODE_TOTAL_TAIL] = node_tail_total(child) + step;
            return bm_node;
        }
        uint64_t raw = slots_type::make_raw(value, mem_v.alloc_v);
        typename compact_type::build_entry be{nullptr, 0, raw};
        return compact_type::build_compact(mem_v,
            static_cast<uint8_t>(suffix_len), suffix, &be, 1);
    }

    struct child_entry {
        const uint8_t* suffix;
        uint32_t       suffix_len;
        const VALUE*   value;
    };

    uint64_t* add_children(const child_entry* entries, size_t count) {
        if (count == 0)
            return mem_v.alloc_node(1);

        using be = typename compact_type::build_entry;
        auto arr = std::make_unique<be[]>(count);
        for (size_t i = 0; i < count; ++i) {
            uint64_t raw = slots_type::make_raw(*entries[i].value, mem_v.alloc_v);
            arr[i].key      = entries[i].suffix;
            arr[i].key_len  = entries[i].suffix_len;
            arr[i].raw_slot = raw;
        }
        uint64_t* node = compact_type::build_compact(
            mem_v, 0, nullptr, arr.get(), static_cast<uint16_t>(count));
        return node;
    }
};

} // namespace gteitelbaum::kstrie_detail

#endif // KSTRIE_IMPL_HPP
