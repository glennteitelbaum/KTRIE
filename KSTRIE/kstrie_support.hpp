#ifndef KSTRIE_SUPPORT_HPP
#define KSTRIE_SUPPORT_HPP

#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace gteitelbaum::kstrie_detail {

// ============================================================================
// Arithmetic helpers
// ============================================================================

// Round x up to the next multiple of alignment y
inline constexpr size_t align_up(size_t x, size_t y) noexcept {
    return ((x + (y - 1)) / y) * y;
}

// Ceiling integer division: ceil(x / y)
inline constexpr size_t div_ceil(size_t x, size_t y) noexcept {
    return (x + y - 1) / y;
}

// Fundamental slot width
#ifndef KTRIE_U64_BYTES_DEFINED
#define KTRIE_U64_BYTES_DEFINED
inline constexpr size_t U64_BYTES = sizeof(uint64_t);
#endif

// Named node-word indices (u64 offsets into node allocation)
inline constexpr size_t NODE_HEADER      = 0;
inline constexpr size_t NODE_PARENT_PTR  = 1;  // bitmask: parent pointer
inline constexpr size_t NODE_TOTAL_TAIL  = 2;  // bitmask only: u64 sum of children's keysuffix bytes

template <typename ALLOC>
struct kstrie_memory;

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_skip;

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_bitmask;

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_compact;


inline constexpr uint16_t padded_size(uint16_t needed) noexcept {
    if (needed <= 4) return needed;
    unsigned bits = std::bit_width(static_cast<unsigned>(needed - 1));
    uint16_t upper = uint16_t(1) << bits;
    uint16_t lower = upper >> 1;
    uint16_t mid   = lower + (lower >> 1);
    if (needed <= lower) return lower;
    if (needed <= mid)   return mid;
    return upper;
}

template <size_t WORDS>
struct bitmap_n {
    static_assert(WORDS == 1 || WORDS == 2 || WORDS == 4);
    uint64_t words[WORDS]{};

    [[nodiscard]] bool has_bit(uint8_t idx) const noexcept {
        if constexpr (WORDS == 1) return (words[0] >> idx) & 1;
        else return (words[idx >> 6] >> (idx & 63)) & 1;
    }
    void set_bit(uint8_t idx) noexcept {
        if constexpr (WORDS == 1) words[0] |= uint64_t(1) << idx;
        else words[idx >> 6] |= uint64_t(1) << (idx & 63);
    }
    void clear_bit(uint8_t idx) noexcept {
        if constexpr (WORDS == 1) words[0] &= ~(uint64_t(1) << idx);
        else words[idx >> 6] &= ~(uint64_t(1) << (idx & 63));
    }
    [[nodiscard]] int find_slot(uint8_t idx) const noexcept {
        if (!has_bit(idx)) return -1;
        return count_below(idx);
    }
    [[nodiscard]] int count_below(uint8_t idx) const noexcept {
        if constexpr (WORDS == 1) {
            uint64_t mask = (uint64_t(1) << idx) - 1;
            return std::popcount(words[0] & mask);
        } else {
            int w = idx >> 6;
            uint64_t mask = (uint64_t(1) << (idx & 63)) - 1;
            int cnt = 0;
            for (int i = 0; i < w; ++i) cnt += std::popcount(words[i]);
            cnt += std::popcount(words[w] & mask);
            return cnt;
        }
    }
    [[nodiscard]] int slot_for_insert(uint8_t idx) const noexcept { return count_below(idx); }
    [[nodiscard]] int popcount() const noexcept {
        if constexpr (WORDS == 1) return std::popcount(words[0]);
        else if constexpr (WORDS == 2) return std::popcount(words[0]) + std::popcount(words[1]);
        else return std::popcount(words[0]) + std::popcount(words[1]) + std::popcount(words[2]) + std::popcount(words[3]);
    }
    [[nodiscard]] int find_next_set(int start) const noexcept {
        constexpr int MAX_BITS = WORDS * 64;
        if (start < 0) start = 0;
        if (start >= MAX_BITS) return -1;
        int w = start >> 6;
        uint64_t masked = words[w] & (~uint64_t(0) << (start & 63));
        while (true) {
            if (masked) return w * 64 + std::countr_zero(masked);
            if (++w >= static_cast<int>(WORDS)) return -1;
            masked = words[w];
        }
    }

    [[nodiscard]] int find_prev_set(int start) const noexcept {
        constexpr int MAX_BITS = WORDS * 64;
        if (start < 0) return -1;
        if (start >= MAX_BITS) start = MAX_BITS - 1;
        int w = start >> 6;
        uint64_t masked = words[w] & (~uint64_t(0) >> (63 - (start & 63)));
        while (true) {
            if (masked) return w * 64 + 63 - std::countl_zero(masked);
            if (--w < 0) return -1;
            masked = words[w];
        }
    }
};

inline constexpr std::array<uint8_t, 256> IDENTITY_MAP = []() {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = static_cast<uint8_t>(i);
    return m;
}();

inline constexpr std::array<uint8_t, 256> UPPER_MAP = []() {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = '*';
    for (int i = 'A'; i <= 'Z'; ++i) m[i] = static_cast<uint8_t>(i);
    for (int i = 'a'; i <= 'z'; ++i) m[i] = static_cast<uint8_t>('A' + (i - 'a'));
    for (int i = '0'; i <= '9'; ++i) m[i] = static_cast<uint8_t>(i);
    m[' '] = ' '; m[','] = ','; m['-'] = '-'; m['.'] = '.'; m['\''] = '\'';
    m[0] = 0;
    return m;
}();

inline constexpr std::array<uint8_t, 256> REVERSE_LOWER_MAP = []() {
    std::array<uint8_t, 256> m{};
    for (int i = 0; i < 256; ++i) m[i] = '*';
    for (int i = 'A'; i <= 'Z'; ++i) m[i] = static_cast<uint8_t>('z' - (i - 'A'));
    for (int i = 'a'; i <= 'z'; ++i) m[i] = static_cast<uint8_t>('z' - (i - 'a'));
    for (int i = '0'; i <= '9'; ++i) m[i] = static_cast<uint8_t>(i);
    m[' '] = ' '; m[','] = ','; m['-'] = '-'; m['.'] = '.'; m['\''] = '\'';
    m[0] = 0;
    return m;
}();

template <std::array<uint8_t, 256> USER_MAP>
struct char_map {
private:
    static constexpr bool compute_is_identity() { return USER_MAP == IDENTITY_MAP; }
    static constexpr size_t compute_unique_count() {
        std::array<bool, 256> seen{};
        size_t n = 0;
        for (int c = 0; c < 256; ++c) {
            if (!seen[USER_MAP[c]]) { seen[USER_MAP[c]] = true; n++; }
        }
        return n;
    }
    static constexpr auto gather_sorted_unique() {
        std::array<uint8_t, 256> vals{};
        std::array<bool, 256> seen{};
        size_t n = 0;
        for (int c = 0; c < 256; ++c) {
            uint8_t v = USER_MAP[c];
            if (!seen[v]) { seen[v] = true; vals[n++] = v; }
        }
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (vals[j] < vals[i]) { auto t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
        return std::pair{vals, n};
    }
    static constexpr auto compute_value_to_index() {
        auto [vals, n] = gather_sorted_unique();
        std::array<uint8_t, 256> m{};
        for (size_t i = 0; i < n; ++i) m[vals[i]] = static_cast<uint8_t>(i + 1);
        return m;
    }
    static constexpr auto compute_char_to_index() {
        auto v2i = compute_value_to_index();
        std::array<uint8_t, 256> m{};
        for (int c = 0; c < 256; ++c) m[c] = v2i[USER_MAP[c]];
        return m;
    }
    static constexpr auto compute_index_to_char() {
        auto [vals, n] = gather_sorted_unique();
        std::array<uint8_t, 256> m{};
        for (size_t i = 0; i < n; ++i) m[i + 1] = vals[i];
        return m;
    }
public:
    static constexpr bool IS_IDENTITY   = compute_is_identity();
    static constexpr size_t UNIQUE_COUNT = IS_IDENTITY ? 256 : compute_unique_count();
    static constexpr size_t BITMAP_WORDS =
        IS_IDENTITY ? 4 : (UNIQUE_COUNT <= 64) ? 1 : (UNIQUE_COUNT <= 128) ? 2 : 4;
    static constexpr bool NEEDS_REMAP = !IS_IDENTITY && (BITMAP_WORDS < 4);
    static constexpr std::array<uint8_t, 256> CHAR_TO_INDEX =
        NEEDS_REMAP ? compute_char_to_index() : USER_MAP;
    static constexpr std::array<uint8_t, 256> INDEX_TO_CHAR =
        NEEDS_REMAP ? compute_index_to_char() : USER_MAP;
    static constexpr uint8_t to_index(uint8_t c) noexcept {
        if constexpr (IS_IDENTITY) return c;
        else return CHAR_TO_INDEX[c];
    }
    static constexpr uint8_t from_index(uint8_t i) noexcept {
        if constexpr (IS_IDENTITY) return i;
        else return INDEX_TO_CHAR[i];
    }
    using bitmap_type = bitmap_n<BITMAP_WORDS>;
};

using identity_char_map       = char_map<IDENTITY_MAP>;
using upper_char_map          = char_map<UPPER_MAP>;
using reverse_lower_char_map  = char_map<REVERSE_LOWER_MAP>;

template <typename VALUE>
struct kstrie_slots {
    static constexpr bool IS_TRIVIAL = std::is_trivially_copyable_v<VALUE>;
    static constexpr bool IS_BITMAP  = std::is_same_v<VALUE, bool>;
    static constexpr bool IS_INLINE  = !IS_BITMAP && IS_TRIVIAL && sizeof(VALUE) <= U64_BYTES;
    static constexpr size_t SLOT_WIDTH = IS_INLINE ? sizeof(VALUE) : sizeof(void*);

    static constexpr size_t BITS_PER_WORD = sizeof(uint64_t) * CHAR_BIT;
    static constexpr size_t WORD_BIT_MASK = BITS_PER_WORD - 1;
    static constexpr size_t LOG2_BPW      = 6;

    static constinit bool BOOL_VALUES[2];

    using value_base_t = std::conditional_t<IS_BITMAP, uint64_t,
                         std::conditional_t<IS_INLINE, VALUE, uint64_t>>;

    static constexpr size_t size_bytes(uint16_t n) noexcept {
        if constexpr (IS_BITMAP)
            return div_ceil(static_cast<size_t>(n), BITS_PER_WORD) * U64_BYTES;
        else
            return n * sizeof(value_base_t);
    }

    // u64s needed for n packed values, padded to u64 alignment
    static constexpr size_t value_u64s(uint16_t n) noexcept {
        if constexpr (IS_BITMAP)
            return div_ceil(static_cast<size_t>(n), BITS_PER_WORD);
        else
            return div_ceil(static_cast<size_t>(n) * sizeof(value_base_t), U64_BYTES);
    }

    // Array element count for stack buffers (bitmap: words, else: entries)
    static constexpr size_t value_array_size(size_t n) noexcept {
        if constexpr (IS_BITMAP)
            return div_ceil(n, BITS_PER_WORD);
        else
            return n;
    }

    // --- Value access (value_base_t* base, natural array indexing) ---

    template <typename Alloc>
    static void store_value(value_base_t* base, size_t index, const VALUE& v, Alloc& alloc) {
        if constexpr (IS_BITMAP) {
            size_t word = index >> LOG2_BPW;
            size_t bit  = index & WORD_BIT_MASK;
            if (v) base[word] |=  (uint64_t(1) << bit);
            else   base[word] &= ~(uint64_t(1) << bit);
        } else if constexpr (IS_INLINE) {
            base[index] = value_base_t{};
            std::memcpy(&base[index], &v, sizeof(VALUE));
        } else {
            using va = typename std::allocator_traits<Alloc>::template rebind_alloc<VALUE>;
            va a(alloc);
            VALUE* p = a.allocate(1);
            std::construct_at(p, v);
            std::memcpy(&base[index], &p, sizeof(p));
        }
    }

    static VALUE* load_value(value_base_t* base, size_t index) noexcept {
        if constexpr (IS_BITMAP) {
            return &BOOL_VALUES[(base[index >> LOG2_BPW] >> (index & WORD_BIT_MASK)) & 1];
        } else if constexpr (IS_INLINE) {
            return reinterpret_cast<VALUE*>(&base[index]);
        } else {
            return reinterpret_cast<VALUE*>(base[index]);
        }
    }
    static const VALUE* load_value(const value_base_t* base, size_t index) noexcept {
        if constexpr (IS_BITMAP) {
            return &BOOL_VALUES[(base[index >> LOG2_BPW] >> (index & WORD_BIT_MASK)) & 1];
        } else if constexpr (IS_INLINE) {
            return reinterpret_cast<const VALUE*>(&base[index]);
        } else {
            return reinterpret_cast<const VALUE*>(base[index]);
        }
    }

    template <typename Alloc>
    static void destroy_value(value_base_t* base, size_t index, Alloc& alloc) {
        if constexpr (!IS_INLINE && !IS_BITMAP) {
            using va = typename std::allocator_traits<Alloc>::template rebind_alloc<VALUE>;
            va a(alloc);
            VALUE* p = reinterpret_cast<VALUE*>(base[index]);
            std::destroy_at(p);
            a.deallocate(p, 1);
        }
    }
    template <typename Alloc>
    static void destroy_values(value_base_t* base, size_t start, size_t count, Alloc& alloc) {
        if constexpr (!IS_INLINE && !IS_BITMAP) {
            for (size_t i = 0; i < count; ++i) destroy_value(base, start + i, alloc);
        }
    }

    static void copy_values(value_base_t* dst, size_t di,
                            const value_base_t* src, size_t si, size_t n) noexcept {
        if (n == 0) return;
        if constexpr (IS_BITMAP) {
            if (si == 0 && di == 0) {
                std::memcpy(dst, src, div_ceil(n, BITS_PER_WORD) * sizeof(uint64_t));
                return;
            }
            for (size_t i = 0; i < n; ++i) {
                size_t sb = si + i, db = di + i;
                uint64_t bit = (src[sb >> LOG2_BPW] >> (sb & WORD_BIT_MASK)) & 1;
                dst[db >> LOG2_BPW] = (dst[db >> LOG2_BPW] & ~(uint64_t(1) << (db & WORD_BIT_MASK)))
                                    | (bit << (db & WORD_BIT_MASK));
            }
        } else {
            std::memcpy(&dst[di], &src[si], n * sizeof(value_base_t));
        }
    }
    static void bitmap_shift_up(value_base_t* bm, size_t from, size_t count) noexcept {
        if (count == 0) return;
        size_t w_lo = from >> LOG2_BPW;
        size_t w_hi = (from + count) >> LOG2_BPW;
        size_t fb   = from & WORD_BIT_MASK;

        uint64_t lo_mask = (fb == 0) ? 0 : ((uint64_t(1) << fb) - 1);
        uint64_t lo_save = bm[w_lo] & lo_mask;

        for (size_t w = w_hi; w > w_lo; --w)
            bm[w] = (bm[w] << 1) | (bm[w - 1] >> WORD_BIT_MASK);
        bm[w_lo] <<= 1;

        bm[w_lo] = (bm[w_lo] & ~lo_mask) | lo_save;
    }

    static void bitmap_shift_down(value_base_t* bm, size_t from, size_t count) noexcept {
        if (count == 0) return;
        size_t dst  = from - 1;
        size_t d_lo = dst >> LOG2_BPW;
        size_t w_lo = from >> LOG2_BPW;
        size_t w_hi = (from + count - 1) >> LOG2_BPW;
        size_t db   = dst & WORD_BIT_MASK;

        uint64_t d_mask = (db == 0) ? 0 : ((uint64_t(1) << db) - 1);
        uint64_t d_save = bm[d_lo] & d_mask;

        // Cross-word carry: when dst is in a lower word than from,
        // the >>1 loop won't reach d_lo. Carry bit[from] manually.
        if (d_lo < w_lo) {
            size_t fb = from & WORD_BIT_MASK;
            uint64_t carry = (bm[w_lo] >> fb) & 1;
            bm[d_lo] = (bm[d_lo] & ~(uint64_t(1) << db)) | (carry << db);
        }

        // Bulk shift >>1 with carry, low to high
        for (size_t w = w_lo; w < w_hi; ++w)
            bm[w] = (bm[w] >> 1) | (bm[w + 1] << WORD_BIT_MASK);
        bm[w_hi] >>= 1;

        // Restore preserved bits below dst
        if (d_lo == w_lo)
            bm[d_lo] = (bm[d_lo] & ~d_mask) | d_save;
    }

    static void move_values(value_base_t* dst, size_t di,
                            const value_base_t* src, size_t si, size_t n) noexcept {
        if (n == 0 || di == si) return;
        if constexpr (IS_BITMAP) {
            if (di == si + 1) bitmap_shift_up(dst, si, n);
            else if (di + 1 == si) bitmap_shift_down(dst, si, n);
        } else {
            std::memmove(&dst[di], &src[si], n * sizeof(value_base_t));
        }
    }

    // --- Raw slot transfer (build_entry.raw_slot ↔ packed value region) ---

    // Create a raw u64 slot from a value. For inline types, memcpys the value.
    // For non-inline types, allocates via the provided allocator.
    template <typename Alloc>
    static uint64_t make_raw(const VALUE& v, Alloc& alloc) {
        if constexpr (IS_BITMAP) {
            return v ? 1 : 0;
        } else if constexpr (IS_INLINE) {
            uint64_t raw = 0;
            std::memcpy(&raw, &v, sizeof(VALUE));
            return raw;
        } else {
            using va = typename std::allocator_traits<Alloc>::template rebind_alloc<VALUE>;
            va a(alloc);
            VALUE* p = a.allocate(1);
            std::construct_at(p, v);
            uint64_t raw = 0;
            std::memcpy(&raw, &p, sizeof(p));
            return raw;
        }
    }

    static void store_raw(value_base_t* base, size_t index, uint64_t raw) noexcept {
        if constexpr (IS_BITMAP) {
            size_t word = index >> LOG2_BPW;
            size_t bit  = index & WORD_BIT_MASK;
            if (raw & 1) base[word] |=  (uint64_t(1) << bit);
            else         base[word] &= ~(uint64_t(1) << bit);
        } else {
            std::memcpy(&base[index], &raw, sizeof(value_base_t));
        }
    }
    static uint64_t load_raw(const value_base_t* base, size_t index) noexcept {
        if constexpr (IS_BITMAP) {
            return (base[index >> LOG2_BPW] >> (index & WORD_BIT_MASK)) & 1;
        } else {
            uint64_t raw = 0;
            std::memcpy(&raw, &base[index], sizeof(value_base_t));
            return raw;
        }
    }

    // --- Child pointer access (always u64, separate from values) ---

    static void store_child(uint64_t* base, size_t index, uint64_t* child) noexcept {
        base[index] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(child));
    }
    static uint64_t* load_child(const uint64_t* base, size_t index) noexcept {
        return reinterpret_cast<uint64_t*>(static_cast<std::uintptr_t>(base[index]));
    }
    static void copy_children(uint64_t* dst, size_t di,
                              const uint64_t* src, size_t si, size_t n) noexcept {
        if (n > 0) std::memcpy(&dst[di], &src[si], n * U64_BYTES);
    }
    static void move_children(uint64_t* dst, size_t di,
                              const uint64_t* src, size_t si, size_t n) noexcept {
        if (n > 0) std::memmove(&dst[di], &src[si], n * U64_BYTES);
    }
};

template <typename VALUE>
constinit bool kstrie_slots<VALUE>::BOOL_VALUES[2] = {false, true};

// ============================================================================
// node_header -- 8 bytes
//
// slots_off: cached offset from node start to slots region, in u64 units.
// Read path: get_slots(node) = node + slots_off. One add, zero arithmetic.
// Write path computes keys_bytes locally, stores slots_off in header.
// ============================================================================

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct node_header {
    uint16_t alloc_u64;     // allocation size in u64 units
    uint16_t count;         // compact: entry count, bitmask: child_count
    uint16_t slots_off;     // compact: values offset (u64 units), bitmask: unused
    uint8_t  skip;          // prefix byte count (0 = no prefix)
    uint8_t  flags;         // bit0: bitmask, bit2: has_skip. All-zeros = compact.

    // Maximum skip prefix that fits in a single node (full u8 range, no sentinel)
    static constexpr uint8_t SKIP_MAX = 255;

    static constexpr uint8_t FLAG_BITMASK    = 1;
    static constexpr uint8_t FLAG_HAS_SKIP   = 4;

    [[nodiscard]] bool is_compact()  const noexcept { return !(flags & FLAG_BITMASK); }
    [[nodiscard]] bool is_bitmap()   const noexcept { return flags & FLAG_BITMASK; }
    [[nodiscard]] bool has_skip()    const noexcept { return flags & FLAG_HAS_SKIP; }

    [[nodiscard]] uint32_t skip_bytes() const noexcept { return skip; }

    void set_compact(bool v) noexcept {
        if (v) flags &= ~FLAG_BITMASK;
    }
    void set_bitmask(bool v) noexcept {
        if (v) flags |= FLAG_BITMASK;
        else   flags &= ~FLAG_BITMASK;
    }
    void set_has_skip(bool v) noexcept { if (v) flags |= FLAG_HAS_SKIP; else flags &= ~FLAG_HAS_SKIP; }
    void set_skip_len(uint8_t len) noexcept { skip = len; set_has_skip(len > 0); }

    using slots_t = kstrie_slots<VALUE>;

    // Bitmask: count = child_count (no leaves)
    [[nodiscard]] uint16_t child_count() const noexcept { return count; }
    void set_child_count(uint16_t c) noexcept { count = c; }

    void copy_from(const node_header& src) noexcept {
        uint16_t saved = alloc_u64; *this = src; alloc_u64 = saved;
    }

    [[nodiscard]] uint16_t total_slots() const noexcept {
        if (is_compact()) return static_cast<uint16_t>(slots_t::value_u64s(count));
        // bitmask: sentinel(1) + children(count) + eos(1) = count + 2
        return count + 2;
    }

    static constexpr size_t header_size() noexcept { return 8; }

    // REMOVED skip_size() — skip no longer at fixed offset after header.
    // Use get_skip(node, h) which dispatches by node type.

    // Write-path only: compute index size by dispatching to type
    [[nodiscard]] size_t index_size() const noexcept;

    [[nodiscard]] size_t node_size() const noexcept {
        if (is_compact()) {
            return (static_cast<size_t>(slots_off) + slots_t::value_u64s(count)) * U64_BYTES;
        }
        // bitmask: header + parent_ptr + total_tail + bitmap + sentinel + children + eos + skip
        size_t u64s = 3 + BITMAP_WORDS + 1 + count + 1;
        if (has_skip()) u64s += div_ceil(align_up(skip_bytes(), U64_BYTES), U64_BYTES);
        return u64s * U64_BYTES;
    }

    // Type-aware skip accessor. Only called when has_skip() is true.
    static uint8_t* get_skip(uint64_t* node, const node_header& h) noexcept;
    static const uint8_t* get_skip(const uint64_t* node, const node_header& h) noexcept {
        return get_skip(const_cast<uint64_t*>(node), h);
    }

    // REMOVED get_index() — compact and bitmask have different fixed-offset layouts.

    // REMOVED get_slots — bitmask uses child_slots(), compact uses get_compact_slots()

    // --- Bitmap-typed accessors (read-path: branchless) ---

    static constexpr size_t BITMAP_WORDS = CHARMAP::BITMAP_WORDS;

    // Bitmask bitmap at byte 16: after header(8B) + desc(8B)
    static constexpr size_t BITMAP_NODE_OFF = 2;  // u64 offset

    [[nodiscard]] const uint64_t* get_bitmap_index(const uint64_t* node) const noexcept {
        return node + BITMAP_NODE_OFF;
    }
    [[nodiscard]] uint64_t* get_bitmap_index(uint64_t* node) const noexcept {
        return node + BITMAP_NODE_OFF;
    }
    // REMOVED get_bitmap_slots — bitmask uses child_slots/leaf_vals/eos_ptr accessors

    // --- Compact-typed accessors ---

    // Compact parallel arrays start after header(8B) + ck_prefix(8B) + parent_ptr(8B)
    static constexpr size_t COMPACT_ARRAYS_OFF = 3 * U64_BYTES;

    [[nodiscard]] const uint8_t* get_compact_index(const uint64_t* node) const noexcept {
        return reinterpret_cast<const uint8_t*>(node) + COMPACT_ARRAYS_OFF;
    }
    [[nodiscard]] uint8_t* get_compact_index(uint64_t* node) const noexcept {
        return reinterpret_cast<uint8_t*>(node) + COMPACT_ARRAYS_OFF;
    }

    using vbase_t = typename slots_t::value_base_t;
    [[nodiscard]] const vbase_t* get_compact_slots(const uint64_t* node) const noexcept {
        return reinterpret_cast<const vbase_t*>(node + slots_off);
    }
    [[nodiscard]] vbase_t* get_compact_slots(uint64_t* node) const noexcept {
        return reinterpret_cast<vbase_t*>(node + slots_off);
    }

    static node_header& from_node(uint64_t* node) noexcept {
        return *reinterpret_cast<node_header*>(node);
    }
    static const node_header& from_node(const uint64_t* node) noexcept {
        return *reinterpret_cast<const node_header*>(node);
    }
};

// Deferred index_size (write-path only)
template <typename VALUE, typename CHARMAP, typename ALLOC>
size_t node_header<VALUE, CHARMAP, ALLOC>::index_size() const noexcept {
    if (is_compact())
        return kstrie_compact<VALUE, CHARMAP, ALLOC>::index_size(*this);
    else
        return kstrie_bitmask<VALUE, CHARMAP, ALLOC>::index_size(*this);
}

// Deferred get_skip (type-aware: compact reads skip_data_off, bitmask at end-of-slots)
template <typename VALUE, typename CHARMAP, typename ALLOC>
uint8_t* node_header<VALUE, CHARMAP, ALLOC>::get_skip(
        uint64_t* node, const node_header& h) noexcept {
    if (h.is_compact()) {
        auto& p = kstrie_compact<VALUE, CHARMAP, ALLOC>::get_prefix(node, h);
        return reinterpret_cast<uint8_t*>(node) + p.skip_data_off;
    } else {
        // bitmask: skip after sentinel + children + eos_ptr
        constexpr size_t SENTINEL_OFF = kstrie_bitmask<VALUE, CHARMAP, ALLOC>::SENTINEL_OFF;
        return reinterpret_cast<uint8_t*>(node + SENTINEL_OFF + 1 + h.count + 1);
    }
}

static_assert(sizeof(node_header<int, identity_char_map, std::allocator<uint64_t>>) == 8);

// REMOVED sentinel — use nullptr for empty state

// ============================================================================
// Layout helpers
// ============================================================================

// REMOVED align8 — use align_up(n, 8) instead

template <class T>
inline int makecmp(T a, T b) noexcept {
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

inline constexpr uint32_t COMPACT_KEYSUFFIX_LIMIT = 4096;  // max keysuffix bytes
inline constexpr uint8_t  COMPACT_SUFFIX_LEN_MAX = 255;    // max suffix length per entry (uint8_t L[])
using ks_offset_type = std::conditional_t<COMPACT_KEYSUFFIX_LIMIT <= 256, uint8_t, uint16_t>;

// Max suffix bytes per compact entry (same as COMPACT_SUFFIX_LEN_MAX but uint32_t for sizing)
inline constexpr uint32_t COMPACT_ENTRY_KEY_MAX = COMPACT_SUFFIX_LEN_MAX;

// Bitmask node child limits
inline constexpr uint32_t BYTE_VALUES          = 1u << CHAR_BIT;            // 256
inline constexpr uint32_t BITMASK_EOS_SLOTS    = 1;                          // one EOS child slot
inline constexpr uint32_t BITMASK_MAX_CHILDREN = BYTE_VALUES + BITMASK_EOS_SLOTS;  // 257

// Parent byte sentinel: node is root (no parent)
inline constexpr uint16_t ROOT_PARENT_BYTE = 256;
// Parent byte for EOS children (end-of-string at a bitmask level)
inline constexpr uint16_t EOS_PARENT_BYTE  = 257;

enum class insert_mode : uint8_t { INSERT, UPSERT, ASSIGN };
enum class insert_outcome : uint8_t { INSERTED, UPDATED, FOUND };
enum class dir_t : int8_t { FWD = +1, BWD = -1 };

struct insert_result {
    uint64_t*      node;
    insert_outcome outcome;
    uint64_t*      leaf = nullptr;   // compact leaf containing the entry
    uint16_t       pos  = 0;         // slot index within leaf
};

enum class erase_status : uint8_t {
    MISSING,    // key not found
    PENDING,    // found but not yet erased
    DONE        // erased
};

struct erase_info {
    uint64_t     desc;   // PENDING: descendant count excluding erased
    erase_status status;
    uint64_t*    leaf;   // PENDING: node containing the entry. DONE: replacement node.
    int          pos;    // PENDING: position in leaf (-1 = eos)
};

struct prefix_erase_result {
    uint64_t* node;     // replacement node (may be sentinel)
    size_t    erased;   // entries erased
};

struct prefix_clone_result {
    uint64_t* cloned;   // cloned subtree root (needs reskip at caller)
    size_t    count;    // entries in clone
    uint32_t  path_len; // mapped bytes consumed before clone root
};

struct prefix_split_result {
    uint64_t* source;   // updated source node
    uint64_t* stolen;   // stolen subtree root (needs reskip at caller)
    size_t    count;    // entries stolen
    uint32_t  path_len; // mapped bytes consumed before stolen root
};

// Collapse: when bitmask total_tail <= COMPACT_KEYSUFFIX_LIMIT, try collapse to compact

// REMOVED VK2_INIT_CAP — cap is now derived from padded allocation in alloc_compact_ks


// Maximum possible node size in allocation units (u64s) is structurally bounded:
// compact ≤ COMPACT_KEYSUFFIX_LIMIT/8 + slots + header; bitmask ≤ BITMASK_MAX_CHILDREN + header.
// Both are well under uint16_t::max (65535).
static_assert(
    (COMPACT_KEYSUFFIX_LIMIT / sizeof(uint64_t) + BITMASK_MAX_CHILDREN + 64)
        <= std::numeric_limits<uint16_t>::max(),
    "structural maximum node size exceeds uint16_t alloc cap");

template <typename ALLOC>
struct kstrie_memory {
    ALLOC alloc_v{};
    kstrie_memory() = default;
    explicit kstrie_memory(const ALLOC& a) : alloc_v(a) {}
    uint64_t* alloc_node(std::size_t needed_u64) {
        std::size_t au = padded_size(static_cast<uint16_t>(needed_u64));
        uint64_t* p = std::allocator_traits<ALLOC>::allocate(alloc_v, au);
        std::memset(p, 0, au * U64_BYTES);
        uint16_t au16 = static_cast<uint16_t>(au);
        std::memcpy(p, &au16, sizeof(au16));
        return p;
    }
    void free_node(uint64_t* p) {
        if (!p) [[unlikely]] return;
        uint16_t au;
        std::memcpy(&au, p, sizeof(au));
        if (au == 0) [[unlikely]] return;
        std::allocator_traits<ALLOC>::deallocate(alloc_v, p, au);
    }
};

// ============================================================================
// kstrie_skip
//
// match_skip_fast: read-path. memcmp, pass/fail.
// match_prefix: write-path. byte-by-byte, returns match_len.
// ============================================================================

template <typename VALUE, typename CHARMAP, typename ALLOC>
struct kstrie_skip {
    using hdr_type = node_header<VALUE, CHARMAP, ALLOC>;

    enum class match_status : uint8_t { MATCHED, MISMATCH, KEY_EXHAUSTED };

    struct match_result {
        match_status status;
        uint32_t     consumed;
        uint32_t     match_len;
    };

    // Read-path: memcmp skip prefix
    static bool match_skip_fast(const uint64_t* node, const hdr_type& h,
                                const uint8_t* key, uint32_t key_len,
                                uint32_t& consumed) noexcept {
        if (!h.has_skip()) [[likely]] return true;
        uint32_t sb = h.skip_bytes();
        if (key_len - consumed < sb) [[unlikely]] return false;
        if (std::memcmp(hdr_type::get_skip(node, h), key + consumed, sb) != 0)
            /* [[unpredictable]] */ return false;
        consumed += sb;
        return true;
    }

    // Read-path hot loop variant: caller guarantees has_skip() is true.
    static bool match_skip_unchecked(const uint64_t* node, const hdr_type& h,
                                     const uint8_t* key, uint32_t key_len,
                                     uint32_t& consumed) noexcept {
        uint32_t sb = h.skip_bytes();
        if (key_len - consumed < sb) [[unlikely]] return false;
        if (std::memcmp(hdr_type::get_skip(node, h), key + consumed, sb) != 0)
            return false;
        consumed += sb;
        return true;
    }

    // Write-path: byte-by-byte with match_len
    static match_result match_prefix(const uint64_t*& node, hdr_type& h,
                                     const uint8_t* mapped_key,
                                     uint32_t key_len, uint32_t consumed) noexcept {
        if (!h.has_skip()) [[likely]]
            return {match_status::MATCHED, consumed, 0};

        uint32_t sb = h.skip_bytes();
        uint32_t remaining = key_len - consumed;
        const uint8_t* prefix = hdr_type::get_skip(const_cast<uint64_t*>(node), h);

        if (remaining < sb) [[unlikely]] {
            uint32_t ml = 0;
            while (ml < remaining && mapped_key[consumed + ml] == prefix[ml]) ++ml;
            return {ml < remaining ? match_status::MISMATCH
                                   : match_status::KEY_EXHAUSTED, consumed, ml};
        }

        uint32_t ml = 0;
        while (ml < sb && mapped_key[consumed + ml] == prefix[ml]) ++ml;
        if (ml < sb) /* [[unpredictable]] */
            return {match_status::MISMATCH, consumed, ml};

        consumed += sb;
        return {match_status::MATCHED, consumed, 0};
    }

    static match_result match_prefix(uint64_t*& node, hdr_type& h,
                                     const uint8_t* mapped_key,
                                     uint32_t key_len, uint32_t consumed) noexcept {
        const uint64_t* cnode = node;
        auto r = match_prefix(cnode, h, mapped_key, key_len, consumed);
        node = const_cast<uint64_t*>(cnode);
        return r;
    }

};

// ============================================================================
// Character mapping helpers (free functions)
// ============================================================================

template <typename CHARMAP>
inline void map_bytes_into(const uint8_t* src, uint8_t* dst,
                           uint32_t len) noexcept {
    for (uint32_t i = 0; i < len; ++i)
        dst[i] = CHARMAP::to_index(src[i]);
}

template <typename CHARMAP>
inline std::pair<const uint8_t*, uint8_t*>
get_mapped(const uint8_t* raw, uint32_t len,
           uint8_t* stack_buf, size_t stack_size) noexcept {
    if constexpr (CHARMAP::IS_IDENTITY) {
        return {raw, nullptr};
    } else {
        uint8_t* hb = (len <= stack_size) ? nullptr : new uint8_t[len];
        uint8_t* buf = hb ? hb : stack_buf;
        map_bytes_into<CHARMAP>(raw, buf, len);
        return {buf, hb};
    }
}

} // namespace gteitelbaum::kstrie_detail

#endif // KSTRIE_SUPPORT_HPP
