/**
 * @file ktrie_defines.h
 * @brief Core definitions, macros, and utility functions for KTRIE
 * 
 * This file contains fundamental type definitions, compiler-specific macros,
 * flag bit definitions, and low-level utility functions used throughout the
 * KTRIE implementation.
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <new>  // for std::launder

namespace gteitelbaum {

//==============================================================================
// Compiler-Specific Macros
//==============================================================================

/**
 * @def KTRIE_FORCE_INLINE
 * @brief Forces function inlining across different compilers
 * 
 * Used on performance-critical small functions to eliminate call overhead.
 * Falls back to standard 'inline' if compiler doesn't support forced inlining.
 */
#if defined(_MSC_VER)
#define KTRIE_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define KTRIE_FORCE_INLINE inline __attribute__((always_inline))
#else
#define KTRIE_FORCE_INLINE inline
#endif

/**
 * @def KTRIE_PREFETCH
 * @brief Prefetch memory for read access to reduce cache miss latency
 * 
 * Hints to the processor that the given address will be read soon.
 * This can hide memory latency by fetching data into cache before it's needed.
 * No-op on compilers that don't support prefetch intrinsics.
 */
#if defined(__GNUC__) || defined(__clang__)
#define KTRIE_PREFETCH(addr) __builtin_prefetch(addr, 0, 3)
#elif defined(_MSC_VER)
#include <intrin.h>
#define KTRIE_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#else
#define KTRIE_PREFETCH(addr) ((void)0)
#endif

/**
 * @def KTRIE_ASSUME
 * @brief Provides optimization hints to the compiler
 * 
 * Tells the compiler that a condition is always true, enabling better
 * optimization. In debug builds, this becomes an assertion.
 */
#if __has_cpp_attribute(assume)
#define KTRIE_ASSUME(cond) [[assume((cond))]]
#else
#if defined(_MSC_VER)
#define KTRIE_ASSUME(cond) __assume((cond))
#else
#ifndef __clang__
#define __builtin_assume(cond)            \
  do {                                    \
    if (!(cond)) __builtin_unreachable(); \
  } while (0)
#endif
#define KTRIE_ASSUME(cond) __builtin_assume((cond))
#endif
#endif

/**
 * @def KTRIE_DEBUG_ASSERT
 * @brief Debug-only assertion that becomes an optimization hint in release
 * 
 * In debug builds (NDEBUG not defined), performs a standard assertion.
 * In release builds, provides the condition as an optimization hint.
 */
#ifdef NDEBUG
#define KTRIE_DEBUG_ASSERT(x) KTRIE_ASSUME(x)
#else
#include <cassert>
#define KTRIE_DEBUG_ASSERT(x) assert(x)
#endif

//==============================================================================
// Flag Type and Bit Definitions
//==============================================================================

/**
 * @typedef t_flag
 * @brief Type used for node flags (stored in high bits of pointers)
 */
using t_flag = uint8_t;

/**
 * @enum flags
 * @brief Bit flags indicating node array structure
 * 
 * These flags are stored in the high 5 bits of pointers and indicate
 * what type of data follows in the node array. Multiple flags can be
 * combined (e.g., EOS | HOP means: value data, then HOP node).
 * 
 * Node array processing order when multiple flags are set:
 * 1. EOS (if set): Next node contains the stored value
 * 2. HOP or SKIP (if set): String continuation data
 * 3. LIST or POP (if set): Branch point with children
 */
enum flags : t_flag {
  eos_bit  = 1 << 0,  ///< End-of-string: next node is value data
  skip_bit = 1 << 1,  ///< Long string (>6 chars): length + char data follows
  hop_bit  = 1 << 2,  ///< Short string (1-6 chars): inline in single node
  list_bit = 1 << 3,  ///< Small branch (≤7 children): sorted char list
  pop_bit  = 1 << 4   ///< Large branch (8+ children): 256-bit bitmap
};

/**
 * @enum flag_count
 * @brief Number of bits used for flags in dirty pointers
 * 
 * On 64-bit systems, the high 5 bits of pointers are unused (current
 * architectures only use 48-bit virtual addresses), allowing us to
 * store flags without additional memory overhead.
 */
enum flag_count { num_bits = 5 };

//==============================================================================
// Pointer/Integer Conversion Utilities
//==============================================================================

/**
 * @typedef intptr_type
 * @brief Integer type capable of holding a pointer value
 */
using intptr_type = std::uintptr_t;

/**
 * @brief Convert a pointer to its integer representation
 * @tparam T Pointed-to type
 * @param p Pointer to convert
 * @return Integer representation of the pointer
 */
template <class T>
KTRIE_FORCE_INLINE intptr_type as_int(const T* p) {
  return reinterpret_cast<intptr_type>(p);
}

/**
 * @brief Convert an integer back to a pointer
 * @tparam T Target pointer type
 * @param i Integer value
 * @return Pointer to T
 * 
 * Uses std::launder to prevent optimizer from making incorrect assumptions
 * about pointer provenance when round-tripping through integer representation.
 */
template <class T>
KTRIE_FORCE_INLINE T* as_ptr(intptr_type i) {
  return std::launder(reinterpret_cast<T*>(i));
}

/**
 * @brief Convert an integer to a value type (for small inline values)
 * @tparam T Target value type
 * @param i Integer value
 * @return Value of type T
 */
template <class T>
KTRIE_FORCE_INLINE T as_val(intptr_type i) {
  return static_cast<T>(i);
}

//==============================================================================
// Byte Order Utilities
//==============================================================================

/**
 * @brief Swap byte order if system is little-endian
 * @param x 64-bit value to potentially swap
 * @return Byte-swapped value on little-endian systems, unchanged on big-endian
 * 
 * Used to ensure consistent byte ordering for string/numeric key storage.
 * Keys are stored in big-endian order for correct lexicographic comparison.
 */
KTRIE_FORCE_INLINE uint64_t byteswap_if_le(uint64_t x) {
  if constexpr (std::endian::native == std::endian::little) {
#if __cpp_lib_byteswap >= 202110L
    return std::byteswap(x);
#elif defined(_MSC_VER)
    return _byteswap_uint64(x);
#else
    return __builtin_bswap64(x);
#endif
  }
  return x;
}

/**
 * @brief Convert uint64_t to array of 8 chars (big-endian)
 * @param x Value to convert
 * @return Array of 8 characters representing the value
 * 
 * The result is in big-endian order regardless of system endianness,
 * so character comparisons yield correct lexicographic ordering.
 */
KTRIE_FORCE_INLINE std::array<char, 8> to_char_static(uint64_t x) {
  return std::bit_cast<std::array<char, 8>>(byteswap_if_le(x));
}

/**
 * @brief Convert 8-char array back to uint64_t
 * @param from Character array to convert
 * @return 64-bit integer value
 */
KTRIE_FORCE_INLINE uint64_t from_char_static(std::array<char, 8> from) {
  return byteswap_if_le(std::bit_cast<uint64_t>(from));
}

/**
 * @brief Convert partial char array (1-7 chars) to uint64_t
 * @param from Pointer to character data
 * @param len Number of characters (must be 1-7)
 * @return 64-bit integer with characters in high bytes
 * 
 * Used for HOP nodes which store 1-6 characters inline.
 */
KTRIE_FORCE_INLINE uint64_t from_char_static(const char* from, size_t len) {
  KTRIE_DEBUG_ASSERT(len > 0 && len < 8);
  uint64_t ret = 0;
  memcpy(reinterpret_cast<char*>(&ret), from, len);
  return byteswap_if_le(ret);
}

//==============================================================================
// Memory Allocation Helpers
//==============================================================================

/**
 * @brief Calculate allocation size class for node arrays
 * @param n Requested number of nodes
 * @return Actual allocation size (always >= n)
 * 
 * Node arrays are allocated in size classes to reduce fragmentation:
 * - 1-24 nodes: round up to multiple of 4 (4, 8, 12, 16, 20, 24)
 * - 25+ nodes: round up to multiple of 16 (32, 48, 64, ...)
 * 
 * This provides a good balance between memory efficiency and
 * reducing allocator pressure from frequent small allocations.
 */
KTRIE_FORCE_INLINE size_t alloc_size(size_t n) {
  size_t x = (n <= 24) ? 3 : 15;
  return (n + x) & ~x;
}

//==============================================================================
// Flag Bit Utilities
//==============================================================================

/**
 * @brief Check if any of the specified flag bits are set
 * @param u Flag value to check
 * @param c Bits to test for
 * @return true if any bit in c is set in u
 * 
 * Example: has_bit(flags, eos_bit | hop_bit) returns true if either
 * EOS or HOP bit is set.
 */
KTRIE_FORCE_INLINE bool has_bit(t_flag u, t_flag c) { return (u & c) != 0; }

}  // namespace gteitelbaum
