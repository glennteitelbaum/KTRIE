/**
 * @file ktrie_node.h
 * @brief Core node structure for KTRIE
 * 
 * This file defines the fundamental node type used throughout KTRIE.
 * Each node is exactly 8 bytes (64 bits) and can represent different
 * types of data depending on context (determined by flags in parent pointer).
 * 
 * A node can be interpreted as:
 * - A dirty pointer (with embedded flags) to child node array
 * - A HOP node (inline 1-6 character string)
 * - A SKIP header (length + flags for longer strings)
 * - A LIST header (sorted character list for branches)
 * - Part of a POP bitmap (256-bit bitmap for large branches)
 * - Stored value data (inline or pointer, depending on value size)
 * 
 * ============================================================================
 * POP (POPULATION) BITMAP EXPLAINED
 * ============================================================================
 * 
 * When a branch point has 8+ children, we use a 256-bit bitmap (POP)
 * instead of a sorted LIST. This provides O(1) lookup for any character.
 * 
 * STRUCTURE:
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  POP Word 0  │  POP Word 1  │  POP Word 2  │  POP Word 3  │ Ptrs...│
 * │  (64 bits)   │  (64 bits)   │  (64 bits)   │  (64 bits)   │        │
 * │  chars 0-63  │ chars 64-127 │chars 128-191 │chars 192-255 │        │
 * └─────────────────────────────────────────────────────────────────────┘
 * 
 * Each bit in the bitmap corresponds to one possible byte value (0-255).
 * If bit N is set, character N is a valid branch point.
 * 
 * CHILD POINTER ORDERING:
 * After the 4 bitmap words, child pointers follow in sorted order
 * of their corresponding characters. This means:
 * - Iteration yields children in sorted order
 * - To find a child's pointer, count set bits before its position
 * 
 * ============================================================================
 * BIT MANIPULATION FOR POP LOOKUP
 * ============================================================================
 * 
 * To find if character 'c' exists and get its child pointer offset:
 * 
 * 1. WORD SELECTION:
 *    word_idx = c >> 6   (divide by 64, gives 0-3)
 *    bit_pos = c & 63    (mod 64, gives 0-63)
 *    mask = 1ULL << bit_pos
 * 
 * 2. EXISTENCE CHECK:
 *    exists = (pop[word_idx] & mask) != 0
 * 
 * 3. OFFSET CALCULATION (if exists):
 *    The child pointer offset = 4 + (number of set bits before this one)
 *    
 *    For character c in word w:
 *    - Count all bits in words 0 to w-1
 *    - Count bits in word w that are below bit_pos
 *    - Add 4 (skip the bitmap words)
 *    
 *    count_before = popcount(pop[0]) + ... + popcount(pop[w-1])
 *                 + popcount(pop[w] & (mask - 1))
 *    
 *    The (mask - 1) trick: if mask = 0x0100, then mask - 1 = 0x00FF
 *    This gives a bitmask of all bits below the target bit.
 * 
 * EXAMPLE:
 *   pop[0] = 0x0000000000000005  (bits 0 and 2 set: chars 0x00 and 0x02)
 *   pop[1] = 0x0000000000000001  (bit 0 set: char 0x40 = '@')
 *   pop[2] = pop[3] = 0
 *   
 *   Looking up char 0x40 ('@'):
 *   - word_idx = 0x40 >> 6 = 1
 *   - bit_pos = 0x40 & 63 = 0
 *   - mask = 1ULL << 0 = 0x01
 *   - pop[1] & 0x01 = 1 (exists!)
 *   - count_before = popcount(pop[0]) + popcount(pop[1] & 0) = 2 + 0 = 2
 *   - offset = 4 + 2 = 6
 *   - Child pointer is at node[6]
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <bit>
#include <memory>
#include <vector>

#include "ktrie_data_ptr.h"
#include "ktrie_defines.h"
#include "ktrie_dirty_high_pointer.h"
#include "ktrie_hop.h"
#include "ktrie_skip.h"
#include "ktrie_small_list.h"

namespace gteitelbaum {

/// Raw 64-bit value type for node data
using t_val = std::uint64_t;

/**
 * @class node
 * @brief Fundamental 8-byte storage unit for KTRIE
 * 
 * The node class provides a union-like interface to a 64-bit value,
 * with accessor methods for each possible interpretation. The correct
 * interpretation is determined by flag bits in the parent pointer.
 * 
 * @tparam T Value type stored in the trie
 * @tparam fixed_len Non-zero for fixed-length keys (e.g., sizeof(int) for int keys)
 * @tparam A Allocator type
 */
template <class T, size_t fixed_len, class A = std::allocator<T>>
class node {
 public:
  using node_type = node<T, fixed_len, A>;
  using t_ptr = dirty_high_pointer<node_type, t_flag>;
  using t_data = data_ptr<T, A>;

 private:
  uint64_t data_;  ///< Raw 64-bit data (interpreted based on context)

 public:
  inline node();

  //============================================================================
  // Pointer accessors (when node contains a child pointer)
  //============================================================================
  KTRIE_FORCE_INLINE t_ptr get_ptr() const;
  KTRIE_FORCE_INLINE void set_ptr(const t_ptr& c);
  
  //============================================================================
  // Value data accessors (when node contains EOS data)
  //============================================================================
  KTRIE_FORCE_INLINE void set_data(const T* c, A& alloc);
  KTRIE_FORCE_INLINE void update_data(const T* c, A& alloc);
  KTRIE_FORCE_INLINE T get_value() const;
  KTRIE_FORCE_INLINE const T* get_data_ptr() const;
  KTRIE_FORCE_INLINE T* get_data_ptr_mutable();

  //============================================================================
  // SKIP node accessors
  //============================================================================
  KTRIE_FORCE_INLINE t_skip get_skip() const;
  KTRIE_FORCE_INLINE void set_skip(const t_skip& c);

  //============================================================================
  // LIST node accessors
  //============================================================================
  KTRIE_FORCE_INLINE t_small_list get_list() const;
  KTRIE_FORCE_INLINE void set_list(const t_small_list& c);

  //============================================================================
  // HOP node accessors
  //============================================================================
  KTRIE_FORCE_INLINE t_hop get_hop() const;
  KTRIE_FORCE_INLINE void set_hop(const t_hop& c);

  //============================================================================
  // POP (bitmap) node accessors
  //============================================================================
  KTRIE_FORCE_INLINE t_val get_pop() const;
  KTRIE_FORCE_INLINE void set_pop(t_val c);

  //============================================================================
  // Raw access
  //============================================================================
  KTRIE_FORCE_INLINE t_val raw() const;
  KTRIE_FORCE_INLINE void set_raw(t_val v);

  //============================================================================
  // Memory management
  //============================================================================
  static inline node_type* allocate(size_t len, A& alloc);
  static inline void deallocate(node_type* array, size_t len, A& alloc);
  static inline void skip_copy(node_type* ptr, const char* src, size_t sz);
};

// Static assertions to ensure node is exactly 8 bytes
static_assert(sizeof(node<int, 0>) == 8);
static_assert(sizeof(node<std::array<int, 100>, 0>) == 8);
static_assert(sizeof(node<int, 16>) == 8);
static_assert(sizeof(node<std::array<int, 100>, 16>) == 8);

//==============================================================================
// POP (bitmap) helper functions
//==============================================================================

/**
 * @brief Extract list of characters from a POP bitmap
 * @param pop Pointer to 4 consecutive nodes forming 256-bit bitmap
 * @return Vector of characters whose bits are set
 * 
 * ============================================================================
 * POP BITMAP ITERATION USING POPCOUNT
 * ============================================================================
 * 
 * This function extracts all set bits from the 256-bit bitmap, returning
 * the corresponding character values in sorted order.
 * 
 * ALGORITHM:
 * 
 * For each of the 4 words (w = 0, 1, 2, 3):
 *   While the word has set bits:
 *     1. Find the lowest set bit: bit = countr_zero(word)
 *        countr_zero returns number of trailing zeros
 *        This is the bit position (0-63)
 *     
 *     2. Convert to character: c = w * 64 + bit
 *        Word 0 covers chars 0-63
 *        Word 1 covers chars 64-127
 *        etc.
 *     
 *     3. Clear the lowest bit: word &= word - 1
 *        This is Kernighan's algorithm:
 *        word - 1 flips all bits up to and including the lowest set bit
 *        AND with original clears just the lowest set bit
 *        
 *        Example: 0b1010 & 0b1001 = 0b1000
 *     
 *     4. Repeat until word is 0
 * 
 * PERFORMANCE:
 * - O(k) where k is the number of set bits
 * - Each iteration uses efficient bit operations
 * - countr_zero compiles to single instruction (TZCNT/BSF on x86)
 */
KTRIE_FORCE_INLINE std::vector<char> get_pop_chars(const t_val* pop) {
  std::vector<char> result;
  for (int word = 0; word < 4; ++word) {
    t_val bits = pop[word];
    while (bits) {
      // Find position of lowest set bit
      int bit = std::countr_zero(bits);
      
      // Convert to character value
      result.push_back(static_cast<char>(word * 64 + bit));
      
      // Clear lowest set bit (Kernighan's algorithm)
      bits &= bits - 1;
    }
  }
  return result;
}

/**
 * @brief Compare string data in SKIP nodes
 * @param search Pointer to SKIP data nodes
 * @param in Input string to compare
 * @param len Length to compare
 * @return true if strings match
 */
KTRIE_FORCE_INLINE bool do_find_skip(const char* search, const char* in,
                                      size_t len) {
  return memcmp(search, in, len) == 0;
}

/**
 * @brief Find character in POP bitmap and get child offset
 * @param search Pointer to 4 POP nodes (256-bit bitmap)
 * @param c Character to find
 * @param[out] run_add Set to offset to child pointer (4 + popcount of lower bits)
 * @return true if character exists in bitmap
 * 
 * ============================================================================
 * POP LOOKUP WITH OFFSET CALCULATION
 * ============================================================================
 * 
 * This function performs two tasks:
 * 1. Check if character c exists in the bitmap
 * 2. Calculate the offset to its child pointer
 * 
 * ALGORITHM:
 * 
 * Step 1: Locate the bit
 *   word = c >> 6     (which 64-bit word, 0-3)
 *   bit = c & 63      (which bit in that word, 0-63)
 *   mask = 1ULL << bit
 * 
 * Step 2: Check existence
 *   if (!(search[word] & mask)) return false
 * 
 * Step 3: Count bits before this one
 * 
 *   The child pointers follow the bitmap in sorted character order.
 *   To find the correct pointer, we count how many characters come before c.
 *   
 *   For character c in word w:
 *   
 *   offset = 4                         // Skip the 4 bitmap words
 *          + popcount(search[0])       // All children from word 0
 *          + popcount(search[1])       // All children from word 1
 *          + ...
 *          + popcount(search[w-1])     // All children from earlier words
 *          + popcount(search[w] & (mask-1))  // Children before c in word w
 * 
 * THE (mask - 1) TRICK:
 *   If mask = 0b00010000, then mask - 1 = 0b00001111
 *   This creates a bitmask of all positions below the target bit
 *   popcount(word & (mask-1)) counts only the bits below our target
 * 
 * OPTIMIZATION: CUMULATIVE COUNTING
 *   Instead of conditionally summing, we compute cumulative counts:
 *   
 *   w0 = 4 + popcount(search[word] & (mask-1))  // base for word 0
 *   w1 = w0 + popcount(search[0])               // base for word 1
 *   w2 = w1 + popcount(search[1])               // base for word 2
 *   w3 = w2 + popcount(search[2])               // base for word 3
 *   
 *   Then index by word: offset = {w0, w1, w2, w3}[word]
 *   
 *   This avoids branches and enables better pipelining.
 */
KTRIE_FORCE_INLINE bool do_find_pop(const t_val* search, char c, int* run_add) {
  uint8_t v = static_cast<uint8_t>(c);
  int word = v >> 6;      // Which 64-bit word (0-3)
  int bit = v & 63;       // Which bit within word
  t_val mask = 1ULL << bit;
  
  // Check if character exists in bitmap
  if (!(search[word] & mask)) return false;

  //----------------------------------------------------------------------------
  // Count children before this one using cumulative popcount
  //----------------------------------------------------------------------------
  //
  // We compute the offset for each word cumulatively, then select
  // the appropriate one based on which word our character is in.
  //
  // For word 0: offset = 4 + count of bits below 'bit' in word 0
  // For word 1: offset = (word 0 offset) + all bits in word 0
  // For word 2: offset = (word 1 offset) + all bits in word 1
  // For word 3: offset = (word 2 offset) + all bits in word 2
  //
  
  // Base offset for word 0: 4 (skip bitmap) + bits below target in this word
  int w0 = 4 + std::popcount(search[word] & (mask - 1));
  
  // Cumulative offsets for each word
  int w1 = w0 + std::popcount(search[0]);
  int w2 = w1 + std::popcount(search[1]);
  int w3 = w2 + std::popcount(search[2]);

  // Select correct offset based on word index
  std::array<int, 4> before{w0, w1, w2, w3};
  *run_add = before[word];
  return true;
}

// =============================================================================
// node - Inline Definitions
// =============================================================================

template <class T, size_t fixed_len, class A>
node<T, fixed_len, A>::node() : data_{0} {}

template <class T, size_t fixed_len, class A>
typename node<T, fixed_len, A>::t_ptr node<T, fixed_len, A>::get_ptr() const {
  return t_ptr::from_u64(data_);
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_ptr(const t_ptr& c) {
  data_ = c.to_u64();
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_data(const T* c, A& alloc) {
  if constexpr (small_class<T>) {
    (void)alloc;
    data_ = t_data::make_val(c);
  } else {
    using alloc_traits = std::allocator_traits<A>;
    T* np = alloc_traits::allocate(alloc, 1);
    alloc_traits::construct(alloc, np, *c);
    data_ = as_int(np);
  }
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::update_data(const T* c, A& alloc) {
  if constexpr (small_class<T>) {
    (void)alloc;
    data_ = t_data::make_val(c);
  } else {
    t_data::destroy(data_, alloc);
    using alloc_traits = std::allocator_traits<A>;
    T* np = alloc_traits::allocate(alloc, 1);
    alloc_traits::construct(alloc, np, *c);
    data_ = as_int(np);
  }
}

template <class T, size_t fixed_len, class A>
T node<T, fixed_len, A>::get_value() const {
  if constexpr (small_class<T>)
    return static_cast<T>(data_);
  else {
    T* p = as_ptr<T>(data_);
    return p ? *p : T{};
  }
}

template <class T, size_t fixed_len, class A>
const T* node<T, fixed_len, A>::get_data_ptr() const {
  if constexpr (small_class<T>)
    return reinterpret_cast<const T*>(&data_);
  else
    return as_ptr<T>(data_);
}

template <class T, size_t fixed_len, class A>
T* node<T, fixed_len, A>::get_data_ptr_mutable() {
  if constexpr (small_class<T>)
    return reinterpret_cast<T*>(&data_);
  else
    return as_ptr<T>(data_);
}

template <class T, size_t fixed_len, class A>
t_skip node<T, fixed_len, A>::get_skip() const {
  return t_skip::from_u64(data_);
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_skip(const t_skip& c) {
  data_ = c.to_u64();
}

template <class T, size_t fixed_len, class A>
t_small_list node<T, fixed_len, A>::get_list() const {
  return t_small_list::from_u64(data_);
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_list(const t_small_list& c) {
  data_ = c.to_u64();
}

template <class T, size_t fixed_len, class A>
t_hop node<T, fixed_len, A>::get_hop() const {
  return t_hop::from_u64(data_);
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_hop(const t_hop& c) {
  data_ = c.to_u64();
}

template <class T, size_t fixed_len, class A>
t_val node<T, fixed_len, A>::get_pop() const {
  return data_;
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_pop(t_val c) {
  data_ = c;
}

template <class T, size_t fixed_len, class A>
t_val node<T, fixed_len, A>::raw() const {
  return data_;
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::set_raw(t_val v) {
  data_ = v;
}

template <class T, size_t fixed_len, class A>
typename node<T, fixed_len, A>::node_type* 
node<T, fixed_len, A>::allocate(size_t len, A& alloc) {
  using node_alloc_t =
      typename std::allocator_traits<A>::template rebind_alloc<node_type>;
  node_alloc_t node_alloc(alloc);
  node_type* ptr =
      std::allocator_traits<node_alloc_t>::allocate(node_alloc, len);
  for (size_t i = 0; i < len; ++i) {
    std::allocator_traits<node_alloc_t>::construct(node_alloc, ptr + i);
  }
  return ptr;
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::deallocate(node_type* array, size_t len, A& alloc) {
  if (!array) return;
  using node_alloc_t =
      typename std::allocator_traits<A>::template rebind_alloc<node_type>;
  node_alloc_t node_alloc(alloc);
  for (size_t i = 0; i < len; ++i) {
    std::allocator_traits<node_alloc_t>::destroy(node_alloc, array + i);
  }
  std::allocator_traits<node_alloc_t>::deallocate(node_alloc, array, len);
}

template <class T, size_t fixed_len, class A>
void node<T, fixed_len, A>::skip_copy(node_type* ptr, const char* src, size_t sz) {
  // Copy string data into consecutive nodes
  // Each node holds 8 characters
  while (sz >= 8) {
    uint64_t tmp;
    memcpy(&tmp, src, 8);
    ptr->set_raw(tmp);
    ptr++;
    src += 8;
    sz -= 8;
  }
  // Handle remaining bytes (< 8)
  if (sz > 0) {
    uint64_t tmp = 0;
    memcpy(&tmp, src, sz);
    ptr->set_raw(tmp);
  }
}

}  // namespace gteitelbaum
