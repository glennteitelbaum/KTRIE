/**
 * @file ktrie_flags_loc.h
 * @brief Abstraction for flag storage location during tree modifications
 * 
 * During insert and remove operations, we need to update flags that indicate
 * what type of content follows in a node array. These flags can be stored
 * in three different locations:
 * 
 * 1. In the parent pointer's high bits (most common)
 * 2. In a HOP node's new_flags field
 * 3. In a SKIP node's new_flags field
 * 
 * This class provides a uniform interface for reading and writing flags
 * regardless of their actual storage location, simplifying the tree
 * modification logic.
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include "ktrie_defines.h"
#include "ktrie_hop.h"
#include "ktrie_skip.h"

namespace gteitelbaum {

// Forward declaration
template <class T, size_t fixed_len, class A>
class node;

/**
 * @class flags_location
 * @brief Tracks where flags are stored and provides uniform access
 * 
 * When processing a node array, flags initially come from the parent
 * pointer. After processing a HOP or SKIP, subsequent flags come from
 * that node's new_flags field. This class tracks the current location
 * and provides read/write access.
 * 
 * @tparam T Value type
 * @tparam fixed_len Fixed key length (0 for strings)
 * @tparam A Allocator type
 */
template <class T, size_t fixed_len, class A>
class flags_location {
 public:
  using node_type = node<T, fixed_len, A>;

  /// Where flags are currently stored
  enum class type { 
    in_ptr,   ///< In the parent pointer's high bits
    in_hop,   ///< In a HOP node's new_flags field
    in_skip   ///< In a SKIP node's new_flags field
  };

 private:
  type loc_;        ///< Current storage location type
  node_type* node_; ///< Pointer to node containing flags

 public:
  /**
   * @brief Default constructor
   */
  inline flags_location();

  /**
   * @brief Construct with location type and node
   * @param t Location type
   * @param n Node containing the flags
   */
  inline flags_location(type t, node_type* n);

  /**
   * @brief Create flags_location pointing to a pointer's flags byte
   * @param n Node containing the pointer
   * @return flags_location for the pointer
   */
  inline static flags_location in_ptr(node_type* n);

  /**
   * @brief Create flags_location pointing to a HOP's new_flags
   * @param n Node containing the HOP
   * @return flags_location for the HOP
   */
  inline static flags_location in_hop(node_type* n);

  /**
   * @brief Create flags_location pointing to a SKIP's new_flags
   * @param n Node containing the SKIP
   * @return flags_location for the SKIP
   */
  inline static flags_location in_skip(node_type* n);

  /**
   * @brief Get the location type
   * @return Current location type
   */
  KTRIE_FORCE_INLINE type location_type() const;

  /**
   * @brief Get the node containing the flags
   * @return Pointer to node
   */
  KTRIE_FORCE_INLINE node_type* get_node() const;

  /**
   * @brief Check if this flags_location is valid
   * @return true if node pointer is not null
   */
  KTRIE_FORCE_INLINE bool valid() const;

  /**
   * @brief Read flags from current location
   * @return Current flag bits
   */
  KTRIE_FORCE_INLINE t_flag get() const;

  /**
   * @brief Write flags to current location
   * @param f New flag bits
   */
  KTRIE_FORCE_INLINE void set(t_flag f);

  /**
   * @brief Add flag bits (OR with existing)
   * @param f Flags to add
   */
  KTRIE_FORCE_INLINE void add_flags(t_flag f);
  
  /**
   * @brief Remove flag bits (AND with complement)
   * @param f Flags to remove
   */
  KTRIE_FORCE_INLINE void remove_flags(t_flag f);

  /**
   * @brief Set child pointer in a reference node
   * @param ref Node containing the pointer to update
   * @param child New child pointer value
   */
  KTRIE_FORCE_INLINE void set_child_ptr(node_type* ref, node_type* child);

  /**
   * @brief Set both flags and child pointer
   * @param ref Node containing pointer
   * @param f New flags
   * @param child New child pointer
   */
  KTRIE_FORCE_INLINE void set_flags_and_ptr(node_type* ref, t_flag f, node_type* child);
};

// =============================================================================
// flags_location - Inline Definitions
// =============================================================================

template <class T, size_t fixed_len, class A>
flags_location<T, fixed_len, A>::flags_location()
    : loc_(type::in_ptr), node_(nullptr) {}

template <class T, size_t fixed_len, class A>
flags_location<T, fixed_len, A>::flags_location(type t, node_type* n)
    : loc_(t), node_(n) {}

template <class T, size_t fixed_len, class A>
flags_location<T, fixed_len, A> flags_location<T, fixed_len, A>::in_ptr(node_type* n) {
  return flags_location(type::in_ptr, n);
}

template <class T, size_t fixed_len, class A>
flags_location<T, fixed_len, A> flags_location<T, fixed_len, A>::in_hop(node_type* n) {
  return flags_location(type::in_hop, n);
}

template <class T, size_t fixed_len, class A>
flags_location<T, fixed_len, A> flags_location<T, fixed_len, A>::in_skip(node_type* n) {
  return flags_location(type::in_skip, n);
}

template <class T, size_t fixed_len, class A>
typename flags_location<T, fixed_len, A>::type 
flags_location<T, fixed_len, A>::location_type() const {
  return loc_;
}

template <class T, size_t fixed_len, class A>
typename flags_location<T, fixed_len, A>::node_type* 
flags_location<T, fixed_len, A>::get_node() const {
  return node_;
}

template <class T, size_t fixed_len, class A>
bool flags_location<T, fixed_len, A>::valid() const {
  return node_ != nullptr;
}

template <class T, size_t fixed_len, class A>
t_flag flags_location<T, fixed_len, A>::get() const {
  if (!node_) return 0;
  switch (loc_) {
    case type::in_ptr:
      return node_->get_ptr().get_byte();
    case type::in_hop:
      return node_->get_hop().get_new_flags();
    case type::in_skip:
      return node_->get_skip().get_new_flags();
  }
  return 0;
}

template <class T, size_t fixed_len, class A>
void flags_location<T, fixed_len, A>::set(t_flag f) {
  if (!node_) return;
  switch (loc_) {
    case type::in_ptr: {
      auto p = node_->get_ptr();
      p.set_byte(f);
      node_->set_ptr(p);
      break;
    }
    case type::in_hop: {
      t_hop hop = node_->get_hop();
      auto arr = to_char_static(hop.to_u64());
      arr[t_hop::new_flags_offset] = static_cast<char>(f);
      node_->set_hop(t_hop::from_u64(from_char_static(arr)));
      break;
    }
    case type::in_skip: {
      t_skip sk = node_->get_skip();
      node_->set_skip(t_skip{sk.get_skip_len(), static_cast<uint8_t>(f)});
      break;
    }
  }
}

template <class T, size_t fixed_len, class A>
void flags_location<T, fixed_len, A>::add_flags(t_flag f) {
  set(get() | f);
}

template <class T, size_t fixed_len, class A>
void flags_location<T, fixed_len, A>::remove_flags(t_flag f) {
  set(get() & ~f);
}

template <class T, size_t fixed_len, class A>
void flags_location<T, fixed_len, A>::set_child_ptr(node_type* ref, node_type* child) {
  auto p = ref->get_ptr();
  p.set_ptr(child);
  ref->set_ptr(p);
}

template <class T, size_t fixed_len, class A>
void flags_location<T, fixed_len, A>::set_flags_and_ptr(node_type* ref, t_flag f, 
                                                         node_type* child) {
  set(f);
  auto p = ref->get_ptr();
  p.set_ptr(child);
  if (loc_ == type::in_ptr) {
    p.set_byte(f);
  }
  ref->set_ptr(p);
}

}  // namespace gteitelbaum
