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
#include <cassert>

namespace gteitelbaum {

// ==========================================================================
// Constants
// ==========================================================================

// --- Ceiling division: (x + y - 1) / y ---
template<typename T>
constexpr T ceil_div(T x, T y) noexcept { return (x + y - 1) / y; }

// --- Align up to multiple of alignment ---
template<typename T>
constexpr T align_up(T x, T alignment) noexcept { return ceil_div(x, alignment) * alignment; }

// --- Fundamental sizes (derived, never hardcoded) ---
inline constexpr size_t U64_BYTES          = sizeof(uint64_t);                      // 8
inline constexpr size_t U64_BITS           = sizeof(uint64_t) * CHAR_BIT;           // 64
inline constexpr size_t U64_BITS_MASK      = U64_BITS - 1;                          // 63
inline constexpr size_t U64_BIT_SHIFT      = std::countr_zero(U64_BITS);            // 6
inline constexpr size_t U64_TOP_BYTE_SHIFT = U64_BITS - CHAR_BIT;                  // 56
inline constexpr int    BYTE_BOUNDARY_MASK = ~(CHAR_BIT - 1);                      // ~7

// --- Type bit widths (for NK threshold comparisons) ---
inline constexpr int U8_BITS  = sizeof(uint8_t)  * CHAR_BIT;                       //  8
inline constexpr int U16_BITS = sizeof(uint16_t) * CHAR_BIT;                       // 16
inline constexpr int U32_BITS = sizeof(uint32_t) * CHAR_BIT;                       // 32

// --- Bitmap ---
inline constexpr size_t BITMAP_WORDS   = 4;           // 256-bit bitmap = 4 × u64
inline constexpr size_t BYTE_VALUES    = 1u << CHAR_BIT;  // 256

inline constexpr size_t COMPACT_MAX    = 4096;
inline constexpr size_t HEADER_U64     = 1;            // bitmask node header is 1 u64
inline constexpr unsigned SCAN_MAX     = 16;           // TS ≤ 16: linear scan; > 16: binary search
inline constexpr size_t DESC_U64       = 1;            // descendants count at end of bitmask node

// --- Leaf node layout indices ---
inline constexpr size_t LEAF_HDR_IDX    = 0;           // node_header_t
inline constexpr size_t LEAF_FIND_FN    = 1;           // find exact match fn
inline constexpr size_t LEAF_NEXT_FN    = 2;           // iter next fn
inline constexpr size_t LEAF_PREV_FN    = 3;           // iter prev fn
inline constexpr size_t LEAF_FIRST_FN   = 4;           // descend first (min entry) fn
inline constexpr size_t LEAF_LAST_FN    = 5;           // descend last (max entry) fn
inline constexpr size_t LEAF_PREFIX     = 6;           // left-aligned key prefix
inline constexpr size_t LEAF_HEADER_U64 = 7;           // = LEAF_PREFIX + 1

// --- Bitmask node child layout ---
inline constexpr size_t BM_SENTINEL_U64   = 1;         // sentinel child at children[0]
inline constexpr size_t BM_CHILDREN_START = BITMAP_WORDS + BM_SENTINEL_U64;  // 5

// --- Embed (skip chain) layout ---
// Each embed: [bitmap(BITMAP_WORDS)][sentinel(1)][child_ptr(1)]
inline constexpr size_t EMBED_U64       = BITMAP_WORDS + BM_SENTINEL_U64 + 1;  // 6
inline constexpr size_t EMBED_SENTINEL  = BITMAP_WORDS;                         // 4
inline constexpr size_t EMBED_CHILD_PTR = BITMAP_WORDS + BM_SENTINEL_U64;      // 5

// --- Skip limits ---
inline constexpr size_t MAX_SKIP          = 6;         // max skip bytes per leaf (3-bit field)
inline constexpr size_t MAX_COMBINED_SKIP = MAX_SKIP * 2;

// --- Root structure ---
inline constexpr int ROOT_CONSUMED_BYTES = 2;          // 1 root dispatch byte + 1 leaf minimum

// --- Compact thresholds ---
inline constexpr size_t DUP_SCAN_MAX = 64;             // linear dup scan threshold

// --- Growth/shrink ---
inline constexpr size_t SHRINK_FACTOR = 2;

// Tagged pointer: sign bit = leaf, bit 62 = not found (sentinel)
static constexpr uint64_t LEAF_BIT      = uint64_t(1) << (U64_BITS - 1);
static constexpr uint64_t NOT_FOUND_BIT = uint64_t(1) << (U64_BITS - 2);

// Sign-bit test for branchless bitmap
inline bool tst_sign(uint64_t v) noexcept {
    return v >> U64_BITS_MASK;
}

// (NK narrowing aliases removed — u64-everywhere: routing uses uint64_t,
//  NK only at leaf storage boundary via nk_for_bits_t<BITS>)

// ==========================================================================
// Freelist size classes
//
// ==========================================================================
// Allocation size classes (1.5x growth scheme)
//
// ≤128 u64s: step sizes for in-place growth padding.
// >128 u64s: power-of-2 with midpoints.
// ==========================================================================

inline constexpr size_t FREE_MAX  = 128;
inline constexpr size_t NUM_BINS  = 12;
inline constexpr size_t BIN_SIZES[NUM_BINS] = {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128};

inline constexpr size_t round_up_u64(size_t n) noexcept {
    if (n <= FREE_MAX) {
        for (size_t i = 0; i < NUM_BINS; ++i)
            if (n <= BIN_SIZES[i]) return BIN_SIZES[i];
    }
    int bit  = static_cast<int>(std::bit_width(n - 1));
    size_t pow2 = size_t{1} << bit;
    size_t mid  = pow2 / 2 + pow2 / 4 + 2;  // +2: ensures mid > pow2/2 for smallest sizes
    return (n <= mid) ? mid : pow2;
}

// Shrink when allocated exceeds the class for SHRINK_FACTOR × the needed size.
inline constexpr bool should_shrink_u64(size_t allocated, size_t needed) noexcept {
    return allocated > round_up_u64(needed * SHRINK_FACTOR);
}

// ==========================================================================
// Node Header  (8 bytes = 1 u64)
//
// Struct layout (little-endian):
//   [0..1]   depth_v     (packed depth_t: is_skip:1 skip:3 consumed:6 shift:6)
//   [2..3]   entries     (uint16_t)
//   [4..5]   alloc_u64   (uint16_t)
//   [6..7]   total_slots (uint16_t, compact leaf slot count)
//
// depth_t fields:
//   - Leaf: full depth info (is_skip, skip, consumed, shift)
//   - Bitmask: only skip meaningful (# embedded bo<1> nodes)
//
// Zeroed header -> is_leaf=true, skip=0,
//                  entries=0. Sentinel-safe.
// ==========================================================================

// --- depth_t: 16-bit leaf depth descriptor (pure value type) ---
// No node coupling. Suffix math + skip mask only.
struct depth_t {
    uint16_t is_skip  : 1;   // redundant (skip != 0) but cleaner branch
    uint16_t skip     : 3;   // prefix bytes to compare (0-6)
    uint16_t consumed : 6;   // bits resolved above this leaf (0-56)
    uint16_t shift    : 6;   // = 64 - NK_BITS, only {0, 32, 48, 56}
    //                  16 bits total, no padding

    // HOT: extract suffix from root-level ik. shlx + shrx on x86-64-v3.
    uint64_t suffix(uint64_t ik) const noexcept {
        return (ik << consumed) >> shift;
    }

    // Cold (iter): reconstruct full IK from prefix + suffix
    uint64_t to_ik(uint64_t prefix, uint64_t suf) const noexcept {
        return prefix | ((suf << shift) >> consumed);
    }

    // Cold (iter): maximum suffix value for this leaf's NK type
    uint64_t nk_max() const noexcept {
        return (shift == 0) ? ~uint64_t(0) : (uint64_t(1) << (U64_BITS - shift)) - 1;
    }

    // Cold (unlikely skip path): mask for skip-byte comparison
    uint64_t skip_mask() const noexcept {
        uint8_t lo = U64_BITS - consumed;
        uint8_t hi = lo + skip * CHAR_BIT;
        uint64_t upper = (hi >= U64_BITS) ? 0 : (~uint64_t(0) << hi);
        return upper ^ (~uint64_t(0) << lo);
    }
};
static_assert(sizeof(depth_t) == 2);

struct node_header_t {
    uint16_t depth_v       = 0;  // packed depth_t (is_skip:1 skip:3 consumed:6 shift:6)
    uint16_t entries_v     = 0;
    uint16_t alloc_u64_v   = 0;
    uint16_t total_slots_v = 0;

    // --- depth (leaf: full info; bitmask: only skip used) ---
    depth_t depth() const noexcept {
        return std::bit_cast<depth_t>(depth_v);
    }
    void set_depth(depth_t d) noexcept {
        depth_v = std::bit_cast<uint16_t>(d);
    }

    // --- skip: bitmask node compat + write paths ---
    uint8_t skip()    const noexcept { return depth().skip; }
    bool    is_skip() const noexcept { return depth().is_skip; }
    void set_skip(uint8_t s) noexcept {
        set_depth(depth_t{s > 0, static_cast<uint16_t>(s), 0, 0});
    }

    // --- entries / alloc ---
    unsigned entries()   const noexcept { return entries_v; }
    void set_entries(unsigned n) noexcept { entries_v = static_cast<uint16_t>(n); }

    unsigned alloc_u64() const noexcept { return alloc_u64_v; }
    void set_alloc_u64(unsigned n) noexcept { alloc_u64_v = static_cast<uint16_t>(n); }

    unsigned total_slots() const noexcept { return total_slots_v; }
    void set_total_slots(unsigned n) noexcept { total_slots_v = static_cast<uint16_t>(n); }
};
static_assert(sizeof(node_header_t) == 8);

inline node_header_t*       get_header(uint64_t* n)       noexcept { return reinterpret_cast<node_header_t*>(n); }
inline const node_header_t* get_header(const uint64_t* n) noexcept { return reinterpret_cast<const node_header_t*>(n); }

// --- Prefix u64 helpers (byte 0 at bits 63..56) ---
inline uint8_t pfx_byte(uint64_t pfx, uint8_t i) noexcept {
    return static_cast<uint8_t>(pfx >> (U64_TOP_BYTE_SHIFT - CHAR_BIT * i));
}
inline uint64_t pack_prefix(const uint8_t* bytes, uint8_t len) noexcept {
    uint64_t v = 0;
    for (uint8_t i = 0; i < len; ++i)
        v |= uint64_t(bytes[i]) << (U64_TOP_BYTE_SHIFT - CHAR_BIT * i);
    return v;
}
// Extract bytes from packed u64 (for bitmask interface boundary)
inline void pfx_to_bytes(uint64_t pfx, uint8_t* out, uint8_t len) noexcept {
    for (uint8_t i = 0; i < len; ++i)
        out[i] = static_cast<uint8_t>(pfx >> (U64_TOP_BYTE_SHIFT - CHAR_BIT * i));
}

// --- Skip prefix comparison helpers ---

// XOR-masked diff within the skip region
inline uint64_t skip_diff(uint64_t prefix, depth_t d, uint64_t ik) noexcept {
    return (ik ^ prefix) & d.skip_mask();
}

// Byte-aligned bit position of first differing byte in a diff word
inline int first_diff_shift(uint64_t diff) noexcept {
    return std::countl_zero(diff) & BYTE_BOUNDARY_MASK;
}

// Extract the byte at a given bit shift position (shift=0 → byte at bits 63..56)
inline uint8_t byte_at(uint64_t val, int shift) noexcept {
    return static_cast<uint8_t>(val >> (U64_TOP_BYTE_SHIFT - shift));
}

// HOT: skip bytes match? (find_loop only needs bool)
inline bool skip_eq(uint64_t prefix, depth_t d, uint64_t ik) noexcept {
    return skip_diff(prefix, d, ik) == 0;
}

// Cold: skip byte comparison direction for iter (-1, 0, +1)
inline int skip_cmp(uint64_t prefix, depth_t d, uint64_t ik) noexcept {
    uint64_t diff = skip_diff(prefix, d, ik);
    if (!diff) return 0;
    int s = first_diff_shift(diff);
    return (byte_at(ik, s) < byte_at(prefix, s)) ? -1 : 1;
}

// --- NK type for a given remaining bit count ---
template<int BITS>
using nk_for_bits_t = std::conditional_t<(BITS > U32_BITS), uint64_t,
                      std::conditional_t<(BITS > U16_BITS), uint32_t,
                      std::conditional_t<(BITS > U8_BITS), uint16_t, uint8_t>>>;


// --- Tagged pointer helpers ---
// Bitmask ptr: points to bitmap (node+1), no LEAF_BIT. Use directly.
// Leaf ptr: points to header (node+0), has LEAF_BIT. Strip unconditionally.

inline uint64_t tag_leaf(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t>(node) | LEAF_BIT;
}
inline uint64_t tag_bitmask(const uint64_t* node) noexcept {
    return reinterpret_cast<uint64_t>(node + 1);  // skip header, point at bitmap
}
inline const uint64_t* untag_leaf(uint64_t tagged) noexcept {
    return reinterpret_cast<const uint64_t*>(tagged ^ LEAF_BIT);
}
inline uint64_t* untag_leaf_mut(uint64_t tagged) noexcept {
    return reinterpret_cast<uint64_t*>(tagged ^ LEAF_BIT);
}
inline uint64_t* bm_to_node(uint64_t ptr) noexcept {
    return reinterpret_cast<uint64_t*>(ptr) - 1;  // back up from bitmap to header
}
inline const uint64_t* bm_to_node_const(uint64_t ptr) noexcept {
    return reinterpret_cast<const uint64_t*>(ptr) - 1;
}

// Dynamic header size: only for bitmask nodes (always 1 u64).
// Leaves always use LEAF_HEADER_U64 = 7.

// ==========================================================================
// Leaf result types
// ==========================================================================

// Leaf result: full reconstructed key + value.
template<typename VALUE>
struct leaf_result_t {
    uint64_t      key;     // full IK, left-aligned in u64
    const VALUE*  value;
    bool          found;
};

// find_fn: exact match. Returns value ptr (null = miss).
template<typename VALUE>
using find_fn_t = const VALUE* (*)(const uint64_t*, uint64_t) noexcept;

// iter fn: directional search. Returns {key, value, found}.
template<typename VALUE>
using iter_fn_t = leaf_result_t<VALUE> (*)(const uint64_t*, uint64_t) noexcept;

// edge fn: min/max entry. No search key needed.
template<typename VALUE>
using edge_fn_t = leaf_result_t<VALUE> (*)(const uint64_t*) noexcept;

// ==========================================================================
// Leaf node accessors
// ==========================================================================

inline uint64_t leaf_prefix(const uint64_t* node) noexcept {
    return node[LEAF_PREFIX];
}
inline void set_leaf_prefix(uint64_t* node, uint64_t pfx) noexcept {
    node[LEAF_PREFIX] = pfx;
}

// Fn pointer accessors
template<typename VALUE>
inline find_fn_t<VALUE> get_find_fn(const uint64_t* node) noexcept {
    return reinterpret_cast<find_fn_t<VALUE>>(node[LEAF_FIND_FN]);
}
template<typename VALUE>
inline void set_find_fn(uint64_t* node, find_fn_t<VALUE> fn) noexcept {
    node[LEAF_FIND_FN] = reinterpret_cast<uint64_t>(fn);
}

template<typename VALUE>
inline iter_fn_t<VALUE> get_find_next(const uint64_t* node) noexcept {
    return reinterpret_cast<iter_fn_t<VALUE>>(node[LEAF_NEXT_FN]);
}
template<typename VALUE>
inline void set_find_next(uint64_t* node, iter_fn_t<VALUE> fn) noexcept {
    node[LEAF_NEXT_FN] = reinterpret_cast<uint64_t>(fn);
}

template<typename VALUE>
inline iter_fn_t<VALUE> get_find_prev(const uint64_t* node) noexcept {
    return reinterpret_cast<iter_fn_t<VALUE>>(node[LEAF_PREV_FN]);
}
template<typename VALUE>
inline void set_find_prev(uint64_t* node, iter_fn_t<VALUE> fn) noexcept {
    node[LEAF_PREV_FN] = reinterpret_cast<uint64_t>(fn);
}

template<typename VALUE>
inline edge_fn_t<VALUE> get_find_first(const uint64_t* node) noexcept {
    return reinterpret_cast<edge_fn_t<VALUE>>(node[LEAF_FIRST_FN]);
}
template<typename VALUE>
inline void set_find_first(uint64_t* node, edge_fn_t<VALUE> fn) noexcept {
    node[LEAF_FIRST_FN] = reinterpret_cast<uint64_t>(fn);
}

template<typename VALUE>
inline edge_fn_t<VALUE> get_find_last(const uint64_t* node) noexcept {
    return reinterpret_cast<edge_fn_t<VALUE>>(node[LEAF_LAST_FN]);
}
template<typename VALUE>
inline void set_find_last(uint64_t* node, edge_fn_t<VALUE> fn) noexcept {
    node[LEAF_LAST_FN] = reinterpret_cast<uint64_t>(fn);
}

// --- depth_t node accessors ---
inline depth_t get_depth(const uint64_t* node) noexcept {
    return get_header(node)->depth();
}
inline void set_depth(uint64_t* node, depth_t d) noexcept {
    get_header(node)->set_depth(d);
}

// Copy full leaf header from src to dst
inline void copy_leaf_header(const uint64_t* src, uint64_t* dst) noexcept {
    std::memcpy(dst, src, LEAF_HEADER_U64 * U64_BYTES);
}

// ==========================================================================
// Global sentinel -- zeroed block, valid as:
//   - Leaf with entries=0 -> find returns nullptr
//   - Branchless miss target -> bitmap all zeros -> FAST_EXIT returns -1
// ==========================================================================

// Old SENTINEL_TAGGED removed — each kntrie_impl defines its own sentinel
// with proper fn pointers for branchless dispatch.

// ==========================================================================
// Tagged pointer entry counting (NK-independent)
// ==========================================================================

// ==========================================================================
// key_ops<KEY> -- internal key representation
//
// IK: uint32_t for KEY <= 32 bits, uint64_t otherwise.
// Key is left-aligned in IK. Top bits consumed first via shift.
// ==========================================================================

template<typename KEY>
struct key_ops {
    static_assert(std::is_integral_v<KEY>);

    static constexpr bool   IS_SIGNED = std::is_signed_v<KEY>;
    static constexpr int    KEY_BITS  = static_cast<int>(sizeof(KEY) * CHAR_BIT);

    using IK = std::conditional_t<sizeof(KEY) <= sizeof(uint32_t), uint32_t, uint64_t>;
    static constexpr int IK_BITS = static_cast<int>(sizeof(IK) * CHAR_BIT);

    static constexpr IK to_internal(KEY k) noexcept {
        IK r;
        if constexpr (sizeof(KEY) == sizeof(uint8_t))       r = static_cast<uint8_t>(k);
        else if constexpr (sizeof(KEY) == sizeof(uint16_t)) r = static_cast<uint16_t>(k);
        else if constexpr (sizeof(KEY) == sizeof(uint32_t)) r = static_cast<uint32_t>(k);
        else                                                r = static_cast<uint64_t>(k);
        if constexpr (IS_SIGNED) r ^= IK{1} << (KEY_BITS - 1);
        r <<= (IK_BITS - KEY_BITS);
        return r;
    }

    static constexpr KEY to_key(IK ik) noexcept {
        ik >>= (IK_BITS - KEY_BITS);
        if constexpr (IS_SIGNED) ik ^= IK{1} << (KEY_BITS - 1);
        return static_cast<KEY>(ik);
    }
};

// ==========================================================================
// Iteration result (free struct, shared across all NK specializations)
// ==========================================================================

template<typename IK, typename VST>
struct iter_ops_result_t {
    IK key;
    const VST* value;
    bool found;
};

// ==========================================================================
// 256-bit bitmap
// ==========================================================================

enum class slot_mode { FAST_EXIT, BRANCHLESS, UNFILTERED };

struct bitmap_256_t {
    uint64_t words[BITMAP_WORDS] = {};

    bool has_bit(uint8_t i) const noexcept { return words[i >> U64_BIT_SHIFT] & (1ULL << (i & U64_BITS_MASK)); }
    void set_bit(uint8_t i) noexcept { words[i >> U64_BIT_SHIFT] |= (1ULL << (i & U64_BITS_MASK)); }
    void clear_bit(uint8_t i) noexcept { words[i >> U64_BIT_SHIFT] &= ~(1ULL << (i & U64_BITS_MASK)); }

    int popcount() const noexcept {
        return std::popcount(words[0]) + std::popcount(words[1]) +
               std::popcount(words[2]) + std::popcount(words[3]);
    }

    // Extract the index of the single set bit (for embed chain walking)
    uint8_t single_bit_index() const noexcept {
        uint64_t w0 = words[0], w1 = words[1], w2 = words[2];
        int idx = !w0 + (!w0 & !w1) + (!w0 & !w1 & !w2);
        return static_cast<uint8_t>((idx << U64_BIT_SHIFT) + std::countr_zero(words[idx]));
    }

    // FAST_EXIT:   returns slot (>=0) if bit set, -1 if not set
    // BRANCHLESS:  returns slot if bit set, 0 (sentinel) if not set
    // UNFILTERED:  returns count of set bits below index (for insert position)
    template<slot_mode MODE>
    int find_slot(uint8_t index) const noexcept {
        const int w = index >> U64_BIT_SHIFT, b = index & U64_BITS_MASK;
        uint64_t before = words[w] << (U64_BITS_MASK - b);
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

    // Iterate all set bits, calling fn(uint8_t bit_index, int slot) in order.
    // Single pass: each word visited once, each bit popped with clear-lowest.
    template<typename F>
    void for_each_set(F&& fn) const noexcept {
        int slot = 0;
        for (int w = 0; w < static_cast<int>(BITMAP_WORDS); ++w) {
            uint64_t bits = words[w];
            while (bits) {
                int b = std::countr_zero(bits);
                fn(static_cast<uint8_t>((w << U64_BIT_SHIFT) + b), slot++);
                bits &= bits - 1;
            }
        }
    }

    // Return the lowest set bit index. Undefined if bitmap is empty.
    uint8_t first_set_bit() const noexcept {
        uint64_t w0 = words[0], w1 = words[1], w2 = words[2];
        int idx = !w0 + (!w0 & !w1) + (!w0 & !w1 & !w2);
        return static_cast<uint8_t>((idx << U64_BIT_SHIFT) + std::countr_zero(words[idx]));
    }

    // Return the highest set bit index. Undefined if bitmap is empty.
    uint8_t last_set_bit() const noexcept {
        uint64_t w1 = words[1], w2 = words[2], w3 = words[3];
        int idx = 3 - (!w3 + (!w3 & !w2) + (!w3 & !w2 & !w1));
        return static_cast<uint8_t>((idx << U64_BIT_SHIFT) + U64_BITS_MASK - std::countl_zero(words[idx]));
    }

    struct adj_result { uint8_t idx; uint16_t slot; bool found; };

    // Find smallest set bit > idx, with its slot. Fully branchless.
    adj_result next_set_after(uint8_t idx) const noexcept {
        unsigned start = (unsigned(idx) + 1) & (BYTE_VALUES - 1);
        unsigned sw = start >> U64_BIT_SHIFT;
        unsigned sb = start & U64_BITS_MASK;

        // Zero words before sw, partial-mask only the start word
        uint64_t m[BITMAP_WORDS] = {words[0], words[1], words[2], words[3]};
        m[0] &= -uint64_t(0 >= sw);
        m[1] &= -uint64_t(1 >= sw);
        m[2] &= -uint64_t(2 >= sw);
        m[sw & 3] &= (~0ULL << sb);

        // First non-zero word
        int fw = !m[0] + (!m[0] & !m[1]) + (!m[0] & !m[1] & !m[2]);
        int biw = std::countr_zero(m[fw]);
        int bit = (fw << U64_BIT_SHIFT) + biw;

        // Slot via branchless prefix popcount
        uint64_t before = words[fw] << ((U64_BITS_MASK - biw) & U64_BITS_MASK);
        int slot = std::popcount(before) - 1;
        slot += std::popcount(words[0]) & -int(fw > 0);
        slot += std::popcount(words[1]) & -int(fw > 1);
        slot += std::popcount(words[2]) & -int(fw > 2);

        return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot),
                static_cast<uint8_t>(bit) > idx};
    }

    // Find largest set bit < idx, with its slot. Fully branchless.
    adj_result prev_set_before(uint8_t idx) const noexcept {
        if (idx == 0) return {0, 0, false};
        unsigned last = unsigned(idx) - 1;
        unsigned sw = last >> U64_BIT_SHIFT;
        unsigned sb = last & U64_BITS_MASK;

        // Zero words after sw, partial-mask only the start word
        uint64_t m[BITMAP_WORDS] = {words[0], words[1], words[2], words[3]};
        m[3] &= -uint64_t(3 <= sw);
        m[2] &= -uint64_t(2 <= sw);
        m[1] &= -uint64_t(1 <= sw);
        m[sw & 3] &= (2ULL << sb) - 1;

        // Last non-zero word (search from high to low)
        int lw = 3 - !m[3] - (!m[3] & !m[2]) - (!m[3] & !m[2] & !m[1]);
        int biw = U64_BITS_MASK - std::countl_zero(m[lw] | 1);  // |1 prevents UB on zero
        int bit = (lw << U64_BIT_SHIFT) + biw;
        bool found = (m[0] | m[1] | m[2] | m[3]) != 0;

        // Slot via branchless prefix popcount
        uint64_t before = words[lw] << ((U64_BITS_MASK - biw) & U64_BITS_MASK);
        int slot = std::popcount(before) - 1;
        slot += std::popcount(words[0]) & -int(lw > 0);
        slot += std::popcount(words[1]) & -int(lw > 1);
        slot += std::popcount(words[2]) & -int(lw > 2);

        return {static_cast<uint8_t>(bit), static_cast<uint16_t>(slot), found};
    }

    static bitmap_256_t from_indices(const uint8_t* indices, unsigned count) noexcept {
        bitmap_256_t bm{};
        for (unsigned i = 0; i < count; ++i) bm.set_bit(indices[i]);
        return bm;
    }

    // Fill dest in bitmap order from unsorted (indices, tagged_ptrs)
    static void arr_fill_sorted(const bitmap_256_t& bm, uint64_t* dest,
                                const uint8_t* indices, const uint64_t* tagged_ptrs,
                                unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            dest[bm.find_slot<slot_mode::UNFILTERED>(indices[i])] = tagged_ptrs[i];
    }

    // In-place insert: memmove right, write new entry, set bit
    static void arr_insert(bitmap_256_t& bm, uint64_t* arr, unsigned count,
                           uint8_t idx, uint64_t val) noexcept {
        int isl = bm.find_slot<slot_mode::UNFILTERED>(idx);
        std::memmove(arr + isl + 1, arr + isl, (count - isl) * sizeof(uint64_t));
        arr[isl] = val;
        bm.set_bit(idx);
    }

    // In-place remove: memmove left, clear bit
    static void arr_remove(bitmap_256_t& bm, uint64_t* arr, unsigned count,
                           int slot, uint8_t idx) noexcept {
        std::memmove(arr + slot, arr + slot + 1, (count - 1 - slot) * sizeof(uint64_t));
        bm.clear_bit(idx);
    }

    // Copy old array into new array with a new entry inserted
    static void arr_copy_insert(const uint64_t* old_arr, uint64_t* new_arr,
                                unsigned old_count, int isl, uint64_t val) noexcept {
        std::memcpy(new_arr, old_arr, isl * sizeof(uint64_t));
        new_arr[isl] = val;
        std::memcpy(new_arr + isl + 1, old_arr + isl,
                     (old_count - isl) * sizeof(uint64_t));
    }

    // Copy old array into new array with one entry removed
    static void arr_copy_remove(const uint64_t* old_arr, uint64_t* new_arr,
                                unsigned old_count, int slot) noexcept {
        std::memcpy(new_arr, old_arr, slot * sizeof(uint64_t));
        std::memcpy(new_arr + slot, old_arr + slot + 1,
                     (old_count - 1 - slot) * sizeof(uint64_t));
    }
};

// ==========================================================================
// Bool slots — packed bit storage for VALUE=bool specialization
//
// Wraps a uint64_t* pointing into a node's packed bit region.
// Sentinels are private — all const bool* returns go through ptr/ptr_at.
// ==========================================================================

struct bool_slots {
private:
    static inline constexpr bool TRUE_VAL  = true;
    static inline constexpr bool FALSE_VAL = false;

public:
    uint64_t* data;

    static const bool* ptr(bool v) noexcept {
        return v ? &TRUE_VAL : &FALSE_VAL;
    }

    const bool* ptr_at(unsigned i) const noexcept {
        return ptr((data[i / U64_BITS] >> (i % U64_BITS)) & 1);
    }

    bool get(unsigned i) const noexcept {
        return (data[i / U64_BITS] >> (i % U64_BITS)) & 1;
    }

    void set(unsigned i, bool v) noexcept {
        unsigned word = i / U64_BITS, bit = i % U64_BITS;
        if (v) data[word] |=  (uint64_t{1} << bit);
        else   data[word] &= ~(uint64_t{1} << bit);
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

    // Shift bits [from, from+count) left by 1 (toward lower index).
    // Bit at 'from' is overwritten. Bit at from+count is unchanged.
    // Equivalent to memmove(vd + from - 1, vd + from, count) for 1-byte slots.
    void shift_left_1(unsigned from, unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            set(from - 1 + i, get(from + i));
    }

    // Shift bits [from, from+count) right by 1 (toward higher index).
    // Bit at from+count is overwritten. Bit at 'from' is freed.
    // Equivalent to memmove(vd + from + 1, vd + from, count) for 1-byte slots.
    void shift_right_1(unsigned from, unsigned count) noexcept {
        for (unsigned i = count; i > 0; --i)
            set(from + i, get(from + i - 1));
    }

    // Fill bits [from, from+count) with value v.
    void fill_range(unsigned from, unsigned count, bool v) noexcept {
        for (unsigned i = 0; i < count; ++i)
            set(from + i, v);
    }

    static constexpr size_t u64_for(unsigned n) noexcept {
        return ceil_div(size_t(n), U64_BITS);
    }

    static constexpr size_t bytes_for(unsigned n) noexcept {
        return u64_for(n) * U64_BYTES;
    }
};

// ==========================================================================
// Value traits
// ==========================================================================

template<typename VALUE, typename ALLOC>
struct value_traits {
    // Two categories:
    //   A: trivially_copyable && sizeof <= 64  → inline, memcpy-safe, no dtor
    //   C: else                                → pointer, has dtor+dealloc
    static constexpr bool IS_TRIVIAL =
        std::is_trivially_copyable_v<VALUE> && sizeof(VALUE) <= U64_BYTES;
    static constexpr bool IS_INLINE = IS_TRIVIAL;
    static constexpr bool HAS_DESTRUCTOR = !IS_TRIVIAL;
    static constexpr bool IS_BOOL = std::is_same_v<VALUE, bool>;

    using slot_type = std::conditional_t<IS_INLINE, VALUE, VALUE*>;

    // --- store: VALUE → slot_type (for insert) ---

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

    // --- as_ptr: slot_type → const VALUE* ---

    static const VALUE* as_ptr(const slot_type& s) noexcept {
        if constexpr (IS_BOOL)        return bool_slots::ptr(s);
        else if constexpr (IS_TRIVIAL) return reinterpret_cast<const VALUE*>(&s);
        else                           return s;
    }

    // --- destroy: release resources held by slot ---
    //   A: noop.  C: call destructor + deallocate.

    static void destroy(slot_type& s, ALLOC& alloc) noexcept {
        if constexpr (!IS_TRIVIAL) {
            using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
            VA va(alloc);
            std::allocator_traits<VA>::destroy(va, s);
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    // --- init_slot: write into UNINITIALIZED destination ---
    // Both A and C are pointer-sized or trivial — always memcpy.

    static void init_slot(slot_type* dst, const slot_type& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }

    static void init_slot(slot_type* dst, slot_type&& val) {
        std::memcpy(dst, &val, sizeof(slot_type));
    }

    // --- write_slot: write into INITIALIZED (live or moved-from) destination ---

    static void write_slot(slot_type* dst, const slot_type& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }

    static void write_slot(slot_type* dst, slot_type&& src) noexcept {
        std::memcpy(dst, &src, sizeof(slot_type));
    }

    // --- open_gap: shift right, create uninit hole at pos ---

    static void open_gap(slot_type* vd, size_t count, size_t pos) {
        std::memmove(vd + pos + 1, vd + pos,
                     (count - pos) * sizeof(slot_type));
    }

    // --- close_gap: remove element at pos, shift left ---
    // For C: caller MUST VT::destroy(vd[pos]) first (dealloc pointer).
    // After: count-1 live elements.

    static void close_gap(slot_type* vd, size_t count, size_t pos) {
        std::memmove(vd + pos, vd + pos + 1,
                     (count - 1 - pos) * sizeof(slot_type));
    }

    // --- copy_uninit: copy n slots to UNINIT destination, no overlap ---

    static void copy_uninit(const slot_type* src, size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }

    // --- move_uninit: move n slots to UNINIT destination, no overlap ---

    static void move_uninit(slot_type* src, size_t n, slot_type* dst) {
        std::memcpy(dst, src, n * sizeof(slot_type));
    }
};

// ==========================================================================
// Builder — owns allocator, handles node + value alloc/dealloc
// ==========================================================================

// Base builder: trivial values (A-type, bool) — bump-from-mega slab allocator
//
// Two-level allocation:
//   mega (growing) → blocks (bin-sized, bump or freelist)
//
// Hot path: pop from bins_v[b].
// Cold path: bump mega cursor by bin size.
// Colder:    allocate new mega when current is exhausted.
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
        if (this != &o)
            alloc_v = std::move(o.alloc_v);
        return *this;
    }

    void swap(builder& o) noexcept {
        using std::swap;
        swap(alloc_v, o.alloc_v);
    }

    const ALLOC& get_allocator() const noexcept { return alloc_v; }

    // --- Allocate a node ---
    // pad=true: round up for in-place growth (bitmask nodes)
    // pad=false: exact allocation (compact leaves, VALUE*)
    uint64_t* alloc_node(size_t& u64_count, bool pad = true) {
        size_t actual = pad ? round_up_u64(u64_count) : u64_count;
        uint64_t* p = alloc_v.allocate(actual);
        std::memset(p, 0, actual * U64_BYTES);
        u64_count = actual;
        return p;
    }

    // --- Return a node ---
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept {
        alloc_v.deallocate(p, u64_count);
    }

    // --- drain: no-op, tree destructor frees nodes individually ---
    void drain() noexcept {}

    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;

    slot_type store_value(const VALUE& val) { return val; }
    void destroy_value(slot_type&) noexcept {}
};

// Extended builder: C-type values — routes through base_v freelist bins
template<typename VALUE, typename ALLOC>
struct builder<VALUE, false, ALLOC> {
    using BASE = builder<VALUE, true, ALLOC>;
    using VA = typename std::allocator_traits<ALLOC>::template rebind_alloc<VALUE>;
    using VT = value_traits<VALUE, ALLOC>;
    using slot_type = typename VT::slot_type;  // VALUE*

    static constexpr size_t VAL_U64 = ceil_div(sizeof(VALUE), U64_BYTES);

    BASE base_v;

    builder() = default;
    explicit builder(const ALLOC& a) : base_v(a) {}

    builder(const builder&) = delete;
    builder& operator=(const builder&) = delete;

    builder(builder&& o) noexcept
        : base_v(std::move(o.base_v)) {}

    builder& operator=(builder&& o) noexcept {
        if (this != &o) {
            base_v = std::move(o.base_v);
        }
        return *this;
    }

    ~builder() = default;

    void swap(builder& o) noexcept {
        base_v.swap(o.base_v);
    }

    const ALLOC& get_allocator() const noexcept { return base_v.get_allocator(); }

    uint64_t* alloc_node(size_t& u64_count, bool pad = true) { return base_v.alloc_node(u64_count, pad); }
    void dealloc_node(uint64_t* p, size_t u64_count) noexcept { base_v.dealloc_node(p, u64_count); }

    slot_type store_value(const VALUE& val) {
        if constexpr (VAL_U64 <= FREE_MAX) {
            size_t sz = VAL_U64;
            uint64_t* p = base_v.alloc_node(sz, false);
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
        if constexpr (VAL_U64 <= FREE_MAX) {
            base_v.dealloc_node(reinterpret_cast<uint64_t*>(s), VAL_U64);
        } else {
            VA va(base_v.get_allocator());
            std::allocator_traits<VA>::deallocate(va, s, 1);
        }
    }

    void drain() noexcept {
        base_v.drain();
    }
};

// ==========================================================================
// Normalized ops VALUE: collapse same-sized trivial types to one uint.
// bool stays bool.  Non-trivial / large stays VALUE (keeps HAS_DESTRUCTOR).
// ==========================================================================

template<size_t N>
using sized_uint_for = std::conditional_t<N <= 1, uint8_t,
                       std::conditional_t<N <= 2, uint16_t,
                       std::conditional_t<N <= 4, uint32_t,
                       uint64_t>>>;

template<typename VALUE>
using normalized_ops_value_t =
    std::conditional_t<std::is_same_v<VALUE, bool>, bool,
    std::conditional_t<std::is_trivially_copyable_v<VALUE> && sizeof(VALUE) <= U64_BYTES,
        sized_uint_for<sizeof(VALUE)>,
        VALUE>>;

// ==========================================================================
// Result types
// ==========================================================================

struct insert_result_t {
    uint64_t tagged_ptr;    // tagged pointer (LEAF_BIT for leaf, raw for bitmask)
    bool inserted;
    bool needs_split;
    const void* existing_value;  // non-null on dup: pointer to existing VALUE in leaf
};

struct erase_result_t {
    uint64_t tagged_ptr;    // tagged pointer, or 0 if fully erased
    bool erased;
    uint64_t subtree_entries;  // remaining entries in subtree (exact)
};

} // namespace gteitelbaum

#endif // KNTRIE_SUPPORT_HPP
