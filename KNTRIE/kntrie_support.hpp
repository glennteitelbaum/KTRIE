#ifndef KNTRIE_SUPPORT_HPP
#define KNTRIE_SUPPORT_HPP

#include <cstdint>
#include <cstring>
#include <climits>
#include <bit>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <algorithm>

namespace gteitelbaum::kntrie_detail {

// ==========================================================================
// Constants
// ==========================================================================

template<typename T>
constexpr T ceil_div(T x, T y) noexcept { return (x + y - 1) / y; }

template<typename T>
constexpr T align_up(T x, T alignment) noexcept { return ceil_div(x, alignment) * alignment; }

// --- Fundamental sizes ---
inline constexpr std::size_t U64_BYTES          = sizeof(std::uint64_t);
inline constexpr std::size_t U64_BITS           = sizeof(std::uint64_t) * CHAR_BIT;
inline constexpr std::size_t U64_BITS_MASK      = U64_BITS - 1;
inline constexpr std::size_t U64_BIT_SHIFT      = std::countr_zero(U64_BITS);        // 6
inline constexpr std::size_t U64_TOP_BYTE_SHIFT = U64_BITS - CHAR_BIT;               // 56
inline constexpr int         BYTE_BOUNDARY_MASK = ~(CHAR_BIT - 1);                   // ~7

// --- Type bit widths ---
inline constexpr int U8_BITS  = sizeof(std::uint8_t)  * CHAR_BIT;                    //  8
inline constexpr int U16_BITS = sizeof(std::uint16_t) * CHAR_BIT;                    // 16
inline constexpr int U32_BITS = sizeof(std::uint32_t) * CHAR_BIT;                    // 32

// --- Bitmap ---
inline constexpr std::size_t BITMAP_WORDS      = 4;
inline constexpr std::size_t BITMAP_WORD_MASK  = BITMAP_WORDS - 1;
inline constexpr std::size_t BYTE_VALUES       = 1u << CHAR_BIT;                     // 256

// --- Compact node threshold ---
inline constexpr std::size_t COMPACT_MAX       = 1024;

// --- Bitmask node header ---
inline constexpr std::size_t HEADER_U64        = 2;   // header(1) + parent_ptr(1)
inline constexpr std::size_t BM_PARENT_IDX     = 1;
inline constexpr std::size_t DESC_U64          = 1;   // descendants count at end

// --- Leaf node header sizes ---
// Compact leaf: [header][parent_ptr]
inline constexpr std::size_t COMPACT_HEADER_U64   = 2;
inline constexpr std::size_t COMPACT_PARENT_IDX   = 1;

// Bitmap leaf: [header][parent_ptr][skip_bytes]
inline constexpr std::size_t BITMAP_LEAF_HEADER_U64 = 3;
inline constexpr std::size_t BITMAP_LEAF_PARENT_IDX = 1;
inline constexpr std::size_t BITMAP_LEAF_SKIP_IDX   = 2;

// --- Bitmask node child layout ---
inline constexpr std::size_t BM_SENTINEL_U64   = 1;
inline constexpr std::size_t BM_CHILDREN_START = BITMAP_WORDS + BM_SENTINEL_U64;     // 5

// --- Embed (skip chain) layout ---
inline constexpr std::size_t EMBED_U64         = BITMAP_WORDS + BM_SENTINEL_U64 + 1; // 6
inline constexpr std::size_t EMBED_SENTINEL    = BITMAP_WORDS;                        // 4
inline constexpr std::size_t EMBED_CHILD_PTR   = BITMAP_WORDS + BM_SENTINEL_U64;     // 5

// --- Skip limits ---
inline constexpr std::size_t MAX_SKIP          = 6;
inline constexpr std::size_t MAX_COMBINED_SKIP = MAX_SKIP * 2;

// --- Root structure ---
inline constexpr int ROOT_CONSUMED_BYTES       = 2;

// --- Growth/shrink ---
inline constexpr std::size_t SHRINK_FACTOR     = 2;

// Pointer tagging: bits 62-63 as tag bits.
static_assert(sizeof(std::uintptr_t) <= sizeof(std::uint64_t),
    "kntrie requires uintptr_t fits in uint64_t");

static constexpr std::uint64_t LEAF_BIT      = std::uint64_t(1) << (U64_BITS - 1);
static constexpr std::uint64_t NOT_FOUND_BIT = std::uint64_t(1) << (U64_BITS - 2);

inline bool tst_sign(std::uint64_t v) noexcept {
    return v >> U64_BITS_MASK;
}

// ==========================================================================
// Allocation size classes
// ==========================================================================

inline constexpr std::size_t ALLOC_CLASS_TABLE[] = {
    4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128,
    194, 256, 386, 512, 770, 1024, 1538, 2048,
    3074, 4096, 6146, 8192, 12290, 16384
};
inline constexpr std::size_t NUM_ALLOC_CLASSES = sizeof(ALLOC_CLASS_TABLE) / sizeof(std::size_t);

inline constexpr std::size_t round_up_u64(std::size_t n) noexcept {
    for (std::size_t i = 0; i < NUM_ALLOC_CLASSES; ++i)
        if (n <= ALLOC_CLASS_TABLE[i]) return ALLOC_CLASS_TABLE[i];
    return n;
}

inline constexpr std::size_t MAX_NODE_ALLOC_U64 = 128;

inline constexpr bool should_shrink_u64(std::size_t allocated, std::size_t needed) noexcept {
    return allocated > round_up_u64(needed * SHRINK_FACTOR);
}

// ==========================================================================
// Node Header  (8 bytes = 1 u64)
//
// v2 layout (little-endian):
//   [0]     flags       (is_bitmap:1, skip:3, reserved:4)
//   [1]     reserved
//   [2..3]  entries     (std::uint16_t)
//   [4..5]  alloc_u64   (std::uint16_t — capacity for compact, unused for bitmap)
//   [6..7]  parent_byte (std::uint16_t, dispatch byte in parent bitmask)
//
// skip: only meaningful for bitmap leaves (0-6 prefix bytes to verify).
// Compact leaves: skip is always 0.
// ==========================================================================

struct node_header_t {
    std::uint8_t  flags_v      = 0;  // bit 0: is_bitmap, bits 1-3: skip (bitmap only)
    std::uint8_t  reserved_v   = 0;
    std::uint16_t entries_v    = 0;
    std::uint16_t alloc_u64_v  = 0;
    std::uint16_t parent_byte_v= 0;

    static constexpr std::uint16_t ROOT_BYTE = 256;

    // --- Flags ---
    static constexpr std::uint8_t IS_BITMAP_BIT = 0x01;
    static constexpr std::uint8_t SKIP_SHIFT    = 1;
    static constexpr std::uint8_t SKIP_MASK     = 0x07;  // 3 bits → 0-7

    bool is_bitmap()  const noexcept { return flags_v & IS_BITMAP_BIT; }
    void set_bitmap(bool b) noexcept {
        if (b) flags_v |= IS_BITMAP_BIT;
        else   flags_v &= ~IS_BITMAP_BIT;
    }

    std::uint8_t skip() const noexcept {
        return (flags_v >> SKIP_SHIFT) & SKIP_MASK;
    }
    void set_skip(std::uint8_t s) noexcept {
        flags_v = (flags_v & IS_BITMAP_BIT) | (s << SKIP_SHIFT);
    }
    bool is_skip() const noexcept { return skip() > 0; }

    // --- entries / alloc ---
    unsigned entries()   const noexcept { return entries_v; }
    void set_entries(unsigned n) noexcept { entries_v = static_cast<std::uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_v; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_v = static_cast<std::uint16_t>(n); }

    // --- parent tracking ---
    std::uint16_t parent_byte() const noexcept { return parent_byte_v; }
    void set_parent_byte(std::uint16_t b) noexcept { parent_byte_v = b; }
    bool is_root() const noexcept { return parent_byte_v == ROOT_BYTE; }
};
static_assert(sizeof(node_header_t) == 8);

inline node_header_t*       get_header(std::uint64_t* n)       noexcept { return reinterpret_cast<node_header_t*>(n); }
inline const node_header_t* get_header(const std::uint64_t* n) noexcept { return reinterpret_cast<const node_header_t*>(n); }

// ==========================================================================
// Prefix byte helpers (for bitmask skip chains — still needed)
// ==========================================================================

inline std::uint8_t pfx_byte(std::uint64_t pfx, std::uint8_t i) noexcept {
    return static_cast<std::uint8_t>(pfx >> (U64_TOP_BYTE_SHIFT - CHAR_BIT * i));
}
inline std::uint64_t pack_prefix(const std::uint8_t* bytes, std::uint8_t len) noexcept {
    std::uint64_t v = 0;
    for (std::uint8_t i = 0; i < len; ++i)
        v |= std::uint64_t(bytes[i]) << (U64_TOP_BYTE_SHIFT - CHAR_BIT * i);
    return v;
}
inline void pfx_to_bytes(std::uint64_t pfx, std::uint8_t* out, std::uint8_t len) noexcept {
    for (std::uint8_t i = 0; i < len; ++i)
        out[i] = static_cast<std::uint8_t>(pfx >> (U64_TOP_BYTE_SHIFT - CHAR_BIT * i));
}

// ==========================================================================
// Tagged pointer helpers
// ==========================================================================

inline std::uint64_t tag_leaf(const std::uint64_t* node) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(node)) | LEAF_BIT;
}
inline std::uint64_t tag_bitmask(const std::uint64_t* node) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(node + HEADER_U64));
}
inline const std::uint64_t* untag_leaf(std::uint64_t tagged) noexcept {
    return reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(tagged ^ LEAF_BIT));
}
inline std::uint64_t* untag_leaf_mut(std::uint64_t tagged) noexcept {
    return reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(tagged ^ LEAF_BIT));
}
inline std::uint64_t* bm_to_node(std::uint64_t ptr) noexcept {
    return reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(ptr)) - HEADER_U64;
}
inline const std::uint64_t* bm_to_node_const(std::uint64_t ptr) noexcept {
    return reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(ptr)) - HEADER_U64;
}

// --- Bitmask parent pointer accessors ---
inline std::uint64_t* bm_parent(std::uint64_t* node) noexcept {
    return reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(node[BM_PARENT_IDX]));
}
inline void set_bm_parent(std::uint64_t* node, std::uint64_t* parent) noexcept {
    node[BM_PARENT_IDX] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
}

// --- Leaf parent pointer accessors (slot 1 for both compact and bitmap) ---
inline std::uint64_t* leaf_parent(std::uint64_t* node) noexcept {
    return reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(node[COMPACT_PARENT_IDX]));
}
inline void set_leaf_parent(std::uint64_t* node, std::uint64_t* parent) noexcept {
    node[COMPACT_PARENT_IDX] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
}

// --- Bitmap leaf skip bytes (raw bytes packed in u64 at slot 2) ---
inline std::uint64_t bitmap_leaf_skip_word(const std::uint64_t* node) noexcept {
    return node[BITMAP_LEAF_SKIP_IDX];
}
inline void set_bitmap_leaf_skip_word(std::uint64_t* node, std::uint64_t w) noexcept {
    node[BITMAP_LEAF_SKIP_IDX] = w;
}

// Link a child (leaf or bitmask) to its parent node + dispatch byte.
inline void link_child(std::uint64_t* parent_node, std::uint64_t child_tagged, std::uint8_t byte) noexcept {
    if (child_tagged & LEAF_BIT) {
        if (child_tagged & NOT_FOUND_BIT) return;
        std::uint64_t* leaf = untag_leaf_mut(child_tagged);
        set_leaf_parent(leaf, parent_node);
        get_header(leaf)->set_parent_byte(byte);
    } else {
        std::uint64_t* child_node = reinterpret_cast<std::uint64_t*>(
            static_cast<std::uintptr_t>(child_tagged)) - HEADER_U64;
        set_bm_parent(child_node, parent_node);
        get_header(child_node)->set_parent_byte(byte);
    }
}

inline void set_root_parent(std::uint64_t tagged) noexcept {
    if (tagged & LEAF_BIT) {
        if (tagged & NOT_FOUND_BIT) return;
        get_header(untag_leaf_mut(tagged))->set_parent_byte(node_header_t::ROOT_BYTE);
    } else {
        std::uint64_t* node = reinterpret_cast<std::uint64_t*>(
            static_cast<std::uintptr_t>(tagged)) - HEADER_U64;
        get_header(node)->set_parent_byte(node_header_t::ROOT_BYTE);
    }
}

// Copy leaf header (caller specifies size — compact=2, bitmap=3)
inline void copy_leaf_header(const std::uint64_t* src, std::uint64_t* dst, std::size_t hu) noexcept {
    std::memcpy(dst, src, hu * U64_BYTES);
}

// ==========================================================================
// key_ops<K> — stored key representation
//
// Stored form: sign-flipped, native endian.
// operator< on stored keys gives correct sort order.
// ==========================================================================

template<typename K>
struct key_ops {
    static_assert(std::is_integral_v<K>);

    static constexpr bool IS_SIGNED = std::is_signed_v<K>;
    static constexpr int  KEY_BITS  = static_cast<int>(sizeof(K) * CHAR_BIT);
    static constexpr int  KEY_BYTES = static_cast<int>(sizeof(K));

    // The unsigned type matching K's width — used for stored representation.
    using UK = std::make_unsigned_t<K>;

    // Top-byte shift for dispatch loop
    static constexpr unsigned TOP_BYTE_SHIFT = (sizeof(K) - 1) * CHAR_BIT;

    // Sign flip: convert signed ordering to unsigned ordering.
    // Identity for unsigned types (constexpr folds away).
    static constexpr UK sign_flip(K key) noexcept {
        UK u = static_cast<UK>(key);
        if constexpr (IS_SIGNED)
            u ^= UK(1) << (KEY_BITS - 1);
        return u;
    }

    // Stored form: sign-flipped, native endian.  Binary search uses operator<.
    static constexpr UK to_stored(K user_key) noexcept {
        return sign_flip(user_key);
    }

    // Reverse: stored form → user key.
    static constexpr K to_user(UK stored) noexcept {
        if constexpr (IS_SIGNED)
            stored ^= UK(1) << (KEY_BITS - 1);
        return static_cast<K>(stored);
    }

    // Extract dispatch byte from stored key at a given shift position.
    static constexpr std::uint8_t byte_of(UK stored, unsigned shift) noexcept {
        return static_cast<std::uint8_t>((stored >> shift) & 0xFF);
    }
};

// ==========================================================================
// Result types
// ==========================================================================

enum class dir_t : std::int8_t { FWD = +1, BWD = -1 };

struct leaf_pos_t {
    std::uint64_t* node  = nullptr;
    std::uint16_t  pos   = 0;
    bool           found = false;
};

struct insert_pos_result_t {
    std::uint64_t* leaf     = nullptr;
    std::uint16_t  pos      = 0;
    bool           inserted = false;
};

// Iteration entry — returned by leaf find/advance operations.
// K = stored key type (make_unsigned_t<KEY>).
template<typename UK>
struct iter_entry_t {
    std::uint64_t* leaf  = nullptr;
    std::uint16_t  pos   = 0;       // slot (ordinal index)
    std::uint16_t  bit   = 0;       // byte value (bitmap), unused (compact)
    UK             key   = 0;       // stored form (sign-flipped)
    void*          val   = nullptr;
    bool           found = false;
};

enum class slot_mode { FAST_EXIT, BRANCHLESS, UNFILTERED };

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

struct bitmap_256_t {
    std::uint64_t words[BITMAP_WORDS] = {};

    bool has_bit(std::uint8_t i) const noexcept { return words[i >> U64_BIT_SHIFT] & (1ULL << (i & U64_BITS_MASK)); }
    void set_bit(std::uint8_t i) noexcept { words[i >> U64_BIT_SHIFT] |= (1ULL << (i & U64_BITS_MASK)); }
    void clear_bit(std::uint8_t i) noexcept { words[i >> U64_BIT_SHIFT] &= ~(1ULL << (i & U64_BITS_MASK)); }

    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1]) +
               std::popcount(words[2]) + std::popcount(words[3]);
    }

    template<slot_mode MODE>
    int find_slot(std::uint8_t index) const noexcept {
        const int w = index >> U64_BIT_SHIFT, b = index & U64_BITS_MASK;
        std::uint64_t before = words[w] << (U64_BITS_MASK - b);
        if constexpr (MODE == slot_mode::FAST_EXIT) {
            if (!tst_sign(before)) [[unlikely]] return -1;
        }

        int slot = std::popcount(before);
        slot += std::popcount(words[0]) & -int(w > 0);
        slot += std::popcount(words[1]) & -int(w > 1);
        slot += std::popcount(words[2]) & -int(w > 2);

        if constexpr (MODE == slot_mode::BRANCHLESS)
            slot &= -int(tst_sign(before));
        else if constexpr (MODE == slot_mode::FAST_EXIT)
            slot--;
        else
            slot -= int(tst_sign(before));

        return slot;
    }

    template<typename F>
    void for_each_set(F&& fn) const noexcept {
        int slot = 0;
        for (int w = 0; w < static_cast<int>(BITMAP_WORDS); ++w) {
            std::uint64_t bits = words[w];
            while (bits) {
                int b = std::countr_zero(bits);
                fn(static_cast<std::uint8_t>((w << U64_BIT_SHIFT) + b), slot++);
                bits &= bits - 1;
            }
        }
    }

    std::uint8_t first_set_bit() const noexcept {
        std::uint64_t w0 = words[0], w1 = words[1], w2 = words[2];
        int idx = !w0 + (!w0 & !w1) + (!w0 & !w1 & !w2);
        return static_cast<std::uint8_t>((idx << U64_BIT_SHIFT) + std::countr_zero(words[idx]));
    }

    std::uint8_t last_set_bit() const noexcept {
        std::uint64_t w1 = words[1], w2 = words[2], w3 = words[3];
        int idx = 3 - (!w3 + (!w3 & !w2) + (!w3 & !w2 & !w1));
        return static_cast<std::uint8_t>((idx << U64_BIT_SHIFT) + U64_BITS_MASK - std::countl_zero(words[idx]));
    }

    std::uint8_t select_bit(unsigned n) const noexcept {
        unsigned remaining = n;
        for (unsigned w = 0; w < BITMAP_WORDS; ++w) {
            unsigned pc = static_cast<unsigned>(std::popcount(words[w]));
            if (remaining < pc) {
                std::uint64_t word = words[w];
                for (unsigned i = 0; i < remaining; ++i)
                    word &= word - 1;
                return static_cast<std::uint8_t>(w * U64_BITS +
                    static_cast<unsigned>(std::countr_zero(word)));
            }
            remaining -= pc;
        }
        std::unreachable();
    }

    struct adj_result { std::uint8_t idx; std::uint16_t slot; bool found; };
    struct bit_result { std::uint8_t idx; bool found; };

    bit_result next_bit_after(std::uint8_t idx) const noexcept {
        unsigned next = static_cast<unsigned>(idx) + 1;
        if (next >= BYTE_VALUES) return {0, false};
        unsigned w = next >> U64_BIT_SHIFT;
        unsigned b = next & U64_BITS_MASK;
        std::uint64_t word = words[w] >> b;
        if (word)
            return {static_cast<std::uint8_t>(next + std::countr_zero(word)), true};
        for (unsigned nw = w + 1; nw < BITMAP_WORDS; ++nw) {
            if (words[nw])
                return {static_cast<std::uint8_t>(nw * U64_BITS +
                    std::countr_zero(words[nw])), true};
        }
        return {0, false};
    }

    bit_result prev_bit_before(std::uint8_t idx) const noexcept {
        if (idx == 0) return {0, false};
        unsigned prev = static_cast<unsigned>(idx) - 1;
        unsigned w = prev >> U64_BIT_SHIFT;
        unsigned b = prev & U64_BITS_MASK;
        std::uint64_t word = words[w] & (~std::uint64_t{0} >> (U64_BITS_MASK - b));
        if (word)
            return {static_cast<std::uint8_t>(w * U64_BITS + U64_BITS_MASK -
                static_cast<unsigned>(std::countl_zero(word))), true};
        for (int nw = static_cast<int>(w) - 1; nw >= 0; --nw) {
            if (words[nw])
                return {static_cast<std::uint8_t>(static_cast<unsigned>(nw) * U64_BITS +
                    U64_BITS_MASK - static_cast<unsigned>(
                    std::countl_zero(words[nw]))), true};
        }
        return {0, false};
    }

    adj_result next_set_after(std::uint8_t idx) const noexcept {
        unsigned start = (unsigned(idx) + 1) & (BYTE_VALUES - 1);
        unsigned sw = start >> U64_BIT_SHIFT;
        unsigned sb = start & U64_BITS_MASK;

        std::uint64_t m[BITMAP_WORDS] = {words[0], words[1], words[2], words[3]};
        m[0] &= -std::uint64_t(0 >= sw);
        m[1] &= -std::uint64_t(1 >= sw);
        m[2] &= -std::uint64_t(2 >= sw);
        m[sw & BITMAP_WORD_MASK] &= (~std::uint64_t{0} << sb);

        int fw = !m[0] + (!m[0] & !m[1]) + (!m[0] & !m[1] & !m[2]);
        int biw = std::countr_zero(m[fw]);
        int bit = (fw << U64_BIT_SHIFT) + biw;

        std::uint64_t before = words[fw] << ((U64_BITS_MASK - biw) & U64_BITS_MASK);
        int slot = std::popcount(before) - 1;
        slot += std::popcount(words[0]) & -int(fw > 0);
        slot += std::popcount(words[1]) & -int(fw > 1);
        slot += std::popcount(words[2]) & -int(fw > 2);

        return {static_cast<std::uint8_t>(bit), static_cast<std::uint16_t>(slot),
                static_cast<std::uint8_t>(bit) > idx};
    }

    adj_result prev_set_before(std::uint8_t idx) const noexcept {
        if (idx == 0) return {0, 0, false};
        unsigned last = unsigned(idx) - 1;
        unsigned sw = last >> U64_BIT_SHIFT;
        unsigned sb = last & U64_BITS_MASK;

        std::uint64_t m[BITMAP_WORDS] = {words[0], words[1], words[2], words[3]};
        m[3] &= -std::uint64_t(3 <= sw);
        m[2] &= -std::uint64_t(2 <= sw);
        m[1] &= -std::uint64_t(1 <= sw);
        m[sw & BITMAP_WORD_MASK] &= (2ULL << sb) - 1;

        int lw = 3 - !m[3] - (!m[3] & !m[2]) - (!m[3] & !m[2] & !m[1]);
        int biw = U64_BITS_MASK - std::countl_zero(m[lw]);
        int bit = (lw << U64_BIT_SHIFT) + biw;
        bool found = (m[0] | m[1] | m[2] | m[3]) != 0;

        std::uint64_t before = words[lw] << ((U64_BITS_MASK - biw) & U64_BITS_MASK);
        int slot = std::popcount(before) - 1;
        slot += std::popcount(words[0]) & -int(lw > 0);
        slot += std::popcount(words[1]) & -int(lw > 1);
        slot += std::popcount(words[2]) & -int(lw > 2);

        return {static_cast<std::uint8_t>(bit), static_cast<std::uint16_t>(slot), found};
    }

    static bitmap_256_t from_indices(const std::uint8_t* indices, unsigned count) noexcept {
        bitmap_256_t bm{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    static void arr_fill_sorted(const bitmap_256_t& bm, std::uint64_t* dest,
                                const std::uint8_t* indices, const std::uint64_t* tagged_ptrs,
                                unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            dest[bm.find_slot<slot_mode::UNFILTERED>(indices[i])] = tagged_ptrs[i];
    }

    static void arr_insert(bitmap_256_t& bm, std::uint64_t* arr, unsigned count,
                           std::uint8_t idx, std::uint64_t val) noexcept {
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(std::uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    static void arr_remove(bitmap_256_t& bm, std::uint64_t* arr, unsigned count,
                           int slot, std::uint8_t idx) noexcept {
        std::memmove(arr + slot, arr + slot + 1, (count - 1 - slot) * sizeof(std::uint64_t));
        bm.clear_bit(idx);
    }

    static void arr_copy_insert(const std::uint64_t* old_arr, std::uint64_t* new_arr,
                                unsigned old_count, int isl, std::uint64_t val) noexcept {
        std::memcpy(new_arr, old_arr, isl * sizeof(std::uint64_t));
        new_arr[isl] = val;
        std::memcpy(new_arr + isl + 1, old_arr + isl,
                     (old_count - isl) * sizeof(std::uint64_t));
    }

    static void arr_copy_remove(const std::uint64_t* old_arr, std::uint64_t* new_arr,
                                unsigned old_count, int slot) noexcept {
        std::memcpy(new_arr, old_arr, slot * sizeof(std::uint64_t));
        std::memcpy(new_arr + slot, old_arr + slot + 1,
                     (old_count - 1 - slot) * sizeof(std::uint64_t));
    }
};

// ==========================================================================
// Bool slots — packed bit storage (preserved exactly)
// ==========================================================================

struct bool_slots {
public:
    std::uint64_t* data;

    bool get(unsigned i) const noexcept {
        return (data[i / U64_BITS] >> (i % U64_BITS)) & 1;
    }

    void set(unsigned i, bool v) noexcept {
        unsigned word = i / U64_BITS, bit = i % U64_BITS;
        if (v) data[word] |=  (std::uint64_t{1} << bit);
        else   data[word] &= ~(std::uint64_t{1} << bit);
    }

    void clear_all(unsigned n) noexcept {
        std::memset(data, 0, u64_for(n) * U64_BYTES);
    }

    void unpack_to(bool* dst, unsigned n) const noexcept {
        for (unsigned i = 0; i < n; ++i)
            dst[i] = get(i);
    }

    void pack_from(const bool* src, unsigned n) noexcept {
        clear_all(n);
        for (unsigned i = 0; i < n; ++i)
            if (src[i]) set(i, true);
    }

private:
    static constexpr unsigned WORD_HIGH_BIT = U64_BITS - 1;  // 63

    static constexpr std::uint64_t lo_mask(unsigned b) noexcept {
        return b >= U64_BITS ? ~std::uint64_t{0} : (std::uint64_t{1} << b) - 1;
    }

public:
    void shift_left_1(unsigned from, unsigned count) noexcept {
        if (count == 0) return;
        unsigned dst  = from - 1;
        unsigned end  = from + count - 1;
        unsigned w_lo = dst / U64_BITS;
        unsigned w_hi = end / U64_BITS;
        unsigned b_lo = dst % U64_BITS;
        unsigned b_hi = end % U64_BITS;

        if (w_lo == w_hi) {
            std::uint64_t src_mask  = lo_mask(b_hi + 1) & ~lo_mask(b_lo + 1);
            std::uint64_t dest_bit  = std::uint64_t{1} << b_lo;
            std::uint64_t preserved = data[w_lo] & ~(src_mask | dest_bit);
            data[w_lo] = preserved | ((data[w_lo] & src_mask) >> 1);
            return;
        }

        std::uint64_t save_lo = data[w_lo] & lo_mask(b_lo);
        std::uint64_t save_hi = data[w_hi] & ~lo_mask(b_hi + 1);

        data[w_lo] = (data[w_lo] >> 1) | (data[w_lo + 1] << WORD_HIGH_BIT);
        for (unsigned w = w_lo + 1; w < w_hi; ++w)
            data[w] = (data[w] >> 1) | (data[w + 1] << WORD_HIGH_BIT);
        data[w_hi] >>= 1;

        data[w_lo] = (data[w_lo] & ~lo_mask(b_lo)) | save_lo;
        data[w_hi] = (data[w_hi] & lo_mask(b_hi)) | save_hi;
    }

    void shift_right_1(unsigned from, unsigned count) noexcept {
        if (count == 0) return;
        unsigned src  = from;
        unsigned end  = from + count - 1;
        unsigned w_lo = src / U64_BITS;
        unsigned w_hi = (end + 1) / U64_BITS;
        unsigned b_lo = src % U64_BITS;
        unsigned b_end1 = (end + 1) % U64_BITS;

        if (w_lo == w_hi) {
            std::uint64_t src_mask  = lo_mask(b_end1) & ~lo_mask(b_lo);
            std::uint64_t dest_bit  = std::uint64_t{1} << b_end1;
            std::uint64_t preserved = data[w_lo] & ~(src_mask | dest_bit);
            data[w_lo] = preserved | ((data[w_lo] & src_mask) << 1);
            return;
        }

        std::uint64_t save_lo = data[w_lo] & lo_mask(b_lo);
        std::uint64_t save_hi = data[w_hi] & ~lo_mask(b_end1 + 1);

        data[w_hi] = (data[w_hi] << 1) | (data[w_hi - 1] >> WORD_HIGH_BIT);
        for (int w = static_cast<int>(w_hi) - 1; w > static_cast<int>(w_lo); --w)
            data[w] = (data[w] << 1) | (data[w - 1] >> WORD_HIGH_BIT);
        data[w_lo] <<= 1;

        data[w_lo] = (data[w_lo] & ~lo_mask(b_lo)) | save_lo;
        data[w_hi] = (data[w_hi] & lo_mask(b_end1 + 1)) | save_hi;
    }

    static constexpr std::size_t bytes_for(std::size_t count) noexcept {
        return ceil_div(count, std::size_t{CHAR_BIT});
    }

    static constexpr std::size_t u64_for(std::size_t count) noexcept {
        return ceil_div(count, std::size_t{U64_BITS});
    }
};

// ==========================================================================
// bool_ref
// ==========================================================================

struct bool_ref {
    std::uint64_t* word;
    std::uint8_t   bit;

    operator bool() const noexcept {
        return (*word >> bit) & 1;
    }
    bool_ref& operator=(bool v) noexcept {
        if (v) *word |=  (std::uint64_t{1} << bit);
        else   *word &= ~(std::uint64_t{1} << bit);
        return *this;
    }
    bool_ref& operator=(const bool_ref& o) noexcept {
        return *this = static_cast<bool>(o);
    }
};

// ==========================================================================
// Value traits (preserved exactly)
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct value_traits {
    static constexpr bool IS_TRIVIAL =
        std::is_trivially_copyable_v<VALUE> && sizeof(VALUE) <= U64_BYTES;
    static constexpr bool IS_INLINE = IS_TRIVIAL;
    static constexpr bool HAS_DESTRUCTOR = !IS_TRIVIAL;
    static constexpr bool IS_BOOL = std::is_same_v<VALUE, bool>;

    using slot_type = std::conditional_t<IS_INLINE, VALUE, VALUE*>;

    static slot_type store(const VALUE& val, ALLOC& alloc) {
        if constexpr (IS_TRIVIAL) {
            return val;
        } else {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            VALUE* p = std::allocator_traits<VA>::allocate(va, 1);
            std::allocator_traits<VA>::construct(va, p, val);
            return p;
        }
    }

    static void destroy(slot_type& s, ALLOC& alloc) noexcept {
        if constexpr (!IS_TRIVIAL) {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            std::allocator_traits<VA>::destroy(va, s);
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    static void init_slot(slot_type* dst, const slot_type& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }
    static void init_slot(slot_type* dst, slot_type&& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }

    static void write_slot(slot_type* dst, const slot_type& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }
    static void write_slot(slot_type* dst, slot_type&& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }

    static void open_gap(slot_type* vd, std::size_t count, std::size_t pos) {
        std::memmove(vd + pos + 1, vd + pos,
                     (count - pos) * sizeof(slot_type));
    }

    static void close_gap(slot_type* vd, std::size_t count, std::size_t pos) {
        std::memmove(vd + pos, vd + pos + 1,
                     (count - 1 - pos) * sizeof(slot_type));
    }

    static void copy_uninit(const slot_type* src, std::size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }

    static void move_uninit(slot_type* src, std::size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }
};

// ==========================================================================
// Builder (preserved exactly)
// ==========================================================================

template<typename VALUE, bool IS_TRIVIAL, typename ALLOC>
struct builder;

template<typename VALUE, typename ALLOC>
struct builder<VALUE, true, ALLOC> {
    ALLOC alloc_v;

    builder() : alloc_v() {}
    explicit builder(const ALLOC& a) : alloc_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept : alloc_v(std::move(o.alloc_v)) {}
    builder& operator=(builder&& o) noexcept {
        if (this != &o) alloc_v = std::move(o.alloc_v);
        return *this;
    }

    void swap(builder& o) noexcept {
        using std::swap;
        swap(alloc_v, o.alloc_v);
    }

    const ALLOC& get_allocator() const noexcept { return alloc_v; }

    std::uint64_t* alloc_node(std::size_t& u64_count, bool pad = true) {
        std::size_t actual = pad ? round_up_u64(u64_count) : u64_count;
        std::uint64_t* p = alloc_v.allocate(actual);
        std::memset(p, 0, actual * U64_BYTES);
        u64_count = actual;
        return p;
    }

    void dealloc_node(std::uint64_t* p, std::size_t u64_count) noexcept {
        alloc_v.deallocate(p, u64_count);
    }

    void drain() noexcept {}

    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;

    slot_type store_value(const VALUE& val) { return val; }
    void destroy_value(slot_type&) noexcept {}
};

template<typename VALUE, typename ALLOC>
struct builder<VALUE, false, ALLOC> {
    using BASE = builder<VALUE, true, ALLOC>;
    using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;

    static constexpr std::size_t VAL_U64 = ceil_div(sizeof(VALUE), U64_BYTES);

    BASE base_v;

    builder() = default;
    explicit builder(const ALLOC& a) : base_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept : base_v(std::move(o.base_v)) {}
    builder& operator=(builder&& o) noexcept {
        if (this != &o) base_v = std::move(o.base_v);
        return *this;
    }

    ~builder() = default;

    void swap(builder& o) noexcept { base_v.swap(o.base_v); }
    const ALLOC& get_allocator() const noexcept { return base_v.get_allocator(); }

    std::uint64_t* alloc_node(std::size_t& u64_count, bool pad = true) { return base_v.alloc_node(u64_count, pad); }
    void dealloc_node(std::uint64_t* p, std::size_t u64_count) noexcept { base_v.dealloc_node(p, u64_count); }

    slot_type store_value(const VALUE& val) {
        if constexpr (VAL_U64 <= MAX_NODE_ALLOC_U64) {
            std::size_t sz = VAL_U64;
            std::uint64_t* p = base_v.alloc_node(sz, false);
            std::construct_at(reinterpret_cast<VALUE*>(p), val);
            return reinterpret_cast<VALUE*>(p);
        } else {
            VA va(base_v.get_allocator());
            VALUE* p = std::allocator_traits<VA>::allocate(va, 1);
            std::construct_at(p, val);
            return p;
        }
    }

    void destroy_value(slot_type& s) noexcept {
        std::destroy_at(s);
        if constexpr (VAL_U64 <= MAX_NODE_ALLOC_U64) {
            base_v.dealloc_node(reinterpret_cast<std::uint64_t*>(s), VAL_U64);
        } else {
            VA va(base_v.get_allocator());
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    void drain() noexcept { base_v.drain(); }
};

// ==========================================================================
// Normalized ops VALUE
// ==========================================================================

template<std::size_t N>
using sized_uint_for = std::conditional_t<N <= 1, std::uint8_t,
                       std::conditional_t<N <= 2, std::uint16_t,
                       std::conditional_t<N <= 4, std::uint32_t,
                       std::uint64_t>>>;

template<typename VALUE>
using normalized_ops_value_t =
    std::conditional_t<std::is_same_v<VALUE, bool>, bool,
    std::conditional_t<std::is_trivially_copyable_v<VALUE> && sizeof(VALUE) <= U64_BYTES,
        sized_uint_for<sizeof(VALUE)>,
        VALUE>>;

// ==========================================================================
// Insert/erase result types
// ==========================================================================

struct insert_result_t {
    std::uint64_t tagged_ptr;
    bool inserted;
    bool needs_split;
    const void* existing_value;
    std::uint64_t* leaf;
    std::uint16_t  pos;
};

template<typename UK>
struct erase_result_t {
    std::uint64_t tagged_ptr;
    bool erased;
    std::uint64_t subtree_entries;
    iter_entry_t<UK> next;
};

} // namespace gteitelbaum::kntrie_detail

#endif // KNTRIE_SUPPORT_HPP
