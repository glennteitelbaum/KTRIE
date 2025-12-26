/**
 * @file ktrie_num_cvt.h
 * @brief Numeric key to byte-array conversion for correct sorting
 * 
 * This file provides utilities to convert numeric keys to byte arrays
 * that sort correctly in lexicographic order. This is essential because
 * KTRIE stores keys as byte sequences and uses byte-by-byte comparison.
 * 
 * Challenges addressed:
 * 
 * 1. Endianness: Native integers may be little-endian, but lexicographic
 *    comparison needs big-endian (most significant byte first).
 * 
 * 2. Signed integers: Two's complement representation doesn't sort correctly
 *    as bytes. For example, -1 (0xFFFFFFFF) would sort after 1 (0x00000001).
 *    We XOR with the sign bit to fix this.
 * 
 * Conversion examples (32-bit signed):
 *   INT32_MIN (-2147483648) → 0x00000000 (sorts first)
 *   -1                      → 0x7FFFFFFF
 *   0                       → 0x80000000
 *   1                       → 0x80000001
 *   INT32_MAX (2147483647)  → 0xFFFFFFFF (sorts last)
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace gteitelbaum {

// Forward declaration
template <class T>
struct cvt_numeric;

//==============================================================================
// Concepts for numeric type classification
//==============================================================================

/// Any arithmetic type (int, float, etc.)
template <typename T>
concept numeric = std::is_arithmetic_v<T>;

/// Signed arithmetic types (need sign-bit adjustment)
template <typename T>
concept signed_numeric = std::is_arithmetic_v<T> && std::is_signed_v<T>;

/// Unsigned arithmetic types (just need endian conversion)
template <typename T>
concept unsigned_numeric = std::is_arithmetic_v<T> && std::is_unsigned_v<T>;

namespace detail {

/**
 * @brief Byte-swap if system is little-endian
 * @tparam T Integer type to swap
 * @param inp Value to potentially swap
 * @return Big-endian representation
 */
template <typename T>
KTRIE_FORCE_INLINE T do_byteswap(T inp) {
  if constexpr (std::endian::native == std::endian::little) {
#if __cpp_lib_byteswap >= 202110L
    return std::byteswap(inp);
#elif defined(_MSC_VER)
    if constexpr (sizeof(T) == 2)
      return static_cast<T>(_byteswap_ushort(static_cast<uint16_t>(inp)));
    else if constexpr (sizeof(T) == 4)
      return static_cast<T>(_byteswap_ulong(static_cast<uint32_t>(inp)));
    else if constexpr (sizeof(T) == 8)
      return static_cast<T>(_byteswap_uint64(static_cast<uint64_t>(inp)));
    else
      return inp;
#else
    if constexpr (sizeof(T) == 2)
      return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(inp)));
    else if constexpr (sizeof(T) == 4)
      return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(inp)));
    else if constexpr (sizeof(T) == 8)
      return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(inp)));
    else
      return inp;
#endif
  }
  return inp;
}

}  // namespace detail

//==============================================================================
// Unsigned type conversion (endian swap only)
//==============================================================================

/**
 * @struct cvt_numeric<unsigned_numeric>
 * @brief Conversion for unsigned integers
 * 
 * Unsigned integers only need byte-order conversion to big-endian
 * for correct lexicographic sorting.
 */
template <unsigned_numeric T>
struct cvt_numeric<T> {
  std::array<char, sizeof(T)> val;

  /**
   * @brief Convert to big-endian byte array
   * @param inp Value to convert
   * @return Byte array in big-endian order
   */
  static auto bitcvt(T inp) {
    inp = detail::do_byteswap(inp);
    return std::bit_cast<std::array<char, sizeof(T)>>(inp);
  }

  /**
   * @brief Convert back from big-endian byte array
   * @return Original value
   */
  auto uncvt() const {
    T ret = std::bit_cast<T>(val);
    ret = detail::do_byteswap(ret);
    return ret;
  }

  /**
   * @brief Construct from value
   * @param x Value to convert
   */
  cvt_numeric(T x) : val{bitcvt(x)} {}

  /**
   * @brief Convert back to original type
   */
  operator T() const { return uncvt(); }
};

//==============================================================================
// Signed type conversion (sign bit flip + endian swap)
//==============================================================================

/**
 * @struct cvt_numeric<signed_numeric>
 * @brief Conversion for signed integers
 * 
 * Signed integers need two transformations:
 * 1. XOR with sign bit to make negative values sort before positive
 * 2. Byte-order conversion to big-endian
 * 
 * The sign bit flip works because:
 * - Negative numbers have sign bit 1, positive have 0
 * - XOR with sign bit inverts this, making positives have high bit 1
 * - This transforms two's complement to offset binary encoding
 */
template <signed_numeric T>
struct cvt_numeric<T> {
  using unsign = std::make_unsigned_t<T>;
  cvt_numeric<unsign> val;

  /**
   * @brief Construct from signed value
   * @param x Value to convert
   */
  cvt_numeric(T x) : val{make_sortable(x)} {}

  /**
   * @brief Transform signed value to sortable unsigned representation
   * @param inp Signed input value
   * @return Unsigned value that sorts correctly
   * 
   * Adds the sign bit value to offset the range:
   * INT_MIN becomes 0, 0 becomes SIGN_BIT, INT_MAX becomes UINT_MAX
   */
  static unsign make_sortable(T inp) {
    unsign uns = std::numeric_limits<T>::max();
    uns++;  // Now equals 2^(bits-1), the sign bit position
    uns += inp;
    return uns;
  }

  /**
   * @brief Reverse the sortable transformation
   * @param inp Unsigned sortable value
   * @return Original signed value
   */
  static T unmake_sortable(unsign inp) {
    unsign sign = std::numeric_limits<T>::max();
    sign++;
    T ret = inp - sign;
    return ret;
  }

  /**
   * @brief Convert back to signed type
   */
  operator T() const { return unmake_sortable(val); }

  /**
   * @brief Convert to big-endian byte array with sign adjustment
   * @param inp Signed value
   * @return Byte array that sorts correctly
   */
  static auto bitcvt(T inp) {
    unsign s = make_sortable(inp);
    s = detail::do_byteswap(s);
    return std::bit_cast<std::array<char, sizeof(T)>>(s);
  }

  /**
   * @brief Convert back from byte array
   * @return Original signed value
   */
  auto uncvt() const {
    unsign ret = std::bit_cast<unsign>(val.val);
    ret = detail::do_byteswap(ret);
    ret = unmake_sortable(ret);
    return static_cast<T>(ret);
  }
};

}  // namespace gteitelbaum
