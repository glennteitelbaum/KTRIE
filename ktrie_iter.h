/**
 * @file ktrie_iter.h
 * @brief Bidirectional iterator implementation for KTRIE
 * 
 * This file provides STL-compatible bidirectional iterators for KTRIE.
 * The iterators traverse keys in sorted order (lexicographic for strings,
 * numeric for integer keys).
 * 
 * Iterator Characteristics:
 * - Category: Bidirectional iterator
 * - Supports: ++, --, *, ->, ==, !=
 * - Invalidation: Any modification to the trie invalidates all iterators
 * 
 * Implementation Notes:
 * - Iterators store the current key as a string of bytes
 * - Dereferencing reconstructs the key-value pair on demand
 * - Increment/decrement use navigation helpers to find next/prev keys
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "ktrie_nav.h"
#include "ktrie_node.h"

namespace gteitelbaum {

// Forward declarations
template <class Key, class T, class A>
class ktrie;

template <class T, size_t fixed_len, class A>
class ktrie_base;

//==============================================================================
// Main Iterator Implementation
//==============================================================================

/**
 * @class ktrie_iterator_impl
 * @brief Bidirectional iterator for KTRIE containers
 * 
 * This iterator provides access to key-value pairs in sorted order.
 * It's a bidirectional iterator supporting forward and backward traversal.
 * 
 * The iterator caches the current key-value pair for efficient repeated
 * dereferencing. The cache is invalidated on increment/decrement.
 * 
 * @tparam Key External key type (std::string or numeric)
 * @tparam T Value type
 * @tparam fixed_len Key length for numeric types (0 for strings)
 * @tparam A Allocator type
 * @tparam is_const true for const_iterator, false for iterator
 */
template <class Key, class T, size_t fixed_len, class A, bool is_const>
class ktrie_iterator_impl {
 public:
  using node_type = node<T, fixed_len, A>;
  using nav_type = nav_helper<T, fixed_len, A>;
  using base_type = ktrie_base<T, fixed_len, A>;

  // Standard iterator type aliases
  using iterator_category = std::bidirectional_iterator_tag;
  using difference_type = std::ptrdiff_t;
  using value_type = std::pair<const Key, T>;
  using pointer = std::conditional_t<is_const, const value_type*, value_type*>;
  using reference = std::conditional_t<is_const, const value_type&, value_type&>;

 private:
  const base_type* trie_;          ///< Pointer to containing trie
  std::string current_key_;        ///< Current key as byte string
  mutable std::unique_ptr<value_type> cache_;  ///< Cached key-value pair
  bool at_end_;                    ///< True if iterator is at end()

  // Allow conversion from non-const to const iterator
  friend class ktrie_iterator_impl<Key, T, fixed_len, A, !is_const>;

  /**
   * @brief Convert internal byte representation to external key type
   * @param bytes Internal key bytes
   * @return Key in external type format
   */
  template <class K>
  static K key_from_bytes(const std::string& bytes);

  /**
   * @brief Populate cache with current key-value pair
   * 
   * Called lazily on dereference. Looks up the current key in the trie
   * and constructs a pair for the user.
   */
  void update_cache() const;

 public:
  /**
   * @brief Default constructor (creates end iterator)
   */
  inline ktrie_iterator_impl();

  /**
   * @brief Construct iterator at specific key
   * @param trie Pointer to containing trie
   * @param key Current key as byte string
   * @param at_end True if this is an end iterator
   */
  inline ktrie_iterator_impl(const base_type* trie, const std::string& key, 
                             bool at_end = false);

  /**
   * @brief Copy constructor
   * @param other Iterator to copy
   */
  inline ktrie_iterator_impl(const ktrie_iterator_impl& other);

  /**
   * @brief Copy assignment
   * @param other Iterator to copy
   * @return Reference to this
   */
  inline ktrie_iterator_impl& operator=(const ktrie_iterator_impl& other);

  ktrie_iterator_impl(ktrie_iterator_impl&& other) noexcept = default;
  ktrie_iterator_impl& operator=(ktrie_iterator_impl&& other) noexcept = default;

  /**
   * @brief Convert non-const iterator to const iterator
   * @tparam other_const Must be false (non-const)
   * @param other Non-const iterator to convert
   */
  template <bool other_const, typename = std::enable_if_t<is_const && !other_const>>
  inline ktrie_iterator_impl(const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other);

  /**
   * @brief Dereference operator
   * @return Reference to current key-value pair
   */
  inline reference operator*() const;

  /**
   * @brief Member access operator
   * @return Pointer to current key-value pair
   */
  inline pointer operator->() const;

  /**
   * @brief Pre-increment (move to next key)
   * @return Reference to this iterator
   */
  inline ktrie_iterator_impl& operator++();

  /**
   * @brief Post-increment
   * @return Copy of iterator before increment
   */
  inline ktrie_iterator_impl operator++(int);

  /**
   * @brief Pre-decrement (move to previous key)
   * @return Reference to this iterator
   */
  inline ktrie_iterator_impl& operator--();

  /**
   * @brief Post-decrement
   * @return Copy of iterator before decrement
   */
  inline ktrie_iterator_impl operator--(int);

  /**
   * @brief Equality comparison
   * @param other Iterator to compare
   * @return true if iterators point to same position
   */
  inline bool operator==(const ktrie_iterator_impl& other) const;

  /**
   * @brief Inequality comparison
   * @param other Iterator to compare
   * @return true if iterators point to different positions
   */
  inline bool operator!=(const ktrie_iterator_impl& other) const;

  /**
   * @brief Cross-constness equality comparison
   * @tparam other_const Constness of other iterator
   * @param other Iterator to compare
   * @return true if iterators point to same position
   */
  template <bool other_const>
  inline bool operator==(const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other) const;

  /**
   * @brief Cross-constness inequality comparison
   * @tparam other_const Constness of other iterator
   * @param other Iterator to compare
   * @return true if iterators point to different positions
   */
  template <bool other_const>
  inline bool operator!=(const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other) const;

  /**
   * @brief Get current key as byte string (for internal use)
   * @return Reference to key bytes
   */
  inline const std::string& key_bytes() const;
  
  /**
   * @brief Check if at end position
   * @return true if at end
   */
  inline bool is_end() const;
};

//==============================================================================
// Reverse Iterator Wrapper
//==============================================================================

/**
 * @class ktrie_reverse_iterator
 * @brief Reverse iterator adapter for KTRIE iterators
 * 
 * Provides reverse iteration by wrapping a forward iterator.
 * Follows the standard library convention where rbegin() wraps end()
 * and dereferencing yields the element before the wrapped iterator.
 */
template <class Iterator>
class ktrie_reverse_iterator {
 public:
  using iterator_type = Iterator;
  using iterator_category = std::bidirectional_iterator_tag;
  using difference_type = typename Iterator::difference_type;
  using value_type = typename Iterator::value_type;
  using pointer = typename Iterator::pointer;
  using reference = typename Iterator::reference;

 private:
  Iterator current_;  ///< Wrapped forward iterator

 public:
  /**
   * @brief Default constructor
   */
  inline ktrie_reverse_iterator();

  /**
   * @brief Construct from forward iterator
   * @param it Iterator to wrap
   */
  inline explicit ktrie_reverse_iterator(Iterator it);

  /**
   * @brief Converting constructor from other reverse iterator
   * @tparam Other Other iterator type
   * @param other Reverse iterator to convert from
   */
  template <class Other>
  inline ktrie_reverse_iterator(const ktrie_reverse_iterator<Other>& other);

  /**
   * @brief Get wrapped iterator
   * @return Copy of wrapped iterator
   */
  inline Iterator base() const;

  /**
   * @brief Dereference (returns element before wrapped position)
   * @return Reference to element
   */
  inline reference operator*() const;

  /**
   * @brief Member access
   * @return Pointer to element
   */
  inline pointer operator->() const;

  /**
   * @brief Increment (moves backward in original sequence)
   * @return Reference to this
   */
  inline ktrie_reverse_iterator& operator++();

  /**
   * @brief Post-increment
   * @return Copy before increment
   */
  inline ktrie_reverse_iterator operator++(int);

  /**
   * @brief Decrement (moves forward in original sequence)
   * @return Reference to this
   */
  inline ktrie_reverse_iterator& operator--();

  /**
   * @brief Post-decrement
   * @return Copy before decrement
   */
  inline ktrie_reverse_iterator operator--(int);

  /**
   * @brief Equality comparison
   * @param other Iterator to compare
   * @return true if equal
   */
  inline bool operator==(const ktrie_reverse_iterator& other) const;

  /**
   * @brief Inequality comparison
   * @param other Iterator to compare
   * @return true if not equal
   */
  inline bool operator!=(const ktrie_reverse_iterator& other) const;
};

// =============================================================================
// ktrie_iterator_impl - Inline Definitions
// =============================================================================

template <class Key, class T, size_t fixed_len, class A, bool is_const>
template <class K>
K ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::key_from_bytes(
    const std::string& bytes) {
  if constexpr (std::is_same_v<K, std::string>) {
    return bytes;
  } else {
    // Numeric key: convert from big-endian bytes
    using U = std::make_unsigned_t<K>;
    U val = 0;
    for (size_t i = 0; i < sizeof(K) && i < bytes.size(); ++i) {
      val = (val << 8) | static_cast<uint8_t>(bytes[i]);
    }
    // Undo signed transformation if needed
    if constexpr (std::is_signed_v<K>) {
      val ^= (U(1) << (sizeof(K) * 8 - 1));
    }
    return static_cast<K>(val);
  }
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
void ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::update_cache() const {
  if (!cache_ && !at_end_ && trie_) {
    auto result = trie_->find_internal(current_key_.data(), current_key_.size());
    if (result) {
      if constexpr (std::is_same_v<Key, std::string>) {
        cache_ = std::make_unique<value_type>(current_key_, *result);
      } else if constexpr (std::is_arithmetic_v<Key>) {
        Key k = key_from_bytes<Key>(current_key_);
        cache_ = std::make_unique<value_type>(k, *result);
      }
    }
  }
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::ktrie_iterator_impl()
    : trie_(nullptr), current_key_(), cache_(), at_end_(true) {}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::ktrie_iterator_impl(
    const base_type* trie, const std::string& key, bool at_end)
    : trie_(trie), current_key_(key), cache_(), at_end_(at_end) {}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::ktrie_iterator_impl(
    const ktrie_iterator_impl& other)
    : trie_(other.trie_), current_key_(other.current_key_),
      cache_(), at_end_(other.at_end_) {}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>&
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator=(
    const ktrie_iterator_impl& other) {
  if (this != &other) {
    trie_ = other.trie_;
    current_key_ = other.current_key_;
    cache_.reset();
    at_end_ = other.at_end_;
  }
  return *this;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
template <bool other_const, typename>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::ktrie_iterator_impl(
    const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other)
    : trie_(other.trie_), current_key_(other.current_key_),
      cache_(), at_end_(other.at_end_) {}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
typename ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::reference
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator*() const {
  update_cache();
  return *cache_;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
typename ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::pointer
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator->() const {
  update_cache();
  return cache_.get();
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>&
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator++() {
  if (!trie_ || at_end_) return *this;
  auto result = trie_->next_item_internal(current_key_.data(), current_key_.size());
  if (result.exists) {
    current_key_ = result.key;
    cache_.reset();
  } else {
    at_end_ = true;
  }
  return *this;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator++(int) {
  ktrie_iterator_impl tmp = *this;
  ++(*this);
  return tmp;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>&
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator--() {
  if (!trie_) return *this;
  if (at_end_) {
    // Move from end() to last element
    auto result = trie_->last_internal();
    if (result.exists) {
      current_key_ = result.key;
      at_end_ = false;
      cache_.reset();
    }
  } else {
    auto result = trie_->prev_item_internal(current_key_.data(), current_key_.size());
    if (result.exists) {
      current_key_ = result.key;
      cache_.reset();
    }
  }
  return *this;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>
ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator--(int) {
  ktrie_iterator_impl tmp = *this;
  --(*this);
  return tmp;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
bool ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator==(
    const ktrie_iterator_impl& other) const {
  if (at_end_ && other.at_end_) return true;
  if (at_end_ != other.at_end_) return false;
  return current_key_ == other.current_key_;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
bool ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator!=(
    const ktrie_iterator_impl& other) const {
  return !(*this == other);
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
template <bool other_const>
bool ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator==(
    const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other) const {
  if (at_end_ && other.at_end_) return true;
  if (at_end_ != other.at_end_) return false;
  return current_key_ == other.current_key_;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
template <bool other_const>
bool ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::operator!=(
    const ktrie_iterator_impl<Key, T, fixed_len, A, other_const>& other) const {
  return !(*this == other);
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
const std::string& ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::key_bytes() const {
  return current_key_;
}

template <class Key, class T, size_t fixed_len, class A, bool is_const>
bool ktrie_iterator_impl<Key, T, fixed_len, A, is_const>::is_end() const {
  return at_end_;
}

// =============================================================================
// ktrie_reverse_iterator - Inline Definitions
// =============================================================================

template <class Iterator>
ktrie_reverse_iterator<Iterator>::ktrie_reverse_iterator() : current_() {}

template <class Iterator>
ktrie_reverse_iterator<Iterator>::ktrie_reverse_iterator(Iterator it)
    : current_(it) {}

template <class Iterator>
template <class Other>
ktrie_reverse_iterator<Iterator>::ktrie_reverse_iterator(
    const ktrie_reverse_iterator<Other>& other)
    : current_(other.base()) {}

template <class Iterator>
Iterator ktrie_reverse_iterator<Iterator>::base() const {
  return current_;
}

template <class Iterator>
typename ktrie_reverse_iterator<Iterator>::reference
ktrie_reverse_iterator<Iterator>::operator*() const {
  Iterator tmp = current_;
  --tmp;
  return *tmp;
}

template <class Iterator>
typename ktrie_reverse_iterator<Iterator>::pointer
ktrie_reverse_iterator<Iterator>::operator->() const {
  Iterator tmp = current_;
  --tmp;
  return tmp.operator->();
}

template <class Iterator>
ktrie_reverse_iterator<Iterator>&
ktrie_reverse_iterator<Iterator>::operator++() {
  --current_;
  return *this;
}

template <class Iterator>
ktrie_reverse_iterator<Iterator>
ktrie_reverse_iterator<Iterator>::operator++(int) {
  ktrie_reverse_iterator tmp = *this;
  --current_;
  return tmp;
}

template <class Iterator>
ktrie_reverse_iterator<Iterator>&
ktrie_reverse_iterator<Iterator>::operator--() {
  ++current_;
  return *this;
}

template <class Iterator>
ktrie_reverse_iterator<Iterator>
ktrie_reverse_iterator<Iterator>::operator--(int) {
  ktrie_reverse_iterator tmp = *this;
  ++current_;
  return tmp;
}

template <class Iterator>
bool ktrie_reverse_iterator<Iterator>::operator==(
    const ktrie_reverse_iterator& other) const {
  return current_ == other.current_;
}

template <class Iterator>
bool ktrie_reverse_iterator<Iterator>::operator!=(
    const ktrie_reverse_iterator& other) const {
  return current_ != other.current_;
}

}  // namespace gteitelbaum
