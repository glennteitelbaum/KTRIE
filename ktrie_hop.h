/**
 * @file ktrie_hop.h
 * @brief HOP node for inline storage of short strings (1-6 characters)
 * 
 * A HOP node stores a short string sequence directly within a single
 * 64-bit node, avoiding the overhead of separate storage for short
 * common prefixes. This is one of the key space optimizations in KTRIE.
 * 
 * ============================================================================
 * MEMORY LAYOUT (64 bits, big-endian byte order)
 * ============================================================================
 * 
 * ┌────┬────┬────┬────┬────┬────┬───────────┬────────┐
 * │ c0 │ c1 │ c2 │ c3 │ c4 │ c5 │ new_flags │ length │
 * └────┴────┴────┴────┴────┴────┴───────────┴────────┘
 *  byte 0  1    2    3    4    5      6          7
 *  (MSB)                                        (LSB)
 * 
 * - Bytes 0-5: Characters of the string (unused bytes are 0)
 * - Byte 6: new_flags - indicates what follows after the HOP
 * - Byte 7: length - number of valid characters (1-6)
 * 
 * ============================================================================
 * BIG-ENDIAN STORAGE AND LEXICOGRAPHIC ORDERING
 * ============================================================================
 * 
 * Characters are stored in big-endian order (most significant byte first).
 * This ensures that numeric comparison of the packed 64-bit values yields
 * correct lexicographic ordering of the strings.
 * 
 * Example: "cat" stored as:
 *   0x63'61'74'00'00'00'XX'03
 *       c   a   t  (unused) flags len
 * 
 * WHY BIG-ENDIAN?
 * - On little-endian machines (x86, ARM), we must byteswap
 * - The benefit: memcmp-equivalent comparison using integer comparison
 * - Iteration through children yields sorted order naturally
 * 
 * ============================================================================
 * BIT MANIPULATION IN MATCHES() AND FIND_MISMATCH()
 * ============================================================================
 * 
 * The matches() function compares HOP characters against input using
 * efficient 64-bit operations instead of byte-by-byte comparison.
 * 
 * MASKING TECHNIQUE:
 * 
 * To compare only the character bytes (not flags/length), we use:
 *   mask = ~0xFFFFULL = 0xFFFFFFFFFFFF0000
 * 
 * This clears the bottom 16 bits (flags + length bytes), allowing
 * direct comparison of character data.
 * 
 * Example comparison:
 *   HOP data:  0x6361740000000A03  ("cat", some flags, len=3)
 *   Input:     0x6361740000000000  ("cat" packed)
 *   Mask:      0xFFFFFFFFFFFF0000
 *   
 *   (HOP & mask) == (input & mask) → match!
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <cstdint>
#include <string>
#include <algorithm>

#include "ktrie_defines.h"

namespace gteitelbaum {

/**
 * @class t_hop
 * @brief Inline storage for 1-6 character string sequences
 * 
 * HOP (short for "hop over") allows the trie to skip over short common
 * prefixes without creating individual branch nodes for each character.
 * This dramatically reduces memory usage for tries with many keys sharing
 * common prefixes.
 * 
 * Performance: All operations are O(n) where n ≤ 6, effectively O(1).
 */
class t_hop {
  uint64_t data_;  ///< Packed character data, flags, and length

 public:
  /// Maximum characters that fit in a HOP node
  static constexpr size_t max_hop = 6;
  
  /// Byte offset of the length field (position 7 in big-endian)
  static constexpr size_t sz_offset = 7;
  
  /// Byte offset of the new_flags field (position 6 in big-endian)
  static constexpr size_t new_flags_offset = max_hop;

  /**
   * @brief Default constructor - empty HOP
   */
  inline t_hop();

  /**
   * @brief Construct HOP from character sequence
   * @param c Pointer to character data
   * @param len Number of characters (must be 1-6)
   * @param flags Flags indicating what follows this HOP
   * 
   * ============================================================================
   * CONSTRUCTION BIT MANIPULATION
   * ============================================================================
   * 
   * 1. Pack characters into big-endian format using from_char_static()
   * 2. Clear the length byte and set new length
   * 3. Clear the flags byte and set new flags
   * 
   * The bottom byte (length) is manipulated using:
   *   data_ = (data_ & ~0xFFULL) | len
   *   
   *   ~0xFFULL = 0xFFFFFFFFFFFFFF00  (clears bottom byte)
   *   | len inserts new length value
   * 
   * The flags byte is manipulated using:
   *   data_ = (data_ & ~0xFF00ULL) | (flags << 8)
   *   
   *   ~0xFF00ULL = 0xFFFFFFFFFFFF00FF  (clears byte at position 6)
   *   (flags << 8) shifts flags into correct position
   */
  inline t_hop(const char* c, size_t len, uint8_t flags);

  /**
   * @brief Get character at specified position
   * @param pos Position (0-based, must be < get_hop_sz())
   * @return Character at position
   * 
   * Uses to_char_static() to extract the byte array, then indexes.
   * In big-endian order, position 0 is the most significant byte.
   */
  KTRIE_FORCE_INLINE char get_char_at(size_t pos) const;

  /**
   * @brief Get flags indicating what follows this HOP
   * @return Flag bits (may contain EOS, LIST, POP, or another HOP/SKIP)
   */
  KTRIE_FORCE_INLINE uint8_t get_new_flags() const;

  /**
   * @brief Get number of characters stored
   * @return Length (1-6)
   */
  KTRIE_FORCE_INLINE uint8_t get_hop_sz() const;

  /**
   * @brief Create a suffix HOP starting at given position
   * @param start Starting character position
   * @return New HOP containing characters from start to end
   * 
   * ============================================================================
   * SUFFIX EXTRACTION
   * ============================================================================
   * 
   * When splitting a HOP (e.g., during insert), we need to extract a suffix.
   * 
   * Example: Original HOP contains "hello", start=2
   * Result: New HOP contains "llo"
   * 
   * Algorithm:
   * 1. Extract character array
   * 2. Call from_char_static starting at offset 'start'
   * 3. This automatically shifts characters to the beginning
   * 4. Set new length = original_length - start
   */
  inline t_hop get_suffix(size_t start) const;

  /**
   * @brief Convert to string representation
   * @return String containing the stored characters
   */
  inline std::string to_string() const;

  /**
   * @brief Check if this HOP matches a key prefix
   * @param c Key data to compare against
   * @param remaining Number of characters remaining in key
   * @return true if HOP matches the key prefix
   * 
   * ============================================================================
   * FAST MATCHING USING 64-BIT COMPARISON
   * ============================================================================
   * 
   * Instead of comparing characters one-by-one, we:
   * 
   * 1. Check length: if hop_sz > remaining, immediate fail
   * 
   * 2. Pack input characters into 64-bit big-endian format
   *    input_packed = from_char_static(c, hop_sz)
   * 
   * 3. Mask out flags/length bytes (bottom 16 bits)
   *    mask = ~0xFFFFULL = 0xFFFFFFFFFFFF0000
   * 
   * 4. Compare:
   *    (data_ & mask) == (input_packed & mask)
   * 
   * This is faster than a loop because:
   * - Single 64-bit comparison vs multiple byte comparisons
   * - No branch mispredictions from loop control
   * - Modern CPUs execute this in 1-2 cycles
   * 
   * EDGE CASE: The mask also zeros out unused character positions,
   * ensuring we only compare valid characters.
   */
  KTRIE_FORCE_INLINE bool matches(const char* c, size_t remaining) const;

  /**
   * @brief Find first mismatch position with a key
   * @param c Key data to compare
   * @param remaining Characters remaining in key
   * @return Position of first mismatch (or min(hop_sz, remaining) if all match)
   * 
   * ============================================================================
   * MISMATCH DETECTION
   * ============================================================================
   * 
   * Used during insert to determine where to split the HOP.
   * 
   * Algorithm:
   * 1. Determine check_len = min(hop_sz, remaining)
   * 2. Extract HOP characters to array
   * 3. Compare byte-by-byte until mismatch
   * 4. Return position of first difference
   * 
   * POTENTIAL OPTIMIZATION: Could use XOR + countl_zero for SWAR detection,
   * but for max 6 bytes, the simple loop is often faster due to:
   * - No setup overhead
   * - Early exit on first mismatch (common case)
   * - Better branch prediction for short strings
   */
  KTRIE_FORCE_INLINE size_t find_mismatch(const char* c, size_t remaining) const;

  /**
   * @brief Get raw 64-bit representation
   * @return The raw packed data
   */
  KTRIE_FORCE_INLINE uint64_t to_u64() const;

  /**
   * @brief Reconstruct from raw 64-bit value
   * @param v Raw 64-bit value
   * @return Reconstructed t_hop
   */
  KTRIE_FORCE_INLINE static t_hop from_u64(uint64_t v);
};

// =============================================================================
// t_hop - Inline Definitions
// =============================================================================

t_hop::t_hop() : data_{0} {}

t_hop::t_hop(const char* c, size_t len, uint8_t flags) {
  KTRIE_DEBUG_ASSERT(len > 0 && len <= max_hop);
  
  //----------------------------------------------------------------------------
  // Pack characters into big-endian 64-bit value
  //----------------------------------------------------------------------------
  // from_char_static copies 'len' bytes and pads with zeros,
  // then converts to big-endian so first character is in MSB.
  //
  data_ = from_char_static(c, len);
  
  //----------------------------------------------------------------------------
  // Set length in lowest byte (position 7)
  //----------------------------------------------------------------------------
  // Clear bottom byte: ~0xFFULL = 0xFFFFFFFFFFFFFF00
  // OR in the length value
  //
  data_ = (data_ & ~0xFFULL) | len;
  
  //----------------------------------------------------------------------------
  // Set flags in second-lowest byte (position 6)
  //----------------------------------------------------------------------------
  // Clear byte 6: ~0xFF00ULL = 0xFFFFFFFFFFFF00FF
  // Shift flags left by 8 bits and OR in
  //
  data_ = (data_ & ~0xFF00ULL) | (static_cast<uint64_t>(flags) << 8);
}

char t_hop::get_char_at(size_t pos) const {
  // to_char_static converts to big-endian byte array
  // Position 0 = most significant byte = first character
  return to_char_static(data_)[pos];
}

uint8_t t_hop::get_new_flags() const {
  // Flags are at byte position 6 (offset new_flags_offset)
  return static_cast<uint8_t>(get_char_at(new_flags_offset));
}

uint8_t t_hop::get_hop_sz() const {
  // Length is at byte position 7 (offset sz_offset)
  return static_cast<uint8_t>(get_char_at(sz_offset));
}

t_hop t_hop::get_suffix(size_t start) const {
  uint8_t my_sz = get_hop_sz();
  KTRIE_DEBUG_ASSERT(start < my_sz);
  
  // Extract byte array representation
  auto arr = to_char_static(data_);
  
  // Create new HOP from suffix
  t_hop h;
  h.data_ = from_char_static(arr.data() + start, my_sz - start);
  
  // Set new length
  h.data_ = (h.data_ & ~0xFFULL) | (my_sz - start);
  
  return h;
}

std::string t_hop::to_string() const {
  auto arr = to_char_static(data_);
  return std::string(arr.data(), get_hop_sz());
}

bool t_hop::matches(const char* c, size_t remaining) const {
  uint8_t my_sz = get_hop_sz();
  
  // Quick length check
  if (my_sz > remaining) return false;
  
  //----------------------------------------------------------------------------
  // 64-bit masked comparison
  //----------------------------------------------------------------------------
  // Mask clears flags and length bytes, keeping only character data
  // ~0xFFFFULL = 0xFFFFFFFFFFFF0000 in little-endian representation
  //
  // After masking:
  // - Bytes 0-5: character data (or zeros for unused positions)
  // - Bytes 6-7: forced to zero (don't affect comparison)
  //
  return (data_ & ~0xFFFFULL) == (from_char_static(c, my_sz) & ~0xFFFFULL);
}

size_t t_hop::find_mismatch(const char* c, size_t remaining) const {
  uint8_t my_sz = get_hop_sz();
  size_t check_len = std::min(static_cast<size_t>(my_sz), remaining);
  
  // Extract our characters for comparison
  auto my_data = to_char_static(data_);
  
  // Simple loop - for 6 bytes max, this is often faster than SWAR
  // due to early exit on common-case early mismatches
  for (size_t i = 0; i < check_len; ++i)
    if (my_data[i] != c[i]) return i;
    
  return check_len;
}

uint64_t t_hop::to_u64() const {
  return data_;
}

t_hop t_hop::from_u64(uint64_t v) {
  t_hop h;
  h.data_ = v;
  return h;
}

}  // namespace gteitelbaum
