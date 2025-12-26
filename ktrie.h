/**
 * @file ktrie.h
 * @brief Main public header for KTRIE - High-Performance Trie Container
 * 
 * KTRIE is a header-only C++20 implementation of an ordered associative
 * container using a compact trie data structure. It provides a std::map-like
 * interface with optimized memory layout and cache-friendly traversal.
 * 
 * Features:
 * - Multiple key types: std::string, char*, and all numeric types
 * - Ordered traversal: Keys are always iterated in sorted order
 * - Memory efficient: Compact node representation (8 bytes per node)
 * - Cache-friendly: Contiguous node arrays minimize cache misses
 * - STL-compatible: Familiar interface with iterators
 * 
 * Basic Usage:
 * @code
 * #include "ktrie.h"
 * using namespace gteitelbaum;
 * 
 * // String keys
 * ktrie<std::string, int> phonebook;
 * phonebook.insert({"Alice", 1234});
 * phonebook["Bob"] = 5678;
 * 
 * // Numeric keys
 * ktrie<int, std::string> numbers;
 * numbers.insert({42, "forty-two"});
 * 
 * // Iteration (always sorted)
 * for (const auto& [key, value] : phonebook) {
 *     std::cout << key << ": " << value << "\n";
 * }
 * @endcode
 * 
 * @author Glenn Teitelbaum
 * @see README.md for comprehensive documentation
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "ktrie_base.h"
#include "ktrie_iter.h"
#include "ktrie_num_cvt.h"

namespace gteitelbaum {

//==============================================================================
// Static Assertions
//==============================================================================

static_assert(sizeof(node<int, 0>) == 8, "Node must be 8 bytes");
static_assert((std::endian::native == std::endian::big) ||
                  (std::endian::native == std::endian::little),
              "Irregular Endian not supported");

//==============================================================================
// Primary Template (not implemented)
//==============================================================================

/**
 * @class ktrie
 * @brief Primary template - not implemented directly
 * 
 * Use one of the specializations:
 * - ktrie<std::string, T> for string keys
 * - ktrie<char*, T> for raw pointer keys
 * - ktrie<NumericType, T> for numeric keys
 * 
 * @tparam Key Key type
 * @tparam T Value type
 * @tparam A Allocator type
 */
template <class Key, class T, class A = std::allocator<T>>
class ktrie;

//==============================================================================
// Specialization for std::string keys
//==============================================================================

/**
 * @class ktrie<std::string, T, A>
 * @brief Trie with std::string keys
 * 
 * Provides full STL container interface for string-keyed tries.
 * Keys are stored using path compression (HOP for short sequences,
 * SKIP for longer ones). Common prefixes are shared between keys.
 * 
 * @tparam T Value type
 * @tparam A Allocator type
 */
template <class T, class A>
class ktrie<std::string, T, A> {
 public:
  //============================================================================
  // Type Aliases
  //============================================================================
  
  using key_type = std::string;
  using mapped_type = T;
  using value_type = std::pair<const key_type, T>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using allocator_type = A;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = typename std::allocator_traits<A>::pointer;
  using const_pointer = typename std::allocator_traits<A>::const_pointer;

  using iterator = ktrie_iterator_impl<std::string, T, 0, A, false>;
  using const_iterator = ktrie_iterator_impl<std::string, T, 0, A, true>;
  using reverse_iterator = ktrie_reverse_iterator<iterator>;
  using const_reverse_iterator = ktrie_reverse_iterator<const_iterator>;

 private:
  ktrie_base<T, 0, A> base_;  ///< Implementation

 public:
  //============================================================================
  // Constructors
  //============================================================================

  /**
   * @brief Default constructor
   */
  ktrie() = default;

  /**
   * @brief Construct with allocator
   * @param alloc Allocator to use
   */
  explicit ktrie(const A& alloc) : base_(alloc) {}

  /**
   * @brief Construct from initializer list
   * @param init Initializer list of key-value pairs
   * @param alloc Allocator to use
   */
  ktrie(std::initializer_list<value_type> init, const A& alloc = A())
      : base_(alloc) {
    for (const auto& p : init) insert(p);
  }

  /// Copy constructor (deleted)
  ktrie(const ktrie&) = delete;
  
  /// Copy assignment (deleted)
  ktrie& operator=(const ktrie&) = delete;
  
  /// Move constructor
  ktrie(ktrie&&) = default;
  
  /// Move assignment
  ktrie& operator=(ktrie&&) = default;

  /**
   * @brief Get the allocator
   * @return Copy of the allocator
   */
  allocator_type get_allocator() const { return base_.get_allocator(); }

  //============================================================================
  // Element Access
  //============================================================================

  /**
   * @brief Access element with bounds checking
   * @param key Key to look up
   * @return Reference to the value
   * @throws std::out_of_range if key not found
   */
  T& at(const key_type& key) { 
    return base_.at(key.data(), key.size()); 
  }

  /**
   * @brief Access element with bounds checking (const)
   * @param key Key to look up
   * @return Const reference to the value
   * @throws std::out_of_range if key not found
   */
  const T& at(const key_type& key) const { 
    return base_.at(key.data(), key.size()); 
  }

  /**
   * @brief Access or insert element
   * @param key Key to look up or insert
   * @return Reference to the value (default-constructed if inserted)
   */
  T& operator[](const key_type& key) {
    auto result = base_.insert_internal(key.data(), key.size(), T{});
    return const_cast<T&>(*result.first);
  }

  //============================================================================
  // Iterators
  //============================================================================

  /**
   * @brief Get iterator to first element
   * @return Iterator to first element, or end() if empty
   */
  iterator begin() {
    auto first = base_.first_internal();
    if (!first.exists) return end();
    return iterator(&base_, first.key);
  }

  /**
   * @brief Get const iterator to first element
   * @return Const iterator to first element
   */
  const_iterator begin() const { return cbegin(); }

  /**
   * @brief Get const iterator to first element
   * @return Const iterator to first element
   */
  const_iterator cbegin() const {
    auto first = base_.first_internal();
    if (!first.exists) return cend();
    return const_iterator(&base_, first.key);
  }

  /**
   * @brief Get iterator past last element
   * @return End iterator
   */
  iterator end() { return iterator(&base_, "", true); }

  /**
   * @brief Get const iterator past last element
   * @return Const end iterator
   */
  const_iterator end() const { return cend(); }

  /**
   * @brief Get const iterator past last element
   * @return Const end iterator
   */
  const_iterator cend() const { return const_iterator(&base_, "", true); }

  /**
   * @brief Get reverse iterator to last element
   * @return Reverse iterator
   */
  reverse_iterator rbegin() { return reverse_iterator(end()); }

  /**
   * @brief Get const reverse iterator to last element
   * @return Const reverse iterator
   */
  const_reverse_iterator rbegin() const { return crbegin(); }

  /**
   * @brief Get const reverse iterator to last element
   * @return Const reverse iterator
   */
  const_reverse_iterator crbegin() const { 
    return const_reverse_iterator(cend()); 
  }

  /**
   * @brief Get reverse iterator before first element
   * @return Reverse end iterator
   */
  reverse_iterator rend() { return reverse_iterator(begin()); }

  /**
   * @brief Get const reverse iterator before first element
   * @return Const reverse end iterator
   */
  const_reverse_iterator rend() const { return crend(); }

  /**
   * @brief Get const reverse iterator before first element
   * @return Const reverse end iterator
   */
  const_reverse_iterator crend() const { 
    return const_reverse_iterator(cbegin()); 
  }

  //============================================================================
  // Capacity
  //============================================================================

  /**
   * @brief Check if the trie is empty
   * @return true if empty
   */
  [[nodiscard]] bool empty() const noexcept { return base_.empty(); }

  /**
   * @brief Get number of elements
   * @return Element count
   */
  size_type size() const noexcept { return base_.size(); }

  /**
   * @brief Get maximum possible size
   * @return Maximum number of elements
   */
  size_type max_size() const noexcept { return base_.max_size(); }

  //============================================================================
  // Modifiers
  //============================================================================

  /**
   * @brief Clear all elements
   */
  void clear() noexcept { base_.clear(); }

  /**
   * @brief Insert a key-value pair
   * @param value Pair to insert
   * @return Pair of (iterator, true if inserted)
   */
  std::pair<iterator, bool> insert(const value_type& value) {
    auto [ptr, inserted] = base_.insert_internal(
        value.first.data(), value.first.size(), value.second);
    return {iterator(&base_, value.first), inserted};
  }

  /**
   * @brief Insert a key-value pair (move)
   * @param value Pair to insert (value will be moved)
   * @return Pair of (iterator, true if inserted)
   */
  std::pair<iterator, bool> insert(value_type&& value) {
    auto key = value.first;  // Copy key before moving
    auto [ptr, inserted] = base_.insert_internal(
        key.data(), key.size(), std::move(value.second));
    return {iterator(&base_, key), inserted};
  }

  /**
   * @brief Insert with hint (hint is ignored)
   * @param hint Iterator hint (ignored)
   * @param value Pair to insert
   * @return Iterator to inserted or existing element
   */
  iterator insert(const_iterator hint, const value_type& value) {
    (void)hint;
    return insert(value).first;
  }

  /**
   * @brief Insert range of elements
   * @tparam InputIt Iterator type
   * @param first Start of range
   * @param last End of range
   */
  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) {
      auto& v = *first;
      base_.insert_internal(v.first.data(), v.first.size(), v.second);
    }
  }

  /**
   * @brief Insert from initializer list
   * @param ilist List of pairs to insert
   */
  void insert(std::initializer_list<value_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  /**
   * @brief Emplace a new element
   * @tparam Args Constructor argument types
   * @param args Arguments forwarded to value_type constructor
   * @return Pair of (iterator, true if inserted)
   */
  template <class... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    value_type v(std::forward<Args>(args)...);
    return insert(std::move(v));
  }

  /**
   * @brief Emplace with hint (hint is ignored)
   * @tparam Args Constructor argument types
   * @param hint Iterator hint (ignored)
   * @param args Arguments forwarded to value_type constructor
   * @return Iterator to inserted or existing element
   */
  template <class... Args>
  iterator emplace_hint(const_iterator hint, Args&&... args) {
    (void)hint;
    return emplace(std::forward<Args>(args)...).first;
  }

  /**
   * @brief Try to emplace (only if key doesn't exist)
   * @tparam Args Value constructor argument types
   * @param key Key to insert
   * @param args Arguments forwarded to value constructor
   * @return Pair of (iterator, true if inserted)
   */
  template <class... Args>
  std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args) {
    auto ptr = base_.find_internal(key.data(), key.size());
    if (ptr) return {iterator(&base_, key), false};
    T val(std::forward<Args>(args)...);
    base_.insert_internal(key.data(), key.size(), val);
    return {iterator(&base_, key), true};
  }

  /**
   * @brief Insert or update value
   * @param key Key to insert or update
   * @param value Value to assign
   * @return Pair of (iterator, true if inserted, false if updated)
   */
  std::pair<iterator, bool> insert_or_assign(const key_type& key, const T& value) {
    auto [ptr, inserted] = base_.insert_or_assign_internal(
        key.data(), key.size(), value);
    return {iterator(&base_, key), inserted};
  }

  /**
   * @brief Erase element at iterator position
   * @param pos Iterator to element to erase
   * @return Iterator to next element
   */
  iterator erase(const_iterator pos) {
    if (pos.is_end()) return end();
    std::string key = pos.key_bytes();
    auto next = base_.next_item_internal(key.data(), key.size());
    base_.erase_internal(key.data(), key.size());
    if (!next.exists) return end();
    return iterator(&base_, next.key);
  }

  /**
   * @brief Erase range of elements
   * @param first Start of range
   * @param last End of range
   * @return Iterator past last erased element
   */
  iterator erase(const_iterator first, const_iterator last) {
    while (first != last) first = erase(first);
    return iterator(&base_, first.key_bytes(), first.is_end());
  }

  /**
   * @brief Erase element by key
   * @param key Key to erase
   * @return Number of elements erased (0 or 1)
   */
  size_type erase(const key_type& key) {
    return base_.erase_internal(key.data(), key.size());
  }

  /**
   * @brief Swap contents with another trie
   * @param other Trie to swap with
   */
  void swap(ktrie& other) noexcept { base_.swap(other.base_); }

  //============================================================================
  // Lookup
  //============================================================================

  /**
   * @brief Count elements with key
   * @param key Key to count
   * @return 1 if found, 0 otherwise
   */
  size_type count(const key_type& key) const {
    return base_.count_internal(key.data(), key.size());
  }

  /**
   * @brief Find element by key
   * @param key Key to find
   * @return Iterator to element, or end() if not found
   */
  iterator find(const key_type& key) {
    auto ptr = base_.find_internal(key.data(), key.size());
    if (!ptr) return end();
    return iterator(&base_, key);
  }

  /**
   * @brief Find element by key (const)
   * @param key Key to find
   * @return Const iterator to element, or cend() if not found
   */
  const_iterator find(const key_type& key) const {
    auto ptr = base_.find_internal(key.data(), key.size());
    if (!ptr) return cend();
    return const_iterator(&base_, key);
  }

  /**
   * @brief Check if key exists
   * @param key Key to check
   * @return true if key exists
   */
  bool contains(const key_type& key) const {
    return base_.contains_internal(key.data(), key.size());
  }

  /**
   * @brief Get range of elements matching key
   * @param key Key to find
   * @return Pair of iterators (both point to same element or both are end)
   */
  std::pair<iterator, iterator> equal_range(const key_type& key) {
    auto it = find(key);
    if (it == end()) return {it, it};
    auto next = it; ++next;
    return {it, next};
  }

  /**
   * @brief Get range of elements matching key (const)
   * @param key Key to find
   * @return Pair of const iterators
   */
  std::pair<const_iterator, const_iterator> equal_range(const key_type& key) const {
    auto it = find(key);
    if (it == cend()) return {it, it};
    auto next = it; ++next;
    return {it, next};
  }

  /**
   * @brief Get iterator to first element >= key
   * @param key Key to compare
   * @return Iterator to lower bound
   */
  iterator lower_bound(const key_type& key) {
    auto result = base_.lower_bound_internal(key.data(), key.size());
    if (!result.exists) return end();
    return iterator(&base_, result.key);
  }

  /**
   * @brief Get iterator to first element >= key (const)
   * @param key Key to compare
   * @return Const iterator to lower bound
   */
  const_iterator lower_bound(const key_type& key) const {
    auto result = base_.lower_bound_internal(key.data(), key.size());
    if (!result.exists) return cend();
    return const_iterator(&base_, result.key);
  }

  /**
   * @brief Get iterator to first element > key
   * @param key Key to compare
   * @return Iterator to upper bound
   */
  iterator upper_bound(const key_type& key) {
    auto result = base_.upper_bound_internal(key.data(), key.size());
    if (!result.exists) return end();
    return iterator(&base_, result.key);
  }

  /**
   * @brief Get iterator to first element > key (const)
   * @param key Key to compare
   * @return Const iterator to upper bound
   */
  const_iterator upper_bound(const key_type& key) const {
    auto result = base_.upper_bound_internal(key.data(), key.size());
    if (!result.exists) return cend();
    return const_iterator(&base_, result.key);
  }

  //============================================================================
  // Debug
  //============================================================================

  /**
   * @brief Print trie structure for debugging
   * @param only_summary If true, only print summary statistics
   */
  void pretty_print(bool only_summary = false) const { 
    base_.pretty_print(only_summary); 
  }
};

//==============================================================================
// Specialization for char* keys
//==============================================================================

/**
 * @class ktrie<char*, T, A>
 * @brief Trie with raw pointer keys
 * 
 * Provides a minimal interface for performance-critical applications
 * where string lengths are known. Does not provide iterators.
 * 
 * @tparam T Value type
 * @tparam A Allocator type
 */
template <class T, class A>
class ktrie<char*, T, A> {
 public:
  using key_type = const char*;
  using mapped_type = T;
  using size_type = std::size_t;
  using allocator_type = A;

 private:
  ktrie_base<T, 0, A> base_;

 public:
  ktrie() = default;
  explicit ktrie(const A& alloc) : base_(alloc) {}
  ktrie(const ktrie&) = delete;
  ktrie& operator=(const ktrie&) = delete;
  ktrie(ktrie&&) = default;
  ktrie& operator=(ktrie&&) = default;

  allocator_type get_allocator() const { return base_.get_allocator(); }
  [[nodiscard]] bool empty() const noexcept { return base_.empty(); }
  size_type size() const noexcept { return base_.size(); }
  size_type max_size() const noexcept { return base_.max_size(); }
  void clear() noexcept { base_.clear(); }

  /**
   * @brief Insert a key-value pair
   * @param key Pointer to key data
   * @param len Key length
   * @param value Value to insert
   * @return Pointer to stored value
   */
  const T* insert(const char* key, size_t len, const T& value) {
    return base_.insert_internal(key, len, value).first;
  }

  /**
   * @brief Insert or update a key-value pair
   * @param key Pointer to key data
   * @param len Key length
   * @param value Value to insert or update
   * @return Pointer to stored value
   */
  const T* insert_or_assign(const char* key, size_t len, const T& value) {
    return base_.insert_or_assign_internal(key, len, value).first;
  }

  /**
   * @brief Find a key
   * @param key Pointer to key data
   * @param len Key length
   * @return Pointer to value, or nullptr if not found
   */
  const T* find(const char* key, size_t len) const {
    return base_.find_internal(key, len);
  }

  /**
   * @brief Erase a key
   * @param key Pointer to key data
   * @param len Key length
   * @return Number of elements erased (0 or 1)
   */
  size_type erase(const char* key, size_t len) {
    return base_.erase_internal(key, len);
  }

  /**
   * @brief Check if a key exists
   * @param key Pointer to key data
   * @param len Key length
   * @return true if key exists
   */
  bool contains(const char* key, size_t len) const {
    return base_.contains_internal(key, len);
  }

  void pretty_print(bool only_summary = false) const { 
    base_.pretty_print(only_summary); 
  }
};

//==============================================================================
// Specialization for numeric keys
//==============================================================================

/**
 * @class ktrie<N, T, A>
 * @brief Trie with numeric keys
 * 
 * Provides full STL container interface for numeric-keyed tries.
 * Numeric keys are converted to big-endian byte representation for
 * correct lexicographic ordering (negative numbers sort before positive).
 * 
 * @tparam N Numeric key type (int, long, int64_t, uint64_t, etc.)
 * @tparam T Value type
 * @tparam A Allocator type
 */
template <numeric N, class T, class A>
class ktrie<N, T, A> {
 public:
  using key_type = N;
  using mapped_type = T;
  using value_type = std::pair<const key_type, T>;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using allocator_type = A;
  using reference = value_type&;
  using const_reference = const value_type&;

  using iterator = ktrie_iterator_impl<N, T, sizeof(N), A, false>;
  using const_iterator = ktrie_iterator_impl<N, T, sizeof(N), A, true>;
  using reverse_iterator = ktrie_reverse_iterator<iterator>;
  using const_reverse_iterator = ktrie_reverse_iterator<const_iterator>;

 private:
  ktrie_base<T, sizeof(N), A> base_;
  using cvt = cvt_numeric<N>;

  /**
   * @brief Convert numeric key to byte array
   * @param n Numeric key
   * @return Byte array in sortable order
   */
  static std::array<char, sizeof(N)> to_bytes(N n) { return cvt::bitcvt(n); }

 public:
  ktrie() = default;
  explicit ktrie(const A& alloc) : base_(alloc) {}

  ktrie(std::initializer_list<value_type> init, const A& alloc = A())
      : base_(alloc) {
    for (const auto& p : init) insert(p);
  }

  ktrie(const ktrie&) = delete;
  ktrie& operator=(const ktrie&) = delete;
  ktrie(ktrie&&) = default;
  ktrie& operator=(ktrie&&) = default;

  allocator_type get_allocator() const { return base_.get_allocator(); }

  T& at(key_type key) {
    auto bytes = to_bytes(key);
    return base_.at(bytes.data(), sizeof(N));
  }

  const T& at(key_type key) const {
    auto bytes = to_bytes(key);
    return base_.at(bytes.data(), sizeof(N));
  }

  T& operator[](key_type key) {
    auto bytes = to_bytes(key);
    auto result = base_.insert_internal(bytes.data(), sizeof(N), T{});
    return const_cast<T&>(*result.first);
  }

  iterator begin() {
    auto first = base_.first_internal();
    if (!first.exists) return end();
    return iterator(&base_, first.key);
  }
  const_iterator begin() const { return cbegin(); }
  const_iterator cbegin() const {
    auto first = base_.first_internal();
    if (!first.exists) return cend();
    return const_iterator(&base_, first.key);
  }
  iterator end() { return iterator(&base_, "", true); }
  const_iterator end() const { return cend(); }
  const_iterator cend() const { return const_iterator(&base_, "", true); }
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const { return crbegin(); }
  const_reverse_iterator crbegin() const { 
    return const_reverse_iterator(cend()); 
  }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return crend(); }
  const_reverse_iterator crend() const { 
    return const_reverse_iterator(cbegin()); 
  }

  [[nodiscard]] bool empty() const noexcept { return base_.empty(); }
  size_type size() const noexcept { return base_.size(); }
  size_type max_size() const noexcept { return base_.max_size(); }
  void clear() noexcept { base_.clear(); }

  std::pair<iterator, bool> insert(const value_type& value) {
    auto bytes = to_bytes(value.first);
    auto [ptr, inserted] = base_.insert_internal(
        bytes.data(), sizeof(N), value.second);
    return {iterator(&base_, std::string(bytes.data(), sizeof(N))), inserted};
  }

  std::pair<iterator, bool> insert(key_type key, const T& value) {
    auto bytes = to_bytes(key);
    auto [ptr, inserted] = base_.insert_internal(bytes.data(), sizeof(N), value);
    return {iterator(&base_, std::string(bytes.data(), sizeof(N))), inserted};
  }

  template <class... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    value_type v(std::forward<Args>(args)...);
    return insert(std::move(v));
  }

  std::pair<iterator, bool> insert_or_assign(key_type key, const T& value) {
    auto bytes = to_bytes(key);
    auto [ptr, inserted] = base_.insert_or_assign_internal(
        bytes.data(), sizeof(N), value);
    return {iterator(&base_, std::string(bytes.data(), sizeof(N))), inserted};
  }

  iterator erase(const_iterator pos) {
    if (pos.is_end()) return end();
    std::string key = pos.key_bytes();
    auto next = base_.next_item_internal(key.data(), key.size());
    base_.erase_internal(key.data(), key.size());
    if (!next.exists) return end();
    return iterator(&base_, next.key);
  }

  size_type erase(key_type key) {
    auto bytes = to_bytes(key);
    return base_.erase_internal(bytes.data(), sizeof(N));
  }

  void swap(ktrie& other) noexcept { base_.swap(other.base_); }

  size_type count(key_type key) const {
    auto bytes = to_bytes(key);
    return base_.count_internal(bytes.data(), sizeof(N));
  }

  iterator find(key_type key) {
    auto bytes = to_bytes(key);
    auto ptr = base_.find_internal(bytes.data(), sizeof(N));
    if (!ptr) return end();
    return iterator(&base_, std::string(bytes.data(), sizeof(N)));
  }

  const_iterator find(key_type key) const {
    auto bytes = to_bytes(key);
    auto ptr = base_.find_internal(bytes.data(), sizeof(N));
    if (!ptr) return cend();
    return const_iterator(&base_, std::string(bytes.data(), sizeof(N)));
  }

  bool contains(key_type key) const {
    auto bytes = to_bytes(key);
    return base_.contains_internal(bytes.data(), sizeof(N));
  }

  iterator lower_bound(key_type key) {
    auto bytes = to_bytes(key);
    auto result = base_.lower_bound_internal(bytes.data(), sizeof(N));
    if (!result.exists) return end();
    return iterator(&base_, result.key);
  }

  const_iterator lower_bound(key_type key) const {
    auto bytes = to_bytes(key);
    auto result = base_.lower_bound_internal(bytes.data(), sizeof(N));
    if (!result.exists) return cend();
    return const_iterator(&base_, result.key);
  }

  iterator upper_bound(key_type key) {
    auto bytes = to_bytes(key);
    auto result = base_.upper_bound_internal(bytes.data(), sizeof(N));
    if (!result.exists) return end();
    return iterator(&base_, result.key);
  }

  const_iterator upper_bound(key_type key) const {
    auto bytes = to_bytes(key);
    auto result = base_.upper_bound_internal(bytes.data(), sizeof(N));
    if (!result.exists) return cend();
    return const_iterator(&base_, result.key);
  }

  std::pair<iterator, iterator> equal_range(key_type key) {
    auto it = find(key);
    if (it == end()) return {it, it};
    auto next = it; ++next;
    return {it, next};
  }

  std::pair<const_iterator, const_iterator> equal_range(key_type key) const {
    auto it = find(key);
    if (it == cend()) return {it, it};
    auto next = it; ++next;
    return {it, next};
  }

  void pretty_print(bool only_summary = false) const { 
    base_.pretty_print(only_summary); 
  }
};

//==============================================================================
// Non-member functions
//==============================================================================

/**
 * @brief Swap two tries
 * @tparam Key Key type
 * @tparam T Value type
 * @tparam A Allocator type
 * @param lhs First trie
 * @param rhs Second trie
 */
template <class Key, class T, class A>
void swap(ktrie<Key, T, A>& lhs, ktrie<Key, T, A>& rhs) noexcept {
  lhs.swap(rhs);
}

/**
 * @brief Erase elements matching predicate
 * @tparam Key Key type
 * @tparam T Value type
 * @tparam A Allocator type
 * @tparam Pred Predicate type
 * @param c Trie to modify
 * @param pred Predicate (element is erased if pred returns true)
 * @return Number of elements erased
 */
template <class Key, class T, class A, class Pred>
typename ktrie<Key, T, A>::size_type erase_if(ktrie<Key, T, A>& c, Pred pred) {
  auto old_size = c.size();
  for (auto it = c.begin(); it != c.end();) {
    if (pred(*it)) it = c.erase(it);
    else ++it;
  }
  return old_size - c.size();
}

}  // namespace gteitelbaum
