/**
 * @file ktrie_skip.h
 * @brief SKIP node for storing long strings (>6 characters)
 * 
 * When a string sequence is too long to fit in a HOP node (>6 chars),
 * it's stored as a SKIP node. The SKIP header contains the length and
 * flags, followed by the actual character data in subsequent nodes.
 * 
 * Memory Layout:
 * 
 * SKIP Header Node (64 bits):
 * ┌────────────────────────────────┬──────────────────────────────────┐
 * │     new_flags (5 bits)         │         length (59 bits)         │
 * └────────────────────────────────┴──────────────────────────────────┘
 *   high bits                                 low bits
 * 
 * Following nodes contain packed character data (8 chars per node):
 * ┌────────────────────────────────────────────────────────────────────┐
 * │                    8 characters (64 bits)                          │
 * └────────────────────────────────────────────────────────────────────┘
 * 
 * Example: SKIP for "abcdefghij" (10 chars)
 *   Node 0: [flags | length=10]
 *   Node 1: "abcdefgh" (8 chars)
 *   Node 2: "ij\0\0\0\0\0\0" (2 chars + padding)
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <cstdint>

#include "ktrie_defines.h"

namespace gteitelbaum {

/**
 * @class t_skip
 * @brief Header for long string storage (>6 characters)
 * 
 * SKIP nodes are used for string sequences that don't fit in a HOP.
 * The header contains metadata; the actual characters are stored in
 * the following num_skip_nodes(length) nodes.
 * 
 * This is particularly useful for:
 * - Long common prefixes (e.g., URLs with same domain)
 * - Keys with long unique suffixes
 * - File paths with deep directory structures
 */
class t_skip {
  uint64_t data_;  ///< Packed flags and length

 public:
  /// Bit position where flags start (high 5 bits)
  static constexpr int flag_shift = 64 - num_bits;
  
  /// Mask for extracting length (low 59 bits)
  static constexpr uint64_t len_mask = (1ULL << (64 - num_bits)) - 1;

  /**
   * @brief Default constructor
   */
  inline t_skip();

  /**
   * @brief Construct SKIP header
   * @param len Number of characters (can be very large)
   * @param flags Flags indicating what follows the SKIP data
   */
  inline t_skip(uint64_t len, uint8_t flags);

  /**
   * @brief Get flags indicating what follows the SKIP data
   * @return Flag bits
   */
  KTRIE_FORCE_INLINE uint8_t get_new_flags() const;

  /**
   * @brief Get number of characters stored
   * @return Character count
   */
  KTRIE_FORCE_INLINE uint64_t get_skip_len() const;

  /**
   * @brief Calculate number of nodes needed to store n characters
   * @param n Number of characters
   * @return Number of 64-bit nodes required (ceiling of n/8)
   * 
   * Each node holds 8 characters, so we need ceil(n/8) nodes.
   */
  KTRIE_FORCE_INLINE static size_t num_skip_nodes(size_t n);

  /**
   * @brief Get raw 64-bit representation
   * @return The raw packed data
   */
  KTRIE_FORCE_INLINE uint64_t to_u64() const;

  /**
   * @brief Reconstruct from raw 64-bit value
   * @param v Raw 64-bit value
   * @return Reconstructed t_skip
   */
  KTRIE_FORCE_INLINE static t_skip from_u64(uint64_t v);
};

// =============================================================================
// t_skip - Inline Definitions
// =============================================================================

t_skip::t_skip() : data_{0} {}

t_skip::t_skip(uint64_t len, uint8_t flags)
    : data_{(static_cast<uint64_t>(flags) << flag_shift) | (len & len_mask)} {}

uint8_t t_skip::get_new_flags() const {
  return static_cast<uint8_t>(data_ >> flag_shift);
}

uint64_t t_skip::get_skip_len() const {
  return data_ & len_mask;
}

size_t t_skip::num_skip_nodes(size_t n) {
  return (n + 7) / 8;
}

uint64_t t_skip::to_u64() const {
  return data_;
}

t_skip t_skip::from_u64(uint64_t v) {
  t_skip s;
  s.data_ = v;
  return s;
}

}  // namespace gteitelbaum
