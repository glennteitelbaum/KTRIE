#ifndef KSTRIE_BITMASK_HPP
#define KSTRIE_BITMASK_HPP

#include "kstrie_support.hpp"

namespace gteitelbaum::kstrie_detail {

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_bitmask {
    using hdr_type    = node_header<VALUE, CHARMAP, ALLOC>;
    using bitmap_type = typename CHARMAP::bitmap_type;
    using slots       = kstrie_slots<VALUE>;
    using vbase_t     = typename slots::value_base_t;
    using mem_type    = kstrie_memory<ALLOC>;
    using compact_type = kstrie_compact<VALUE, CHARMAP, ALLOC>;

    static constexpr size_t BITMAP_BYTES = sizeof(bitmap_type);
    static constexpr size_t BITMAP_WORDS = CHARMAP::BITMAP_WORDS;
    static constexpr size_t BITMAP_U64   = BITMAP_BYTES / U64_BYTES;

    // ------------------------------------------------------------------
    // Layout:
    //   [header 8B][desc 8B][bitmap BU*8B][SENTINEL ptr 8B]
    //   [child_ptrs Nc*8B][eos_ptr 8B][skip bytes padded to 8B]
    // ------------------------------------------------------------------

    static constexpr size_t CHILD_BITMAP_OFF = 2;                           // u64 units (after header + desc)
    static constexpr size_t SENTINEL_OFF     = CHILD_BITMAP_OFF + BITMAP_U64; // after bitmap
    static constexpr size_t CHILD_SLOTS_OFF  = SENTINEL_OFF + 1;             // after sentinel

    // index_size (write-path)
    static constexpr size_t index_size(const hdr_type&) noexcept {
        return BITMAP_BYTES;  // just child bitmap
    }

    // --- Child bitmap (fixed offset) ---

    static constexpr size_t BITMAP_BYTE_OFF = 2 * U64_BYTES;

    static bitmap_type* get_bitmap(uint64_t* node, const hdr_type& /*h*/) noexcept {
        return reinterpret_cast<bitmap_type*>(reinterpret_cast<uint8_t*>(node) + BITMAP_BYTE_OFF);
    }
    static const bitmap_type* get_bitmap(const uint64_t* node, const hdr_type& /*h*/) noexcept {
        return reinterpret_cast<const bitmap_type*>(
            reinterpret_cast<const uint8_t*>(node) + BITMAP_BYTE_OFF);
    }

    // --- Slot region accessors ---

    static uint64_t* child_slots(uint64_t* node) noexcept { return node + CHILD_SLOTS_OFF; }
    static const uint64_t* child_slots(const uint64_t* node) noexcept { return node + CHILD_SLOTS_OFF; }

    // Slots base: points to sentinel (slot 0). dispatch indexes from here.
    static const uint64_t* slots_base(const uint64_t* node) noexcept {
        return node + SENTINEL_OFF;
    }

    // Sentinel slot (fixed offset)
    static uint64_t* sentinel_slot(uint64_t* node) noexcept {
        return node + SENTINEL_OFF;
    }

    // eos_ptr: after last child
    static uint64_t* eos_child_ptr(uint64_t* node, const hdr_type& h) noexcept {
        return node + CHILD_SLOTS_OFF + h.count;
    }
    static uint64_t* eos_child(const uint64_t* node, const hdr_type& h) noexcept {
        return slots::load_child(node + CHILD_SLOTS_OFF + h.count, 0);
    }

    // Set/get EOS child
    static void set_eos_child(uint64_t* node, const hdr_type& h,
                               uint64_t* eos_node) noexcept {
        slots::store_child(eos_child_ptr(node, h), 0, eos_node);
    }

    static bool has_eos(const uint64_t* node, const hdr_type& h) noexcept {
        return eos_child(node, h) != compact_type::sentinel();
    }

    // skip: after eos_ptr
    static uint8_t* get_bitmask_skip(uint64_t* node, const hdr_type& h) noexcept {
        return reinterpret_cast<uint8_t*>(node + CHILD_SLOTS_OFF + h.count + 1);
    }
    static const uint8_t* get_bitmask_skip(const uint64_t* node, const hdr_type& h) noexcept {
        return reinterpret_cast<const uint8_t*>(node + CHILD_SLOTS_OFF + h.count + 1);
    }

    // ------------------------------------------------------------------
    // Bitmap probe — branchless popcount
    // ------------------------------------------------------------------

    static int do_find_pop(const uint64_t* search, uint8_t v) noexcept {
        constexpr size_t W = BITMAP_WORDS;
        const int word = v >> 6;
        const int bit  = v & 63;
        uint64_t before = search[word] << (63 - bit);
        int pc0 = 0, pc1 = 0, pc2 = 0;
        if constexpr (W > 1) pc0 = std::popcount(search[0]);
        if constexpr (W > 2) pc1 = std::popcount(search[1]);
        if constexpr (W > 3) pc2 = std::popcount(search[2]);
        int count = std::popcount(before);
        if constexpr (W > 1) count += pc0 & -int(word > 0);
        if constexpr (W > 2) count += pc1 & -int(word > 1);
        if constexpr (W > 3) count += pc2 & -int(word > 2);
        bool found = before & (1ULL << 63);
        count &= -uint64_t(found);
        return count;
    }

    // ------------------------------------------------------------------
    // Branchless dispatch: returns SENTINEL on miss, child on hit.
    // Read path: just follow the pointer.
    // Write path: compare result to sentinel to detect miss.
    // ------------------------------------------------------------------

    static uint64_t* dispatch(const uint64_t* node, const hdr_type& /*h*/,
                               uint8_t idx) noexcept {
        const uint64_t* sb = slots_base(node);
        int rank = do_find_pop(node + CHILD_BITMAP_OFF, idx);
        return slots::load_child(sb, rank);  // rank=0 → sentinel, rank>0 → child
    }

    // child_by_slot: 0-based iteration over real children (past sentinel).
    // Used by insert_child/remove_child/iteration.
    static uint64_t* child_by_slot(const uint64_t* node, const hdr_type& /*h*/,
                                    uint16_t slot) noexcept {
        return slots::load_child(node + CHILD_SLOTS_OFF, slot);
    }

    // replace_child: bitmap lookup via count_below + store
    static void replace_child(uint64_t* node, const hdr_type& h,
                              uint8_t idx, uint64_t* new_child) noexcept {
        const bitmap_type* bm = get_bitmap(node, h);
        assert(bm->has_bit(idx));
        int slot = bm->count_below(idx);
        slots::store_child(child_slots(node), slot, new_child);
    }

    // ------------------------------------------------------------------
    // needed_u64
    // ------------------------------------------------------------------

    static constexpr size_t needed_u64(uint8_t skip_len,
                                        uint16_t child_count) noexcept {
        // header(1) + desc(1) + bitmap(BU) + sentinel(1) + children(Nc) + eos(1) + skip
        size_t u64s = 2 + BITMAP_U64 + 1 + child_count + 1;
        if (skip_len > 0)
            u64s += div_ceil(align_up(static_cast<size_t>(skip_len), U64_BYTES),
                             U64_BYTES);
        return u64s;
    }

    // ------------------------------------------------------------------
    // Create / mutate
    // ------------------------------------------------------------------

    static uint64_t* create(mem_type& mem, uint8_t skip_len,
                             const uint8_t* skip_data) {
        size_t nu = needed_u64(skip_len, 0);
        uint64_t* node = mem.alloc_node(nu);
        hdr_type& h = hdr_type::from_node(node);
        h.set_bitmask(true);
        h.set_skip_len(skip_len);
        h.count = 0;
        node[NODE_TOTAL_TAIL] = 0;
        // Set sentinel
        slots::store_child(node + SENTINEL_OFF, 0, compact_type::sentinel());
        // Set eos to sentinel
        slots::store_child(eos_child_ptr(node, h), 0, compact_type::sentinel());
        if (h.has_skip()) [[unlikely]]
            std::memcpy(get_bitmask_skip(node, h), skip_data, skip_len);
        return node;
    }

    static uint64_t* create_with_children(
            mem_type& mem,
            uint8_t skip_len, const uint8_t* skip_data,
            const uint8_t* bucket_idx, uint64_t* const* bucket_child,
            uint16_t n_buckets) {
        size_t nu = needed_u64(skip_len, n_buckets);
        uint64_t* node = mem.alloc_node(nu);
        hdr_type& h = hdr_type::from_node(node);
        h.set_bitmask(true);
        h.set_skip_len(skip_len);
        h.count = n_buckets;
        node[NODE_TOTAL_TAIL] = 0;
        // Sentinel
        slots::store_child(node + SENTINEL_OFF, 0, compact_type::sentinel());
        // Bitmap + children
        bitmap_type* bm = get_bitmap(node, h);
        uint64_t* cs = child_slots(node);
        for (uint16_t i = 0; i < n_buckets; ++i) {
            bm->set_bit(bucket_idx[i]);
            slots::store_child(cs, i, bucket_child[i]);
        }
        // EOS = sentinel
        slots::store_child(eos_child_ptr(node, h), 0, compact_type::sentinel());
        // Skip
        if (h.has_skip()) [[unlikely]]
            std::memcpy(get_bitmask_skip(node, h), skip_data, skip_len);
        return node;
    }

    // ------------------------------------------------------------------
    // insert_child
    // ------------------------------------------------------------------

    static uint64_t* insert_child(uint64_t* node, hdr_type& h, mem_type& mem,
                                    uint8_t idx, uint64_t* child) {
        bitmap_type* bm = get_bitmap(node, h);
        int pos = bm->slot_for_insert(idx);
        uint16_t old_cc = h.count;
        uint16_t new_cc = old_cc + 1;

        // Save eos_ptr and skip before shifting
        uint64_t* old_eos = eos_child(node, h);
        uint8_t skip_buf[hdr_type::SKIP_MAX];
        uint32_t slen = 0;
        if (h.has_skip()) {
            slen = h.skip_bytes();
            std::memcpy(skip_buf, get_bitmask_skip(node, h), slen);
        }

        size_t new_nu = needed_u64(h.skip, new_cc);
        if (new_nu <= h.alloc_u64) {
            uint64_t* cs = child_slots(node);
            int shift = old_cc - pos;
            if (shift > 0) slots::move_children(cs, pos + 1, cs, pos, shift);
            slots::store_child(cs, pos, child);
            bm->set_bit(idx);
            h.count = new_cc;
            hdr_type::from_node(node).count = new_cc;
            // Restore eos + skip at new positions
            slots::store_child(eos_child_ptr(node, h), 0, old_eos);
            if (slen > 0) std::memcpy(get_bitmask_skip(node, h), skip_buf, slen);
            return node;
        }

        // Realloc
        uint64_t* nn = mem.alloc_node(new_nu);
        hdr_type& nh = hdr_type::from_node(nn);
        nh.copy_from(h);
        nh.count = new_cc;
        nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];
        slots::store_child(nn + SENTINEL_OFF, 0, compact_type::sentinel());
        *get_bitmap(nn, nh) = *bm;
        get_bitmap(nn, nh)->set_bit(idx);
        uint64_t* new_cs = child_slots(nn);
        const uint64_t* old_cs = child_slots(node);
        if (pos > 0) slots::copy_children(new_cs, 0, old_cs, 0, pos);
        slots::store_child(new_cs, pos, child);
        int after = old_cc - pos;
        if (after > 0) slots::copy_children(new_cs, pos + 1, old_cs, pos, after);
        slots::store_child(eos_child_ptr(nn, nh), 0, old_eos);
        if (slen > 0) std::memcpy(get_bitmask_skip(nn, nh), skip_buf, slen);
        mem.free_node(node);
        return nn;
    }

    // ------------------------------------------------------------------
    // remove_child
    // ------------------------------------------------------------------

    static uint64_t* remove_child(uint64_t* node, hdr_type& h,
                                    uint8_t idx) noexcept {
        bitmap_type* bm = get_bitmap(node, h);
        assert(bm->has_bit(idx));
        int pos = bm->count_below(idx);
        // Save eos + skip
        uint64_t* old_eos = eos_child(node, h);
        uint8_t skip_buf[hdr_type::SKIP_MAX];
        uint32_t slen = 0;
        if (h.has_skip()) {
            slen = h.skip_bytes();
            std::memcpy(skip_buf, get_bitmask_skip(node, h), slen);
        }
        uint16_t old_cc = h.count;
        uint64_t* cs = child_slots(node);
        int after = old_cc - pos - 1;
        if (after > 0) slots::move_children(cs, pos, cs, pos + 1, after);
        bm->clear_bit(idx);
        h.count = old_cc - 1;
        hdr_type::from_node(node).count = h.count;
        // Restore eos + skip at new positions
        slots::store_child(eos_child_ptr(node, h), 0, old_eos);
        if (slen > 0) std::memcpy(get_bitmask_skip(node, h), skip_buf, slen);
        return node;
    }

    // ------------------------------------------------------------------
    // reskip
    // ------------------------------------------------------------------

    static uint64_t* reskip(uint64_t* node, hdr_type& h, mem_type& mem,
                              uint8_t new_skip_len, const uint8_t* new_skip_data) {
        size_t nu = needed_u64(new_skip_len, h.count);
        uint64_t* nn = mem.alloc_node(nu);
        hdr_type& nh = hdr_type::from_node(nn);
        nh.copy_from(h);
        nh.set_skip_len(new_skip_len);
        nh.count = h.count;
        nn[NODE_TOTAL_TAIL] = node[NODE_TOTAL_TAIL];
        slots::store_child(nn + SENTINEL_OFF, 0, compact_type::sentinel());
        std::memcpy(nn + CHILD_BITMAP_OFF, node + CHILD_BITMAP_OFF, BITMAP_BYTES);
        slots::copy_children(child_slots(nn), 0, child_slots(node), 0, h.count);
        slots::store_child(eos_child_ptr(nn, nh), 0, eos_child(node, h));
        if (nh.has_skip()) [[unlikely]]
            std::memcpy(get_bitmask_skip(nn, nh), new_skip_data, new_skip_len);
        mem.free_node(node);
        return nn;
    }

};

} // namespace gteitelbaum::kstrie_detail

#endif // KSTRIE_BITMASK_HPP
