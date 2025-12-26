/**
 * @file ktrie_data_ptr.h
 * @brief Type-erased value storage for KTRIE nodes
 *
 * This file provides the data_ptr template class which handles storage
 * of user values in trie nodes. Values are stored either inline (for
 * small types) or via heap allocation (for large types).
 *
 * Storage Strategy:
 * - Small types (≤8 bytes): Stored directly in the node's 64-bit data field
 * - Large types (>8 bytes): Heap-allocated, node stores pointer
 *
 * This optimization eliminates heap allocation overhead for common small
 * types like int, double, and pointers, while still supporting arbitrary
 * value types.
 *
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <bit>
#include <memory>
#include <type_traits>

#include "ktrie_defines.h"

namespace gteitelbaum {

//==============================================================================
// Type Classification Concepts
//==============================================================================

/**
 * @concept small_class
 * @brief Types small enough to store inline in a node (≤ pointer size)
 *
 * On 64-bit systems, this includes:
 * - All integer types up to 64 bits
 * - float and double
 * - Pointers
 * - Small structs (≤8 bytes)
 */
template <class T>
concept small_class = sizeof(T) <= sizeof(T*);

/**
 * @concept big_class
 * @brief Types too large to store inline, requiring heap allocation
 *
 * Examples: std::string, std::vector, large structs, long double (on some
 * platforms)
 */
template <class T>
concept big_class = sizeof(T) > sizeof(T*);

//==============================================================================
// Forward Declaration
//==============================================================================

template <class T, class A = std::allocator<T>>
class data_ptr;

//==============================================================================
// Big Class Specialization (Heap Allocated)
//==============================================================================

/**
 * @class data_ptr<big_class T, A>
 * @brief Heap-allocated value storage for large types
 *
 * For types larger than a pointer, values are heap-allocated using the
 * provided allocator. The node stores only a pointer to the allocated value.
 *
 * Memory Management:
 * - Construction: Allocates and copy-constructs the value
 * - Assignment: Deallocates old value, allocates and copies new
 * - Destruction: Must call destroy() explicitly before node deallocation
 *
 * @tparam T Value type (must satisfy big_class concept)
 * @tparam A Allocator type
 */
template <big_class T, class A>
class data_ptr<T, A> {
  T* ptr_;  ///< Pointer to heap-allocated value

  using alloc_traits = std::allocator_traits<A>;

  /**
   * @brief Allocate and copy-construct a value
   * @param p Source value pointer (may be null)
   * @param alloc Allocator to use
   * @return Pointer to newly allocated copy, or nullptr if p is null
   */
  KTRIE_FORCE_INLINE static T* alloc_copy(const T* p, A& alloc);

  /**
   * @brief Deallocate a value
   * @param p Pointer to deallocate (may be null)
   * @param alloc Allocator to use
   */
  KTRIE_FORCE_INLINE static void dealloc(T* p, A& alloc);

 public:
  /**
   * @brief Default constructor - null pointer
   */
  inline data_ptr();

  /**
   * @brief Construct from raw integer (from node's data field)
   * @param i Raw integer value containing pointer
   */
  inline explicit data_ptr(intptr_type i);

  /**
   * @brief Construct by copying a value
   * @param p Pointer to value to copy
   * @param alloc Allocator for allocation
   */
  inline explicit data_ptr(const T* p, A& alloc);

  /**
   * @brief Copy constructor with allocator
   * @param p Source data_ptr
   * @param alloc Allocator for allocation
   */
  inline data_ptr(const data_ptr& p, A& alloc);

  /**
   * @brief Assign by copying a value
   * @param p Pointer to value to copy
   * @param alloc Allocator for allocation
   * @return Reference to this
   */
  inline data_ptr& assign(const T* p, A& alloc);

  /**
   * @brief Assign from another data_ptr
   * @param p Source data_ptr
   * @param alloc Allocator for allocation
   * @return Reference to this
   */
  inline data_ptr& assign(const data_ptr& p, A& alloc);

  /**
   * @brief Move constructor
   * @param p Source data_ptr (will be nulled)
   */
  inline data_ptr(data_ptr&& p) noexcept;

  /**
   * @brief Move assignment
   * @param p Source data_ptr (will be nulled)
   * @return Reference to this
   */
  inline data_ptr& operator=(data_ptr&& p) noexcept;

  /**
   * @brief Destructor - does NOT free memory
   *
   * Caller must call destroy() explicitly before node deallocation.
   */
  ~data_ptr() = default;

  /**
   * @brief Reconstruct data_ptr from raw uint64_t (node's data field)
   * @param val Raw value from node
   * @return Reconstructed data_ptr
   */
  KTRIE_FORCE_INLINE static data_ptr make_data(uint64_t val);

  /**
   * @brief Convert value pointer to uint64_t for node storage
   * @param data Pointer to value
   * @return Raw uint64_t for storage
   */
  KTRIE_FORCE_INLINE static uint64_t make_val(const T* data);

  /**
   * @brief Destroy the stored value (must call before node deallocation)
   * @param val Raw uint64_t from node's data field
   * @param alloc Allocator to use for deallocation
   */
  KTRIE_FORCE_INLINE static void destroy(uint64_t val, A& alloc);

  /**
   * @brief Destroy using default delete (for compatibility)
   * @param val Raw uint64_t from node's data field
   */
  KTRIE_FORCE_INLINE static void destroy(uint64_t val);

  /**
   * @brief Get pointer to stored value
   * @return Pointer to value
   */
  KTRIE_FORCE_INLINE const T* get() const;
};

//==============================================================================
// Small Class Specialization (Inline Storage)
//==============================================================================

/**
 * @class data_ptr<small_class T, A>
 * @brief Inline value storage for small types
 *
 * For types ≤8 bytes, values are stored directly in the node's data field
 * without heap allocation. This eliminates allocation overhead for common
 * types like int, double, and pointers.
 *
 * Bit Preservation:
 * - Integral types: Use static_cast (value semantics)
 * - Floating-point types: Use std::bit_cast (bit-exact preservation)
 *
 * The distinction is important: static_cast<uint64_t>(3.14) gives 3,
 * but bit_cast preserves the IEEE 754 representation.
 *
 * @tparam T Value type (must satisfy small_class concept)
 * @tparam A Allocator type (unused, but kept for API consistency)
 *
 * @note All member functions are defined inline within the class body
 *       for MSVC compatibility with concept-constrained specializations.
 */
template <small_class T, class A>
class data_ptr<T, A> {
  T val_;  ///< Value stored directly (not a pointer)

  /**
   * @brief Safely dereference a potentially-null pointer
   * @param p Pointer to dereference
   * @return Dereferenced value, or default-constructed T if null
   */
  KTRIE_FORCE_INLINE static T safe(const T* p) { return p ? *p : T{}; }

 public:
  /**
   * @brief Default constructor - default value
   */
  data_ptr() : val_{} {}

  /**
   * @brief Construct from raw integer (from node's data field)
   * @param i Raw integer value
   *
   * Uses bit_cast for floating-point types to preserve bit representation,
   * static_cast for integral types.
   */
  explicit data_ptr(intptr_type i) {
    if constexpr (std::is_integral_v<T>) {
      val_ = static_cast<T>(i);
    } else {
      // For floating point types, use bit_cast to preserve bits
      val_ = std::bit_cast<T>(static_cast<uint64_t>(i));
    }
  }

  /**
   * @brief Construct from pointer to value
   * @param p Pointer to value (may be null)
   */
  explicit data_ptr(const T* p) : val_(safe(p)) {}

  /**
   * @brief Construct from pointer with allocator (allocator unused)
   * @param p Pointer to value
   * @param alloc Allocator (unused for small types)
   */
  explicit data_ptr(const T* p, A&) : val_(safe(p)) {}

  /**
   * @brief Copy constructor
   * @param p Source data_ptr
   */
  data_ptr(const data_ptr& p) = default;

  /**
   * @brief Copy constructor with allocator (allocator unused)
   * @param p Source data_ptr
   * @param alloc Allocator (unused)
   */
  data_ptr(const data_ptr& p, A&) : val_(p.val_) {}

  /**
   * @brief Assign from value pointer
   * @param p Pointer to value
   * @param alloc Allocator (unused)
   * @return Reference to this
   */
  data_ptr& assign(const T* p, A&) {
    val_ = safe(p);
    return *this;
  }

  /**
   * @brief Assign from another data_ptr
   * @param p Source data_ptr
   * @param alloc Allocator (unused)
   * @return Reference to this
   */
  data_ptr& assign(const data_ptr& p, A&) {
    val_ = p.val_;
    return *this;
  }

  /**
   * @brief Assign from value pointer
   * @param p Pointer to value
   * @return Reference to this
   */
  data_ptr& operator=(const T* p) {
    val_ = safe(p);
    return *this;
  }

  data_ptr& operator=(const data_ptr&) = default;
  data_ptr(data_ptr&&) noexcept = default;
  data_ptr& operator=(data_ptr&&) noexcept = default;
  ~data_ptr() = default;

  /**
   * @brief Reconstruct data_ptr from raw uint64_t (node's data field)
   * @param v Raw value from node
   * @return Reconstructed data_ptr
   *
   * Uses bit_cast for floating-point types to preserve IEEE 754 representation.
   */
  KTRIE_FORCE_INLINE static data_ptr make_data(uint64_t v) {
    data_ptr d;
    if constexpr (std::is_integral_v<T>) {
      d.val_ = static_cast<T>(v);
    } else {
      // For floating point types, use bit_cast to preserve bits
      d.val_ = std::bit_cast<T>(v);
    }
    return d;
  }

  /**
   * @brief Convert value to uint64_t for node storage
   * @param data Pointer to value
   * @return Raw uint64_t for storage
   *
   * Uses bit_cast for floating-point types to preserve bit representation.
   * For example, 3.14 becomes its IEEE 754 bit pattern, not 3.
   */
  KTRIE_FORCE_INLINE static uint64_t make_val(const T* data) {
    if constexpr (std::is_integral_v<T>) {
      return static_cast<uint64_t>(*data);
    } else {
      // For floating point types, use bit_cast to preserve bits
      return std::bit_cast<uint64_t>(*data);
    }
  }

  /**
   * @brief No-op for inline storage (nothing to deallocate)
   * @param val Raw value (unused)
   * @param alloc Allocator (unused)
   */
  KTRIE_FORCE_INLINE static void destroy(uint64_t, A&) {}

  /**
   * @brief No-op for inline storage
   * @param val Raw value (unused)
   */
  KTRIE_FORCE_INLINE static void destroy(uint64_t) {}

  /**
   * @brief Get pointer to stored value
   * @return Pointer to inline value
   */
  KTRIE_FORCE_INLINE const T* get() const { return &val_; }
};

// =============================================================================
// data_ptr<big_class> - Inline Definitions
// =============================================================================

template <big_class T, class A>
T* data_ptr<T, A>::alloc_copy(const T* p, A& alloc) {
  if (!p) return nullptr;
  T* np = alloc_traits::allocate(alloc, 1);
  alloc_traits::construct(alloc, np, *p);
  return np;
}

template <big_class T, class A>
void data_ptr<T, A>::dealloc(T* p, A& alloc) {
  if (p) {
    alloc_traits::destroy(alloc, p);
    alloc_traits::deallocate(alloc, p, 1);
  }
}

template <big_class T, class A>
data_ptr<T, A>::data_ptr() : ptr_(nullptr) {}

template <big_class T, class A>
data_ptr<T, A>::data_ptr(intptr_type i) : ptr_{as_ptr<T>(i)} {}

template <big_class T, class A>
data_ptr<T, A>::data_ptr(const T* p, A& alloc) : ptr_(alloc_copy(p, alloc)) {}

template <big_class T, class A>
data_ptr<T, A>::data_ptr(const data_ptr& p, A& alloc)
    : ptr_(alloc_copy(p.ptr_, alloc)) {}

template <big_class T, class A>
data_ptr<T, A>& data_ptr<T, A>::assign(const T* p, A& alloc) {
  if (ptr_ != p) {
    dealloc(ptr_, alloc);
    ptr_ = alloc_copy(p, alloc);
  }
  return *this;
}

template <big_class T, class A>
data_ptr<T, A>& data_ptr<T, A>::assign(const data_ptr& p, A& alloc) {
  if (this != &p) {
    dealloc(ptr_, alloc);
    ptr_ = alloc_copy(p.ptr_, alloc);
  }
  return *this;
}

template <big_class T, class A>
data_ptr<T, A>::data_ptr(data_ptr&& p) noexcept : ptr_(p.ptr_) {
  p.ptr_ = nullptr;
}

template <big_class T, class A>
data_ptr<T, A>& data_ptr<T, A>::operator=(data_ptr&& p) noexcept {
  if (this != &p) {
    ptr_ = p.ptr_;
    p.ptr_ = nullptr;
  }
  return *this;
}

template <big_class T, class A>
data_ptr<T, A> data_ptr<T, A>::make_data(uint64_t val) {
  data_ptr d;
  d.ptr_ = as_ptr<T>(val);
  return d;
}

template <big_class T, class A>
uint64_t data_ptr<T, A>::make_val(const T* data) {
  return as_int(data);
}

template <big_class T, class A>
void data_ptr<T, A>::destroy(uint64_t val, A& alloc) {
  dealloc(as_ptr<T>(val), alloc);
}

template <big_class T, class A>
void data_ptr<T, A>::destroy(uint64_t val) {
  delete as_ptr<T>(val);
}

template <big_class T, class A>
const T* data_ptr<T, A>::get() const {
  return ptr_;
}

}  // namespace gteitelbaum