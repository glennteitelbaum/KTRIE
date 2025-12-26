/**
 * @file ktrie_dirty_high_pointer.h
 * @brief Pointer with embedded flag bits in high-order bits
 * 
 * This file implements "dirty" pointers that store additional metadata
 * in the unused high bits of 64-bit pointers. On current x86-64 and ARM64
 * architectures, only 48 bits are used for virtual addresses, leaving
 * 16 bits available for other purposes.
 * 
 * We use 5 of these bits to store node type flags, allowing us to know
 * what type of data a pointer references without any additional memory
 * overhead.
 * 
 * Memory Layout (64 bits):
 * ┌─────────────┬──────────────────────────────────────────────────────┐
 * │ 5-bit flags │              59-bit pointer address                  │
 * └─────────────┴──────────────────────────────────────────────────────┘
 *   bits 63-59              bits 58-0
 * 
 * @warning This technique relies on implementation-defined behavior.
 *          It works on current mainstream 64-bit architectures but may
 *          need adjustment for future systems with larger address spaces.
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <cstdint>
#include <utility>

#include "ktrie_defines.h"

namespace gteitelbaum {

/**
 * @class dirty_high_pointer
 * @brief A pointer that stores flag bits in its high-order bits
 * 
 * This class provides a compact way to associate metadata flags with
 * pointers without using additional storage. The flags indicate what
 * type of node array the pointer references.
 * 
 * @tparam T Type being pointed to (typically node<>)
 * @tparam U Type of the flag byte (typically t_flag = uint8_t)
 * 
 * Usage:
 * @code
 * dirty_high_pointer<node_type, t_flag> ptr;
 * ptr.set_ptr(some_node);
 * ptr.set_byte(eos_bit | hop_bit);
 * 
 * auto [node, flags] = ptr.get_both();
 * @endcode
 */
template <class T, class U>
class dirty_high_pointer {
  uint64_t raw_;  ///< Combined pointer and flags
  
  /// Mask for extracting the pointer (low 59 bits)
  static constexpr uint64_t ptr_mask = (1ULL << (64 - num_bits)) - 1;
  
  /// Shift amount to access flags in high bits
  static constexpr int flag_shift = 64 - num_bits;

 public:
  /**
   * @brief Default constructor - null pointer with no flags
   */
  inline dirty_high_pointer();
  
  /**
   * @brief Construct with pointer and flags
   * @param p Pointer value (must fit in 59 bits)
   * @param x Flag bits
   */
  inline dirty_high_pointer(T* p, U x);

  /**
   * @brief Get the flag byte
   * @return Flag bits stored in high 5 bits
   */
  KTRIE_FORCE_INLINE U get_byte() const;

  /**
   * @brief Set the flag byte
   * @param x New flag bits
   */
  KTRIE_FORCE_INLINE void set_byte(U x);

  /**
   * @brief Get the pointer value
   * @return Pointer with high bits masked off
   */
  KTRIE_FORCE_INLINE T* get_ptr() const;

  /**
   * @brief Set the pointer value
   * @param p New pointer (high bits will be masked)
   */
  KTRIE_FORCE_INLINE void set_ptr(T* p);

  /**
   * @brief Get both pointer and flags in one operation
   * @return Pair of (pointer, flags)
   * 
   * This is the most common access pattern - retrieving both values
   * together avoids redundant bit operations.
   */
  KTRIE_FORCE_INLINE std::pair<T*, U> get_both() const;

  /**
   * @brief Get raw 64-bit representation
   * @return Combined pointer and flags as uint64_t
   */
  KTRIE_FORCE_INLINE uint64_t to_u64() const;

  /**
   * @brief Reconstruct from raw 64-bit value
   * @param v Raw value (as returned by to_u64())
   * @return Reconstructed dirty_high_pointer
   */
  KTRIE_FORCE_INLINE static dirty_high_pointer from_u64(uint64_t v);
};

// =============================================================================
// dirty_high_pointer - Inline Definitions
// =============================================================================

template <class T, class U>
dirty_high_pointer<T, U>::dirty_high_pointer() : raw_{0} {}

template <class T, class U>
dirty_high_pointer<T, U>::dirty_high_pointer(T* p, U x)
    : raw_{(static_cast<uint64_t>(x) << flag_shift) | (as_int(p) & ptr_mask)} {
}

template <class T, class U>
U dirty_high_pointer<T, U>::get_byte() const {
  return static_cast<U>(raw_ >> flag_shift);
}

template <class T, class U>
void dirty_high_pointer<T, U>::set_byte(U x) {
  raw_ = (raw_ & ptr_mask) | (static_cast<uint64_t>(x) << flag_shift);
}

template <class T, class U>
T* dirty_high_pointer<T, U>::get_ptr() const {
  return as_ptr<T>(raw_ & ptr_mask);
}

template <class T, class U>
void dirty_high_pointer<T, U>::set_ptr(T* p) {
  raw_ = (raw_ & ~ptr_mask) | (as_int(p) & ptr_mask);
}

template <class T, class U>
std::pair<T*, U> dirty_high_pointer<T, U>::get_both() const {
  return {as_ptr<T>(raw_ & ptr_mask), static_cast<U>(raw_ >> flag_shift)};
}

template <class T, class U>
uint64_t dirty_high_pointer<T, U>::to_u64() const {
  return raw_;
}

template <class T, class U>
dirty_high_pointer<T, U> dirty_high_pointer<T, U>::from_u64(uint64_t v) {
  dirty_high_pointer p;
  p.raw_ = v;
  return p;
}

}  // namespace gteitelbaum
