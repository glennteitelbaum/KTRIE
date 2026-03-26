#ifndef KSTRIE_COMPACT_HPP
#define KSTRIE_COMPACT_HPP
#include "kstrie_support.hpp"
#include <algorithm>

namespace gteitelbaum::kstrie_detail {

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_bitmask;

// ============================================================================
// kstrie_compact -- compact (leaf) node operations
//
// Node layout:  [header 8B][ck_prefix 8B][L][F][O][keysuffix region][value slots]
// Skip bytes are stored at the byte offset recorded in ck_prefix.skip_data_off.
//
// L[]       : u8, full suffix length (0 = empty suffix / exact prefix match)
// F[]       : u8, suffix[0]; 0 for empty suffix
// O[]       : u16, start of suffix tail (suffix[1..]) in keysuffix region
// keysuffix : suffix[1..len-1], compacted on erase (chain head / standalone)
// values    : u64 slots at slots_off * 8
//
// Sort order: lexicographic (F[i] primary, then tail bytes, shorter key first on tie).
// Search: single binary search. O(log N).
//
// Invariants (checked only on insert or prefix-shrink):
//   keysuffix_used <= COMPACT_KEYSUFFIX_LIMIT
//   every suffix length <= 255
// Either violation triggers split_node -> bitmask promotion.
//
// VALUE ownership:
//   - T* created exactly once on insert (via slots::store_value)
//   - Moved between nodes by raw uint64_t memcpy
//   - Deleted only on erase or tree destruction
// ============================================================================

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_compact {
    using hdr_type    = node_header<VALUE, CHARMAP, ALLOC>;
    using slots       = kstrie_slots<VALUE>;
    using mem_type    = kstrie_memory<ALLOC>;
    using skip_type   = kstrie_skip<VALUE, CHARMAP, ALLOC>;
    using bitmask_ops = kstrie_bitmask<VALUE, CHARMAP, ALLOC>;
    using match_result = typename skip_type::match_result;
    using match_status = typename skip_type::match_status;

    // Sentinel: all-zeros compact(0). Static, never freed (alloc_u64=0).
    // 4 u64s: header + ck_prefix + parent_ptr + start of L array — all zero-safe.
    static inline constinit uint64_t sentinel_data_[4] = {};
    static uint64_t* sentinel() noexcept { return sentinel_data_; }

    // Compute a subtree's total-tail contribution (keysuffix cost).
    // Replicates kstrie_impl::node_tail_total as a static for use in compact.
    static uint64_t subtree_tail(const uint64_t* node) noexcept {
        if (!node || node == sentinel()) return 0;
        hdr_type h = hdr_type::from_node(node);
        if (h.is_bitmap()) return node[NODE_TOTAL_TAIL];
        const auto& p = get_prefix(node, h);
        return p.keysuffix_used
             + static_cast<uint64_t>(h.count) * (1 + h.skip_bytes());
    }

    // build_entry: used by split_node, collect_entries, build_compact, kstrie_impl.
    // key points to full suffix (first byte included); key_len is full suffix length.
    struct build_entry {
        const uint8_t* key;
        uint32_t       key_len;
        uint64_t       raw_slot;
    };

    // ------------------------------------------------------------------
    // Compact prefix (immediately after header + skip pad)
    // ------------------------------------------------------------------

    struct ck_prefix {
        uint16_t cap;
        uint16_t keysuffix_used;
        uint16_t skip_data_off;   // byte offset from node start to skip bytes
        uint16_t parent_byte;     // dispatch byte in parent (ROOT_PARENT_BYTE = root)
    };

    static constexpr size_t CK_PREFIX_OFF = U64_BYTES;  // byte offset: right after header

    static ck_prefix& get_prefix(uint64_t* node, const hdr_type& /*h*/) noexcept {
        return *reinterpret_cast<ck_prefix*>(reinterpret_cast<uint8_t*>(node) + CK_PREFIX_OFF);
    }
    static const ck_prefix& get_prefix(const uint64_t* node,
                                        const hdr_type& h) noexcept {
        return get_prefix(const_cast<uint64_t*>(node), h);
    }

    // ------------------------------------------------------------------
    // Parent pointer: node[2] for compact nodes
    // ------------------------------------------------------------------

    static constexpr size_t COMPACT_PARENT_PTR = 2;  // u64 offset

    static uint64_t* get_parent(const uint64_t* node) noexcept {
        return reinterpret_cast<uint64_t*>(
            static_cast<std::uintptr_t>(node[COMPACT_PARENT_PTR]));
    }
    static void set_parent(uint64_t* node, uint64_t* parent) noexcept {
        node[COMPACT_PARENT_PTR] = static_cast<uint64_t>(
            reinterpret_cast<std::uintptr_t>(parent));
    }
    static uint16_t get_parent_byte(const uint64_t* node, const hdr_type& h) noexcept {
        return get_prefix(node, h).parent_byte;
    }
    static void set_parent_byte(uint64_t* node, const hdr_type& h, uint16_t byte) noexcept {
        get_prefix(node, h).parent_byte = byte;
    }

    // ------------------------------------------------------------------
    // Parallel array accessors
    // ------------------------------------------------------------------

    static uint8_t* lengths(uint64_t* node, const hdr_type& /*h*/) noexcept {
        return reinterpret_cast<uint8_t*>(node) + hdr_type::COMPACT_ARRAYS_OFF;
    }
    static const uint8_t* lengths(const uint64_t* node,
                                   const hdr_type& h) noexcept {
        return lengths(const_cast<uint64_t*>(node), h);
    }

    static uint8_t* firsts(uint64_t* node, const hdr_type& h) noexcept {
        return lengths(node, h) + get_prefix(node, h).cap;
    }
    static const uint8_t* firsts(const uint64_t* node,
                                  const hdr_type& h) noexcept {
        return firsts(const_cast<uint64_t*>(node), h);
    }

    static ks_offset_type* offsets(uint64_t* node, const hdr_type& h) noexcept {
        return reinterpret_cast<ks_offset_type*>(firsts(node, h) + get_prefix(node, h).cap);
    }
    static const ks_offset_type* offsets(const uint64_t* node,
                                          const hdr_type& h) noexcept {
        return offsets(const_cast<uint64_t*>(node), h);
    }

    static uint8_t* keysuffix(uint64_t* node, const hdr_type& h) noexcept {
        const ck_prefix& p = get_prefix(node, h);
        return reinterpret_cast<uint8_t*>(node) + p.skip_data_off + h.skip_bytes();
    }
    static const uint8_t* keysuffix(const uint64_t* node,
                                     const hdr_type& h) noexcept {
        return keysuffix(const_cast<uint64_t*>(node), h);
    }

    // ------------------------------------------------------------------
    // Sizing helpers
    // ------------------------------------------------------------------

    // All parallel arrays are padded to 8-entry blocks.
    // cap = align_up(count, 8). With COMPACT_ARRAYS_OFF = 24:
    //   lengths  at 16,          size = cap bytes
    //   firsts   at 16 + cap,    size = cap bytes
    //   offsets  at 16 + 2*cap,  size = cap * sizeof(ks_offset_type) bytes
    //   skip     at 16 + 2*cap + cap*sizeof(ks_offset_type)
    //   keysuffix after skip
    //   values   at slots_off, 8-aligned

    static constexpr size_t ARRAY_BLOCK = 8;  // array slot alignment

    static constexpr uint16_t array_cap(uint16_t count) noexcept {
        return static_cast<uint16_t>(align_up(std::max<size_t>(count, 1), ARRAY_BLOCK));
    }

    static constexpr size_t ck_offsets_off(uint16_t cap) noexcept {
        return hdr_type::COMPACT_ARRAYS_OFF + 2 * cap;  // 16-aligned by construction
    }

    static constexpr size_t ck_skip_off(uint16_t cap) noexcept {
        return ck_offsets_off(cap) + cap * sizeof(ks_offset_type);
    }

    static constexpr size_t ck_keysuffix_off(uint16_t cap, uint8_t skip_len) noexcept {
        return ck_skip_off(cap) + skip_len;
    }

    static constexpr size_t ck_values_off(uint16_t cap, uint8_t skip_len,
                                           uint32_t ks_bytes) noexcept {
        return align_up(ck_keysuffix_off(cap, skip_len) + ks_bytes, U64_BYTES);
    }

    static constexpr size_t ck_alloc_bytes(uint16_t cap, uint8_t skip_len,
                                            uint32_t ks_bytes) noexcept {
        return ck_values_off(cap, skip_len, ks_bytes)
             + slots::value_u64s(cap) * U64_BYTES;
    }

        static size_t ck_keysuffix_cap(const uint64_t* node, const hdr_type& h) noexcept {
        uint16_t cap = get_prefix(node, h).cap;
        size_t values_off = static_cast<size_t>(h.slots_off) * U64_BYTES;
        return values_off - ck_keysuffix_off(cap, h.skip_bytes());
    }

    static constexpr uint16_t compute_slots_off(uint16_t cap, uint8_t skip_len,
                                                 uint32_t ks_bytes) noexcept {
        return static_cast<uint16_t>(ck_values_off(cap, skip_len, ks_bytes) / U64_BYTES);
    }

    static bool should_shrink(uint16_t count, uint16_t cap) noexcept {
        return count < cap / 2;
    }

    // ------------------------------------------------------------------
    // index_size (called by node_header::index_size, write-path only)
    // ------------------------------------------------------------------

    static size_t index_size(const hdr_type& h) noexcept {
        return static_cast<size_t>(h.slots_off) * U64_BYTES - hdr_type::COMPACT_ARRAYS_OFF;
    }

    // ------------------------------------------------------------------
    // alloc_compact_ks -- allocate compact node for `count` entries.
    // cap = array_cap(count) — 8-aligned block size.
    // ------------------------------------------------------------------

    static uint64_t* alloc_compact_ks(mem_type& mem, uint8_t skip_len,
                                       const uint8_t* skip_data,
                                       uint16_t count, uint32_t ks_bytes) {
        uint16_t cap = array_cap(count);
        size_t nu = div_ceil(ck_alloc_bytes(cap, skip_len, ks_bytes), U64_BYTES);
        uint64_t* node = mem.alloc_node(nu);
        hdr_type& h = hdr_type::from_node(node);

        h.set_compact(true);
        h.set_skip_len(skip_len);
        h.count     = 0;
        h.slots_off = compute_slots_off(cap, skip_len, ks_bytes);

        ck_prefix& p = get_prefix(node, h);
        p.cap            = cap;
        p.keysuffix_used = 0;
        p.skip_data_off  = static_cast<uint16_t>(ck_skip_off(cap));
        p.parent_byte = ROOT_PARENT_BYTE;

        if (h.has_skip()) [[unlikely]]
            std::memcpy(reinterpret_cast<uint8_t*>(node) + p.skip_data_off,
                        skip_data, skip_len);
        return node;
    }

    // ------------------------------------------------------------------
    // find_pos -- binary search; returns {found, position/insertion point}
    // ------------------------------------------------------------------

    struct find_result { bool found; int pos; };

    static find_result find_pos(const uint64_t* node, const hdr_type& h,
                                const uint8_t* suffix,
                                uint32_t suffix_len) noexcept {
        const uint8_t*  L  = lengths(node, h);
        const uint8_t*  F  = firsts(node, h);
        const ks_offset_type* O = offsets(node, h);
        const uint8_t*  B = keysuffix(node, h);
        int e = h.count;

        bool has_entries = (e > 0);
        bool has_eos = (L[0] == 0);
        bool valid_eos = has_entries & has_eos;

        if (suffix_len == 0) [[unlikely]] {
            return {valid_eos, 0};
        }

        int            slen  = static_cast<int>(suffix_len);
        int            fb    = static_cast<int>(suffix[0]);
        int            stail = slen - 1;
        const uint8_t* K2    = suffix + 1;

        int lo = valid_eos;
        int hi = e;
        while (lo < hi) [[likely]] {
            int m  = lo + ((hi - lo) >> 1);
            int lm = static_cast<int>(L[m]);
            int c  = fb - static_cast<int>(F[m]);
            if (c == 0) [[unlikely]] {
                int mtail    = lm - 1;
                int min_tail = stail < mtail ? stail : mtail;
                c = std::memcmp(K2, B + O[m], min_tail);
                if (c == 0) [[unlikely]] {
                    c = slen - lm;
                    if (c == 0) [[unlikely]] return {true, m};
                }
            }
            if (c > 0) lo = m + 1; else hi = m;
        }
        return {false, lo};
    }

    // ------------------------------------------------------------------
    // find
    // ------------------------------------------------------------------

    static const VALUE* find(const uint64_t* node, const hdr_type& h,
                             const uint8_t* suffix,
                             uint32_t suffix_len) noexcept {
        auto [found, pos] = find_pos(node, h, suffix, suffix_len);
        if (!found) return nullptr;
        return slots::load_value(h.get_compact_slots(node), pos);
    }

    // ------------------------------------------------------------------
    // collect_entries -- walk parallel arrays into build_entry[].
    // key_buf must be at least N * 256 bytes.
    // ------------------------------------------------------------------

    static uint16_t collect_entries(const uint64_t* node, const hdr_type& h,
                                    build_entry* out,
                                    uint8_t* key_buf) noexcept {
        uint16_t N = h.count;
        if (N == 0) return 0;

        const uint8_t*  L  = lengths(node, h);
        const uint8_t*  F  = firsts(node, h);
        const ks_offset_type* O = offsets(node, h);
        const uint8_t*  B  = keysuffix(node, h);
        const auto* sb = h.get_compact_slots(node);

        size_t buf_off = 0;
        for (uint16_t i = 0; i < N; ++i) {
            uint8_t klen = L[i];
            uint8_t* dst = key_buf + buf_off;
            if (klen > 0) {
                dst[0] = F[i];
                if (klen > 1)
                    std::memcpy(dst + 1, B + O[i], klen - 1);
            }
            out[i].key      = dst;
            out[i].key_len  = klen;
            out[i].raw_slot = slots::load_raw(sb, i);
            buf_off += klen;
        }
        return N;
    }

    // ------------------------------------------------------------------
    // build_compact -- build a compact node from sorted build_entry array.
    // Entries must already be in lexicographic order (F[i], then tail bytes).
    // ------------------------------------------------------------------

    static uint64_t* build_compact(mem_type& mem,
                                    uint8_t skip_len,
                                    const uint8_t* skip_data,
                                    const build_entry* entries,
                                    uint16_t count) {
        // Compute exact tail bytes needed from entries.
        uint32_t actual_tail = 0;
        for (uint16_t i = 0; i < count; ++i)
            if (entries[i].key_len > 1)
                actual_tail += static_cast<uint32_t>(entries[i].key_len - 1);

        // Request exact count + exact ks. alloc_compact_ks derives cap from
        // padded allocation, giving free headroom from padded_size rounding.
        uint64_t* node = alloc_compact_ks(mem, skip_len, skip_data, count, actual_tail);
        hdr_type& h    = hdr_type::from_node(node);

        uint8_t*  L  = lengths(node, h);
        uint8_t*  F  = firsts(node, h);
        ks_offset_type* O = offsets(node, h);
        uint8_t*  B  = keysuffix(node, h);
        auto* sb = h.get_compact_slots(node);

        // Forward pass: populate L[], F[], values, keysuffix region with sharing.
        // Within a sharing chain (same F, each tail is prefix of next),
        // only the longest writes to the keysuffix region. Shorter entries share its O[].
        int chain_start = -1;
        uint16_t cursor = 0;

        for (uint16_t i = 0; i < count; ++i) {
            uint8_t klen = static_cast<uint8_t>(entries[i].key_len);
            uint8_t tail = klen > 0 ? klen - 1 : 0;
            L[i] = klen;
            F[i] = (klen > 0) ? entries[i].key[0] : 0;
            slots::store_raw(sb, i, entries[i].raw_slot);

            if (tail == 0) { O[i] = static_cast<ks_offset_type>(cursor); continue; }

            // Does this entry's tail share into the next entry's tail?
            bool shares_into_next = (i + 1 < count &&
                entries[i+1].key_len > klen &&
                entries[i+1].key[0] == entries[i].key[0] &&
                std::memcmp(entries[i].key + 1, entries[i+1].key + 1, tail) == 0);

            if (shares_into_next) {
                if (chain_start < 0) chain_start = static_cast<int>(i);
                continue;  // defer O[i] until longest is written
            }

            // Longest in chain (or standalone): write keysuffix bytes
            O[i] = static_cast<ks_offset_type>(cursor);
            std::memcpy(B + cursor, entries[i].key + 1, tail);

            // Set O[] for all deferred chain members
            if (chain_start >= 0) {
                for (int j = chain_start; j < static_cast<int>(i); ++j)
                    if (L[j] > 0) O[j] = O[i];
                chain_start = -1;
            }

            cursor += tail;
        }

        ck_prefix& p     = get_prefix(node, h);
        p.keysuffix_used = cursor;
        h.count          = count;
        hdr_type::from_node(node).count = count;
        return node;
    }

    // ------------------------------------------------------------------
    // grow -- increase capacity, repack keysuffix, preserve all entries.
    // ------------------------------------------------------------------

    static uint64_t* grow(uint64_t* old_node, hdr_type& h, mem_type& mem,
                          uint16_t blob_delta) {
        const ck_prefix& op = get_prefix(old_node, h);
        uint16_t e   = h.count;
        uint16_t ksz = op.keysuffix_used;

        const uint8_t* old_skip = nullptr;
        if (h.has_skip()) [[unlikely]]
            old_skip = hdr_type::get_skip(old_node, h);

        uint32_t new_ks = ksz + blob_delta;
        uint64_t* nn = alloc_compact_ks(mem, h.skip, old_skip, e + 1, new_ks);
        hdr_type& nh = hdr_type::from_node(nn);

        std::memcpy(lengths(nn, nh), lengths(old_node, h), e);
        std::memcpy(firsts(nn, nh),  firsts(old_node, h),  e);
        std::memcpy(offsets(nn, nh), offsets(old_node, h), e * sizeof(ks_offset_type));
        if (ksz > 0)
            std::memcpy(keysuffix(nn, nh), keysuffix(old_node, h), ksz);

        const auto* osb = h.get_compact_slots(old_node);
        auto*       nsb = nh.get_compact_slots(nn);
        slots::copy_values(nsb, 0, osb, 0, e);

        ck_prefix& np    = get_prefix(nn, nh);
        np.keysuffix_used = ksz;
        nh.count          = e;
        hdr_type::from_node(nn).count = e;

        mem.free_node(old_node);
        return nn;
    }

    // ------------------------------------------------------------------
    // compute_insert_delta — exact keysuffix bytes needed for this insert.
    // ------------------------------------------------------------------

    static uint16_t compute_insert_delta(const uint64_t* node, const hdr_type& h,
                                          int ins, uint8_t klen, uint8_t fb,
                                          const uint8_t* tail_data, uint8_t tail) noexcept {
        const uint8_t* L = lengths(node, h);
        const uint8_t* F = firsts(node, h);
        const ks_offset_type* O = offsets(node, h);
        const uint8_t* B = keysuffix(node, h);
        uint16_t e = h.count;

        // Case 1: share into next
        if (ins < e && F[ins] == fb && L[ins] > klen &&
            std::memcmp(tail_data, B + O[ins], tail) == 0)
            return 0;

        // Check prev for chain relationship
        if (ins > 0 && F[ins - 1] == fb && L[ins - 1] > 1) {
            uint8_t prev_tail = L[ins - 1] - 1;
            if (prev_tail < tail &&
                std::memcmp(B + O[ins - 1], tail_data, prev_tail) == 0) {
                // Case 4: chain split?
                if (ins < e && O[ins] == O[ins - 1] && F[ins] == fb) {
                    int D = ins;
                    while (D + 1 < e && O[D + 1] == O[ins] && F[D + 1] == fb) D++;
                    uint8_t D_tail = L[D] > 0 ? L[D] - 1 : 0;
                    return static_cast<uint16_t>(tail - prev_tail) + D_tail;
                }
                // Case 3: new longest
                return tail - prev_tail;
            }
        }

        // Case 2: standalone
        return tail;
    }

    // ------------------------------------------------------------------
    // insert_at -- sharing-aware insert with 4 cases.
    // Returns (possibly reallocated) node.
    // ------------------------------------------------------------------

    static uint64_t* insert_at(uint64_t* node, hdr_type& h, mem_type& mem,
                                int ins,
                                uint8_t klen, uint8_t fb,
                                const uint8_t* tail_data, uint8_t tail,
                                uint64_t raw_slot) {
        uint8_t* L = lengths(node, h);
        uint8_t* F = firsts(node, h);
        ks_offset_type* O = offsets(node, h);
        uint8_t* B = keysuffix(node, h);
        uint16_t e = h.count;

        // --- Case 1: SHARE INTO NEXT ---
        if (tail > 0 && ins < e && F[ins] == fb && L[ins] > klen &&
            std::memcmp(tail_data, B + O[ins], tail) == 0) {
            if (e >= get_prefix(node, h).cap) [[unlikely]] {
                node = grow(node, h, mem, 0);
                h = hdr_type::from_node(node);
                L = lengths(node, h); F = firsts(node, h);
                O = offsets(node, h); B = keysuffix(node, h);
            }
            ks_offset_type shared_off = O[ins];
            auto* sb = h.get_compact_slots(node);
            int t = e - ins;
            if (t > 0) {
                std::memmove(L + ins + 1, L + ins, t);
                std::memmove(F + ins + 1, F + ins, t);
                std::memmove(O + ins + 1, O + ins, t * sizeof(ks_offset_type));
                slots::move_values(sb, ins + 1, sb, ins, t);
            }
            L[ins] = klen; F[ins] = fb; O[ins] = shared_off;
            slots::store_raw(sb, ins, raw_slot);
            h.count = e + 1;
            hdr_type::from_node(node).count = e + 1;
            return node;
        }

        // Check prev for chain relationship (Cases 3 and 4)
        bool prev_is_prefix = false;
        if (tail > 0 && ins > 0 && F[ins - 1] == fb && L[ins - 1] > 1) {
            uint8_t prev_tail = L[ins - 1] - 1;
            if (prev_tail < tail &&
                std::memcmp(B + O[ins - 1], tail_data, prev_tail) == 0)
                prev_is_prefix = true;
        }

        // --- Case 4: CHAIN SPLIT ---
        if (prev_is_prefix && ins < e &&
            O[ins] == O[ins - 1] && F[ins] == fb) {
            int D = ins;
            while (D + 1 < e && O[D + 1] == O[ins] && F[D + 1] == fb) D++;
            uint8_t D_tail = L[D] > 0 ? L[D] - 1 : 0;
            uint8_t prev_tail = L[ins - 1] - 1;
            uint8_t N_extra = tail - prev_tail;
            uint16_t blob_delta = static_cast<uint16_t>(N_extra) + D_tail;

            ck_prefix& p = get_prefix(node, h);
            if (e >= p.cap || p.keysuffix_used + blob_delta > ck_keysuffix_cap(node, h)) [[unlikely]] {
                node = grow(node, h, mem, blob_delta);
                h = hdr_type::from_node(node);
                L = lengths(node, h); F = firsts(node, h);
                O = offsets(node, h); B = keysuffix(node, h);
                D = ins;
                while (D + 1 < e && O[D + 1] == O[ins] && F[D + 1] == fb) D++;
            }

            ck_prefix& np = get_prefix(node, h);
            auto* sb = h.get_compact_slots(node);
            ks_offset_type chain_off = O[ins - 1];
            ks_offset_type gap_at = chain_off + prev_tail;

            // Save D's full tail before memmove
            uint8_t D_buf[COMPACT_KEYSUFFIX_LIMIT];
            if (D_tail > 0) std::memcpy(D_buf, B + chain_off, D_tail);

            // memmove keysuffix from gap_at rightward by N_extra
            uint16_t bytes_after = np.keysuffix_used - gap_at;
            if (N_extra > 0 && bytes_after > 0)
                std::memmove(B + gap_at + N_extra, B + gap_at, bytes_after);

            // Write N's extra bytes
            if (N_extra > 0)
                std::memcpy(B + gap_at, tail_data + prev_tail, N_extra);

            // Patch O[] for entries shifted by memmove
            if (N_extra > 0) {
                for (int j = 0; j < e; ++j)
                    if (L[j] > 0 && O[j] >= gap_at && O[j] != chain_off)
                        O[j] += N_extra;
            }

            // Write D's full tail as new region at end
            uint16_t new_D_off = np.keysuffix_used + N_extra;
            if (D_tail > 0) std::memcpy(B + new_D_off, D_buf, D_tail);

            // Point C..D to new region
            for (int j = ins; j <= D; ++j)
                if (L[j] > 0) O[j] = static_cast<ks_offset_type>(new_D_off);

            np.keysuffix_used += blob_delta;

            // Shift arrays to insert N at ins
            int t = e - ins;
            if (t > 0) {
                std::memmove(L + ins + 1, L + ins, t);
                std::memmove(F + ins + 1, F + ins, t);
                std::memmove(O + ins + 1, O + ins, t * sizeof(ks_offset_type));
                slots::move_values(sb, ins + 1, sb, ins, t);
            }
            L[ins] = klen; F[ins] = fb; O[ins] = chain_off;
            slots::store_raw(sb, ins, raw_slot);
            h.count = e + 1;
            hdr_type::from_node(node).count = e + 1;
            return node;
        }

        // --- Case 3: NEW LONGEST ---
        if (prev_is_prefix) {
            uint8_t prev_tail = L[ins - 1] - 1;
            uint8_t extra = tail - prev_tail;
            ks_offset_type chain_off = O[ins - 1];
            ks_offset_type gap_at = chain_off + prev_tail;

            ck_prefix& p = get_prefix(node, h);
            if (e >= p.cap || p.keysuffix_used + extra > ck_keysuffix_cap(node, h)) [[unlikely]] {
                node = grow(node, h, mem, extra);
                h = hdr_type::from_node(node);
                L = lengths(node, h); F = firsts(node, h);
                O = offsets(node, h); B = keysuffix(node, h);
            }

            ck_prefix& np = get_prefix(node, h);
            auto* sb = h.get_compact_slots(node);
            chain_off = O[ins - 1];
            gap_at = chain_off + prev_tail;

            uint16_t bytes_after = np.keysuffix_used - gap_at;
            if (extra > 0 && bytes_after > 0)
                std::memmove(B + gap_at + extra, B + gap_at, bytes_after);

            if (extra > 0)
                std::memcpy(B + gap_at, tail_data + prev_tail, extra);

            if (extra > 0) {
                for (int j = 0; j < e; ++j)
                    if (L[j] > 0 && O[j] >= gap_at)
                        O[j] += extra;
            }

            np.keysuffix_used += extra;

            int t = e - ins;
            if (t > 0) {
                std::memmove(L + ins + 1, L + ins, t);
                std::memmove(F + ins + 1, F + ins, t);
                std::memmove(O + ins + 1, O + ins, t * sizeof(ks_offset_type));
                slots::move_values(sb, ins + 1, sb, ins, t);
            }
            L[ins] = klen; F[ins] = fb; O[ins] = chain_off;
            slots::store_raw(sb, ins, raw_slot);
            h.count = e + 1;
            hdr_type::from_node(node).count = e + 1;
            return node;
        }

        // --- Case 2: STANDALONE ---
        ks_offset_type ins_off = (ins < e && L[ins] > 0) ? O[ins]
                          : static_cast<ks_offset_type>(get_prefix(node, h).keysuffix_used);

        {
            ck_prefix& p = get_prefix(node, h);
            bool cap_full = (e >= p.cap);
            bool ks_full = (tail > 0 && p.keysuffix_used + tail > ck_keysuffix_cap(node, h));
            if (cap_full || ks_full) [[unlikely]] {
                bool shuffled = false;
                if (ks_full && !cap_full) {
                    uint16_t new_slots_off = compute_slots_off(
                        p.cap, h.skip, p.keysuffix_used + tail);
                    size_t val_end = static_cast<size_t>(new_slots_off) * U64_BYTES
                                   + slots::value_u64s(p.cap) * U64_BYTES;
                    if (val_end <= static_cast<size_t>(h.alloc_u64) * U64_BYTES) {
                        auto* old_sb = h.get_compact_slots(node);
                        h.slots_off = new_slots_off;
                        hdr_type::from_node(node).slots_off = new_slots_off;
                        auto* new_sb = h.get_compact_slots(node);
                        if (old_sb != new_sb)
                            std::memmove(new_sb, old_sb, slots::size_bytes(e));
                        shuffled = true;
                    }
                }
                if (!shuffled) {
                    node = grow(node, h, mem, tail);
                    h = hdr_type::from_node(node);
                }
                L = lengths(node, h); F = firsts(node, h);
                O = offsets(node, h); B = keysuffix(node, h);
                ins_off = (ins < e && L[ins] > 0) ? O[ins]
                          : static_cast<ks_offset_type>(get_prefix(node, h).keysuffix_used);
            }
        }

        ck_prefix& np = get_prefix(node, h);
        auto* sb = h.get_compact_slots(node);

        uint16_t bytes_after = np.keysuffix_used - ins_off;
        if (tail > 0 && bytes_after > 0)
            std::memmove(B + ins_off + tail, B + ins_off, bytes_after);
        if (tail > 0) std::memcpy(B + ins_off, tail_data, tail);
        np.keysuffix_used += tail;

        if (tail > 0) {
            for (int j = 0; j < e; ++j)
                if (L[j] > 0 && O[j] >= ins_off)
                    O[j] += tail;
        }

        int t = e - ins;
        if (t > 0) {
            std::memmove(L + ins + 1, L + ins, t);
            std::memmove(F + ins + 1, F + ins, t);
            std::memmove(O + ins + 1, O + ins, t * sizeof(ks_offset_type));
            slots::move_values(sb, ins + 1, sb, ins, t);
        }
        L[ins] = klen; F[ins] = fb; O[ins] = ins_off;
        slots::store_raw(sb, ins, raw_slot);
        h.count = e + 1;
        hdr_type::from_node(node).count = e + 1;
        return node;
    }

    // ------------------------------------------------------------------
    // finalize -- check keysuffix invariant; split to bitmask if violated.
    // suffix_len > 255 is caught before insert_at is called.
    // ------------------------------------------------------------------

    static insert_result finalize(uint64_t* node, hdr_type& h,
                                   mem_type& mem, uint16_t pos) {
        if (get_prefix(node, h).keysuffix_used > COMPACT_KEYSUFFIX_LIMIT) [[unlikely]]
            return split_node(node, h, mem);  // leaf=nullptr → caller re-finds
        return {node, insert_outcome::INSERTED, node, pos};
    }

    // ------------------------------------------------------------------
    // cmp_entry -- lex comparison for sorting build_entry arrays
    // ------------------------------------------------------------------

    static int cmp_entry(const build_entry& a, const build_entry& b) noexcept {
        if (a.key_len == 0 && b.key_len == 0) return 0;
        if (a.key_len == 0) return -1;
        if (b.key_len == 0) return  1;
        int c = static_cast<int>(a.key[0]) - static_cast<int>(b.key[0]);
        if (c != 0) return c;
        uint32_t min_len = a.key_len < b.key_len ? a.key_len : b.key_len;
        c = std::memcmp(a.key + 1, b.key + 1, min_len - 1);
        if (c != 0) return c;
        return static_cast<int>(a.key_len) - static_cast<int>(b.key_len);
    }

    // ------------------------------------------------------------------
    // insert -- main dispatch
    // ------------------------------------------------------------------

    static insert_result insert(uint64_t* node, hdr_type& h,
                                 const uint8_t* key_data, uint32_t key_len,
                                 const VALUE& value, uint32_t consumed,
                                 match_result mr, insert_mode mode,
                                 mem_type& mem) {

        if (mr.status == match_status::MATCHED) {
            // Hot path: skip matched; search for the suffix in this node.
            uint32_t suffix_len = key_len - mr.consumed;
            const uint8_t* suffix = key_data + mr.consumed;

            auto [found, pos] = find_pos(node, h, suffix, suffix_len);
            uint16_t upos = static_cast<uint16_t>(pos);

            if (found) {
                if (mode == insert_mode::INSERT)
                    return {node, insert_outcome::FOUND, node, upos};
                auto* sb = h.get_compact_slots(node);
                slots::destroy_value(sb, pos, mem.alloc_v);
                slots::store_value(sb, pos, value, mem.alloc_v);
                return {node, insert_outcome::UPDATED, node, upos};
            }

            if (mode == insert_mode::ASSIGN)
                return {node, insert_outcome::FOUND};

            // Invariant check: suffix_len > 255 cannot be stored in u8 L[].
            // Route to bitmask promotion path.
            if (suffix_len > COMPACT_SUFFIX_LEN_MAX) [[unlikely]]
                return rebuild_with_new(node, h, key_data, key_len, value,
                                        consumed, mr, mem);
                // leaf=nullptr → caller re-finds

            // Normal in-place insert.
            uint8_t klen = static_cast<uint8_t>(suffix_len);
            uint8_t fb   = klen > 0 ? suffix[0] : 0;
            uint8_t tail = klen > 0 ? klen - 1 : 0;

            // Pre-check: compute sharing-aware keysuffix delta.
            // If ks_used + delta > COMPACT_KEYSUFFIX_LIMIT, must split now.
            if (tail > 0) [[likely]] {
                uint16_t delta = compute_insert_delta(node, h, pos, klen, fb,
                                                      suffix + 1, tail);
                if (get_prefix(node, h).keysuffix_used + delta > COMPACT_KEYSUFFIX_LIMIT) [[unlikely]]
                    return rebuild_with_new(node, h, key_data, key_len, value,
                                            consumed, mr, mem);
                    // leaf=nullptr → caller re-finds
            }

            // Allocate value after pre-split check to avoid leak on redirect.
            uint64_t raw = slots::make_raw(value, mem.alloc_v);

            node = insert_at(node, h, mem, pos,
                              klen, fb,
                              (tail > 0 ? suffix + 1 : nullptr), tail,
                              raw);
            h = hdr_type::from_node(node);
            return finalize(node, h, mem, upos);
        }

        // MISMATCH or KEY_EXHAUSTED: prefix shrinks, full rebuild required.
        if (mode == insert_mode::ASSIGN)
            return {node, insert_outcome::FOUND};

        return rebuild_with_new(node, h, key_data, key_len, value,
                                consumed, mr, mem);
        // leaf=nullptr → caller re-finds
    }

    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // rebuild_with_new -- two paths:
    //
    //   MISMATCH / KEY_EXHAUSTED: skip diverged. Reskip existing compact,
    //     create bitmask above with two children. Zero temp allocations.
    //
    //   MATCHED (suffix > 255 or ks overflow): promote compact to bitmask.
    //     Read F/L/O/B directly to build entries. Stack key buffer (bounded
    //     by COMPACT_KEYSUFFIX_LIMIT + count). Entries via make_unique.
    // ------------------------------------------------------------------

    static insert_result rebuild_with_new(uint64_t* node, hdr_type& h,
                                           const uint8_t* key_data,
                                           uint32_t key_len,
                                           const VALUE& value,
                                           uint32_t consumed,
                                           match_result mr,
                                           mem_type& mem) {
        const uint8_t* skip_data = nullptr;
        if (h.has_skip()) [[unlikely]]
            skip_data = hdr_type::get_skip(node, h);
        uint32_t old_skip = h.skip_bytes();

        if (mr.status != match_status::MATCHED) {
            // ---- MISMATCH / KEY_EXHAUSTED: reskip + bitmask ----
            return promote_mismatch(node, h, key_data, key_len, value,
                                    consumed, mr, skip_data, old_skip, mem);
        }

        // ---- MATCHED: suffix > 255 or ks overflow → promote via split ----
        return promote_matched(node, h, key_data, key_len, value,
                               consumed, mr, skip_data, mem);
    }

    // ------------------------------------------------------------------
    // promote_mismatch -- skip diverged at mr.match_len.
    //   Reskip existing compact, create bitmask above, two children.
    //   Zero temp allocations.
    // ------------------------------------------------------------------

    static insert_result promote_mismatch(uint64_t* node, hdr_type& h,
                                           const uint8_t* key_data,
                                           uint32_t key_len,
                                           const VALUE& value,
                                           uint32_t consumed,
                                           match_result mr,
                                           const uint8_t* skip_data,
                                           uint32_t old_skip,
                                           mem_type& mem) {
        using bitmask_ops = kstrie_bitmask<VALUE, CHARMAP, ALLOC>;

        uint32_t match_len = mr.match_len;
        uint32_t child_skip_off = match_len + 1;
        uint32_t child_skip_len = old_skip > child_skip_off
                                ? old_skip - child_skip_off : 0;

        // Read dispatch byte BEFORE reskip — reskip may free old node
        // that skip_data points into.
        uint8_t exist_byte = skip_data[match_len];

        // Copy skip prefix for parent before reskip invalidates it
        uint8_t skip_prefix[hdr_type::SKIP_MAX];
        if (match_len > 0)
            std::memcpy(skip_prefix, skip_data, match_len);

        // Copy child suffix skip before reskip invalidates it
        uint8_t child_skip_buf[hdr_type::SKIP_MAX];
        if (child_skip_len > 0)
            std::memcpy(child_skip_buf, skip_data + child_skip_off, child_skip_len);

        // Reskip existing compact: skip becomes old_skip[match_len+1..]
        uint64_t* existing = reskip(node, h, mem,
            static_cast<uint8_t>(child_skip_len),
            child_skip_len > 0 ? child_skip_buf : nullptr);
        hdr_type& eh = hdr_type::from_node(existing);

        // New entry suffix
        uint32_t new_off = consumed + match_len;

        if (mr.status == match_status::KEY_EXHAUSTED) {
            // New entry has no dispatch byte — it's the EOS child.
            uint64_t raw = slots::make_raw(value, mem.alloc_v);
            build_entry eos_ent{nullptr, 0, raw};
            uint64_t* eos_node = build_compact(mem, 0, nullptr, &eos_ent, 1);

            // Bitmask: skip = old_skip[0..match_len), one byte child, EOS child
            uint8_t  bkt[1] = {exist_byte};
            uint64_t* cld[1] = {existing};
            uint64_t* parent = bitmask_ops::create_with_children(
                mem, static_cast<uint8_t>(match_len),
                match_len > 0 ? skip_prefix : nullptr,
                bkt, cld, 1);
            // Set EOS child
            hdr_type& ph = hdr_type::from_node(parent);
            bitmask_ops::set_eos_child(parent, ph, eos_node);
            // total_tail = children's tail + all entries * parent_skip
            hdr_type eh2 = hdr_type::from_node(existing);
            uint64_t n_entries = static_cast<uint64_t>(eh2.count) + 1;
            parent[NODE_TOTAL_TAIL] = subtree_tail(existing)
                                    + subtree_tail(eos_node)
                                    + n_entries * match_len;
            return {parent, insert_outcome::INSERTED, eos_node, 0};
        }

        // MISMATCH: new entry has a dispatch byte at key_data[new_off].
        uint8_t new_byte = key_data[new_off];
        uint32_t new_suffix_off = new_off + 1;
        uint32_t new_suffix_len = key_len > new_suffix_off
                                ? key_len - new_suffix_off : 0;
        const uint8_t* new_suffix = new_suffix_len > 0
                                  ? key_data + new_suffix_off : nullptr;

        uint64_t raw = slots::make_raw(value, mem.alloc_v);
        build_entry new_ent{new_suffix,
                            static_cast<uint32_t>(new_suffix_len), raw};
        uint64_t* new_child = build_compact(mem, 0, nullptr, &new_ent, 1);

        // Build bitmask with two children, sorted by dispatch byte
        uint8_t  bkt[2];
        uint64_t* cld[2];
        if (exist_byte < new_byte) {
            bkt[0] = exist_byte; cld[0] = existing;
            bkt[1] = new_byte;   cld[1] = new_child;
        } else {
            bkt[0] = new_byte;   cld[0] = new_child;
            bkt[1] = exist_byte; cld[1] = existing;
        }
        uint64_t* parent = bitmask_ops::create_with_children(
            mem, static_cast<uint8_t>(match_len),
            match_len > 0 ? skip_prefix : nullptr,
            bkt, cld, 2);
        parent[NODE_TOTAL_TAIL] = subtree_tail(existing)
                                + subtree_tail(new_child)
                                + (static_cast<uint64_t>(eh.count) + 1)
                                  * match_len;
        return {parent, insert_outcome::INSERTED, new_child, 0};
    }

    // ------------------------------------------------------------------
    // promote_matched -- MATCHED but suffix > 255 or ks overflow.
    //   Build entries from F/L/O/B arrays directly — stack key buffer,
    //   entries via make_unique. No separate key_buf heap alloc.
    // ------------------------------------------------------------------

    // Stack key buffer size: sum(L[i]) = keysuffix_used + count_with_F_byte.
    // Upper bound: COMPACT_KEYSUFFIX_LIMIT (max keysuffix) + MAX_COLLAPSE_ENTRIES (max count).
    static constexpr size_t PROMOTE_KEY_BUF_SIZE =
        COMPACT_KEYSUFFIX_LIMIT + COMPACT_KEYSUFFIX_LIMIT + BITMASK_MAX_CHILDREN;

    static insert_result promote_matched(uint64_t* node, hdr_type& h,
                                          const uint8_t* key_data,
                                          uint32_t key_len,
                                          const VALUE& value,
                                          uint32_t consumed,
                                          match_result mr,
                                          const uint8_t* skip_data,
                                          mem_type& mem) {
        uint16_t new_count = h.count + 1;
        uint8_t  skip_len  = h.skip;

        const uint8_t*        L  = lengths(node, h);
        const uint8_t*        F  = firsts(node, h);
        const ks_offset_type* O  = offsets(node, h);
        const uint8_t*        B  = keysuffix(node, h);
        const auto*           sb = h.get_compact_slots(node);

        // Stack key buffer: concatenate F[i] + tail for each existing entry.
        // Bounded by sum(L[i]) ≤ COMPACT_KEYSUFFIX_LIMIT + count.
        uint8_t key_stk[PROMOTE_KEY_BUF_SIZE];
        auto entries = std::make_unique<build_entry[]>(new_count);

        size_t buf_off = 0;
        for (uint16_t i = 0; i < h.count; ++i) {
            uint8_t klen = L[i];
            uint8_t* dst = key_stk + buf_off;
            if (klen > 0) {
                dst[0] = F[i];
                if (klen > 1)
                    std::memcpy(dst + 1, B + O[i], klen - 1);
            }
            entries[i] = {dst, klen, slots::load_raw(sb, i)};
            buf_off += klen;
        }

        // New entry — suffix starts at mr.consumed
        const uint8_t* new_suffix = key_data + mr.consumed;
        uint32_t new_suffix_len   = key_len - mr.consumed;
        uint64_t raw = slots::make_raw(value, mem.alloc_v);
        build_entry new_ent{new_suffix, new_suffix_len, raw};

        // Binary search for insertion position (existing entries sorted by F)
        auto less = [](const build_entry& a, const build_entry& b) noexcept {
            return cmp_entry(a, b) < 0;
        };
        auto it = std::lower_bound(entries.get(), entries.get() + h.count,
                                   new_ent, less);
        int ins = static_cast<int>(it - entries.get());
        std::memmove(entries.get() + ins + 1, entries.get() + ins,
                     (h.count - ins) * sizeof(build_entry));
        entries[ins] = new_ent;

        uint64_t* result = build_node_from_entries(mem, skip_len, skip_data,
                                                    entries.get(), new_count);
        mem.free_node(node);
        return {result, insert_outcome::INSERTED};
    }

    // ------------------------------------------------------------------
    // reskip -- rebuild compact with new skip prefix.
    // Key data arrays are untouched; only the skip region changes.
    // ------------------------------------------------------------------

    static uint64_t* reskip(uint64_t* node, hdr_type& h, mem_type& mem,
                             uint8_t new_skip_len,
                             const uint8_t* new_skip_data) {
        const ck_prefix& op = get_prefix(node, h);
        uint16_t e       = h.count;
        uint16_t ksz     = op.keysuffix_used;

        uint64_t* nn = alloc_compact_ks(mem, new_skip_len, new_skip_data, e, ksz);
        hdr_type& nh = hdr_type::from_node(nn);

        std::memcpy(lengths(nn, nh), lengths(node, h), e);
        std::memcpy(firsts(nn, nh),  firsts(node, h),  e);
        std::memcpy(offsets(nn, nh), offsets(node, h), e * sizeof(ks_offset_type));
        if (ksz > 0)
            std::memcpy(keysuffix(nn, nh), keysuffix(node, h), ksz);

        const auto* osb = h.get_compact_slots(node);
        auto*       nsb = nh.get_compact_slots(nn);
        slots::copy_values(nsb, 0, osb, 0, e);

        ck_prefix& np    = get_prefix(nn, nh);
        np.keysuffix_used = ksz;
        nh.count          = e;
        hdr_type::from_node(nn).count = e;

        mem.free_node(node);
        return nn;
    }

    // ------------------------------------------------------------------
    // erase_in_place -- remove entry at pos by shifting parallel arrays and keysuffix.
    // Blob stays packed: tail bytes of entries after pos shift left by erased_tail.
    // Always fits (result is strictly smaller). No holes.
    // ------------------------------------------------------------------

    static void erase_in_place(uint64_t* node, hdr_type& h, int pos, mem_type& mem) {
        uint8_t*  L  = lengths(node, h);
        uint8_t*  F  = firsts(node, h);
        ks_offset_type* O = offsets(node, h);
        auto* sb = h.get_compact_slots(node);
        uint16_t  e  = h.count;

        slots::destroy_value(sb, pos, mem.alloc_v);

        uint8_t erased_tail = L[pos] > 0 ? L[pos] - 1 : 0;
        if (erased_tail > 0) {
            // Case A: chain sharer (shares into next)
            bool shares_into_next = (pos + 1 < e &&
                                      O[pos] == O[pos + 1] &&
                                      F[pos] == F[pos + 1]);

            if (!shares_into_next) {
                // Case B or C
                bool is_head = (pos > 0 &&
                                 O[pos - 1] == O[pos] &&
                                 F[pos - 1] == F[pos]);

                ks_offset_type reclaim;
                ks_offset_type gap_off;

                if (is_head) {
                    // Case B: chain head. pos-1 is the new longest.
                    uint8_t new_longest_tail = L[pos - 1] > 0 ? L[pos - 1] - 1 : 0;
                    reclaim = erased_tail - new_longest_tail;
                    gap_off = O[pos] + new_longest_tail;
                } else {
                    // Case C: standalone. Full reclaim.
                    reclaim = erased_tail;
                    gap_off = O[pos];
                }

                if (reclaim > 0) {
                    uint8_t* B = keysuffix(node, h);
                    uint16_t used = get_prefix(node, h).keysuffix_used;
                    uint16_t after = used - gap_off - reclaim;
                    if (after > 0)
                        std::memmove(B + gap_off, B + gap_off + reclaim, after);
                    get_prefix(node, h).keysuffix_used -= reclaim;
                    for (int j = 0; j < e; ++j)
                        if (j != pos && L[j] > 0 && O[j] > gap_off)
                            O[j] -= reclaim;
                }
            }
            // Case A: no keysuffix change
        }

        int tail = e - pos - 1;
        if (tail > 0) {
            std::memmove(L + pos, L + pos + 1, tail);
            std::memmove(F + pos, F + pos + 1, tail);
            std::memmove(O + pos, O + pos + 1, tail * sizeof(ks_offset_type));
            slots::move_values(sb, pos, sb, pos + 1, tail);
        }

        h.count = e - 1;
        hdr_type::from_node(node).count = e - 1;
    }

    // ------------------------------------------------------------------
    // shrink_compact -- rebuild compact node with tight allocation.
    // Called after erase when count < cap/2.
    // ------------------------------------------------------------------

    static uint64_t* shrink_compact(uint64_t* node, hdr_type& h, mem_type& mem) {
        uint16_t e    = h.count;
        uint16_t ksz  = get_prefix(node, h).keysuffix_used;

        const uint8_t* skip_data = nullptr;
        if (h.has_skip()) [[unlikely]]
            skip_data = hdr_type::get_skip(node, h);

        uint64_t* nn = alloc_compact_ks(mem, h.skip, skip_data, e, ksz);
        hdr_type& nh = hdr_type::from_node(nn);

        std::memcpy(lengths(nn, nh), lengths(node, h), e);
        std::memcpy(firsts(nn, nh),  firsts(node, h),  e);
        std::memcpy(offsets(nn, nh), offsets(node, h), e * sizeof(ks_offset_type));
        if (ksz > 0)
            std::memcpy(keysuffix(nn, nh), keysuffix(node, h), ksz);

        const auto* osb = h.get_compact_slots(node);
        auto*       nsb = nh.get_compact_slots(nn);
        slots::copy_values(nsb, 0, osb, 0, e);

        get_prefix(nn, nh).keysuffix_used = ksz;
        nh.count = e;
        hdr_type::from_node(nn).count = e;

        mem.free_node(node);
        return nn;
    }

    // ------------------------------------------------------------------
    // build_node_from_entries -- build a trie node from a sorted build_entry[].
    //
    // Handles two cases that build_compact cannot:
    //   (a) any entry has key_len > 255 (compact L[] is uint8_t)
    //   (b) total tail bytes > COMPACT_KEYSUFFIX_LIMIT (compact O[] is uint8_t)
    //
    // In either case, promotes to a bitmask parent with compact children,
    // recursing until all child entries fit in compact nodes.
    //
    // Callers: rebuild_with_new (prepend may push keys > 255),
    //          split_node (keysuffix overflow).
    // ------------------------------------------------------------------

    static uint64_t* build_node_from_entries(mem_type& mem,
                                              uint8_t    skip_len,
                                              const uint8_t* skip_data,
                                              build_entry*   entries,
                                              uint16_t       count) {
        // Entries must arrive in lexicographic order. Callers guarantee this:
        //   collect_post_skip  -- bitmask traversal visits bits in ascending byte order
        //   rebuild_with_new   -- linear insertion into already-sorted array
        //   split_node         -- collect_entries reads the sorted compact arrays

        // Fast path: all entries fit in a compact node.
        bool needs_bitmask = false;
        uint32_t total_tail = 0;
        for (uint16_t i = 0; i < count; ++i) {
            if (entries[i].key_len > COMPACT_SUFFIX_LEN_MAX) { needs_bitmask = true; break; }
            if (entries[i].key_len > 1)
                total_tail += entries[i].key_len - 1;
        }
        if (!needs_bitmask && total_tail > COMPACT_KEYSUFFIX_LIMIT)
            needs_bitmask = true;
        if (!needs_bitmask)
            return build_compact(mem, skip_len, skip_data, entries, count);

        // Must promote to bitmask. Zero-length entry becomes EOS child.
        uint64_t eos_raw    = 0;
        bool     has_eos    = false;
        uint16_t data_start = 0;

        if (count > 0 && entries[0].key_len == 0) {
            eos_raw    = entries[0].raw_slot;
            has_eos    = true;
            data_start = 1;
        }

        // Bucket remaining entries by first byte.
        uint8_t  bucket_bytes[256];
        uint16_t bucket_start[256];
        uint16_t bucket_count[256];
        int n_buckets = 0;

        uint8_t prev_byte = 0;
        bool    is_first  = true;
        for (uint16_t i = data_start; i < count; ++i) {
            uint8_t fb = entries[i].key[0];
            if (is_first || fb != prev_byte) {
                bucket_bytes[n_buckets]  = fb;
                bucket_start[n_buckets]  = i;
                bucket_count[n_buckets]  = 1;
                ++n_buckets;
                prev_byte = fb;
                is_first  = false;
            } else {
                ++bucket_count[n_buckets - 1];
            }
        }

        // Build a child node for each bucket.
        // Single-entry key_len==1 buckets become compact(1) children.
        uint64_t* children[256];
        uint8_t   child_bytes[256];
        int       n_children = 0;

        for (int b = 0; b < n_buckets; ++b) {
            uint16_t start = bucket_start[b];
            uint16_t cnt   = bucket_count[b];

            // Single entry with key_len==1 → compact(1) with EOS value
            if (cnt == 1 && entries[start].key_len == 1) {
                build_entry be{nullptr, 0, entries[start].raw_slot};
                child_bytes[n_children] = bucket_bytes[b];
                children[n_children] = build_compact(mem, 0, nullptr, &be, 1);
                n_children++;
                continue;
            }

            // Strip dispatch byte from all entries.
            auto child_entries = std::make_unique<build_entry[]>(cnt);
            for (uint16_t j = 0; j < cnt; ++j) {
                child_entries[j].key      = entries[start + j].key     + 1;
                child_entries[j].key_len  = entries[start + j].key_len - 1;
                child_entries[j].raw_slot = entries[start + j].raw_slot;
            }

            uint32_t first_nz = cnt;
            uint32_t lcp = 0;
            {
                for (uint16_t j = 0; j < cnt; ++j) {
                    if (child_entries[j].key_len > 0) { first_nz = j; break; }
                }
                if (first_nz < cnt) {
                    lcp = child_entries[first_nz].key_len;
                    for (uint16_t j = first_nz + 1; j < cnt; ++j) {
                        uint32_t kl = child_entries[j].key_len;
                        if (kl == 0) { lcp = 0; break; }
                        uint32_t ml = lcp < kl ? lcp : kl;
                        uint32_t i  = 0;
                        while (i < ml && child_entries[first_nz].key[i]
                                      == child_entries[j].key[i]) ++i;
                        lcp = i;
                        if (lcp == 0) break;
                    }
                }
            }
            // Zero-length entries before first_nz share no prefix; LCP must be 0.
            if (first_nz > 0) lcp = 0;
            if (lcp > hdr_type::SKIP_MAX) lcp = hdr_type::SKIP_MAX;

            const uint8_t* child_skip = (lcp > 0) ? child_entries[first_nz].key : nullptr;

            if (lcp > 0) {
                for (uint16_t j = 0; j < cnt; ++j) {
                    if (child_entries[j].key_len > 0) {
                        child_entries[j].key     += lcp;
                        child_entries[j].key_len -= lcp;
                    }
                }
            }

            child_bytes[n_children] = bucket_bytes[b];
            children[n_children] = build_node_from_entries(mem,
                                                   static_cast<uint8_t>(lcp),
                                                   child_skip,
                                                   child_entries.get(), cnt);
            n_children++;
        }

        uint64_t* parent = bitmask_ops::create_with_children(
            mem, skip_len, skip_data,
            child_bytes, children,
            static_cast<uint16_t>(n_children));

        // Compute total_tail: sum of children's collapsed-byte estimates
        uint64_t desc = 0;
        for (int b = 0; b < n_children; ++b) {
            hdr_type ch = hdr_type::from_node(children[b]);
            if (ch.is_bitmap()) desc += children[b][NODE_TOTAL_TAIL];
            else desc += get_prefix(children[b], ch).keysuffix_used
                       + static_cast<uint64_t>(ch.count) * (1 + ch.skip_bytes());
        }

        // EOS → compact(1) child set as eos_child (keysuffix_used=0)
        if (has_eos) {
            build_entry be{nullptr, 0, eos_raw};
            uint64_t* eos_node = build_compact(mem, 0, nullptr, &be, 1);
            hdr_type& ph = hdr_type::from_node(parent);
            bitmask_ops::set_eos_child(parent, ph, eos_node);
        }

        parent[NODE_TOTAL_TAIL] = desc + static_cast<uint64_t>(count) * skip_len;

        // Link all children (byte + EOS) to their parent
        bitmask_ops::link_all_children(parent);

        return parent;
    }

    // split_node -- split an over-full compact node into a bitmask subtree.
    // Triggered by finalize when keysuffix_used > COMPACT_KEYSUFFIX_LIMIT.
    // ------------------------------------------------------------------

    static insert_result split_node(uint64_t* node, hdr_type& h,
                                     mem_type& mem) {
        uint16_t N = h.count;

        // RAII heap buffers for exception safety.
        auto key_buf = std::make_unique<uint8_t[]>(static_cast<size_t>(N) * 256);
        auto all     = std::make_unique<build_entry[]>(N);
        collect_entries(node, h, all.get(), key_buf.get());

        uint8_t        skip_len  = h.skip;
        const uint8_t* skip_data = nullptr;
        if (h.has_skip()) [[unlikely]]
            skip_data = hdr_type::get_skip(node, h);

        uint64_t* result = build_node_from_entries(mem, skip_len, skip_data,
                                                    all.get(), N);
        mem.free_node(node);
        return {result, insert_outcome::INSERTED};
    }
};

} // namespace gteitelbaum::kstrie_detail

#endif // KSTRIE_COMPACT_HPP
