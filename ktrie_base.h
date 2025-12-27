/**
 * @file ktrie_base.h
 * @brief Base implementation class for KTRIE
 * 
 * This file contains the core ktrie_base template class which implements
 * the fundamental trie operations. The public ktrie class wraps this
 * with a type-safe interface for different key types.
 * 
 * ktrie_base handles:
 * - Memory management for node arrays
 * - Insert, find, and erase operations
 * - Iterator support via navigation helpers
 * - Allocator propagation
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <iomanip>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "ktrie_insert_help.h"
#include "ktrie_nav.h"
#include "ktrie_node.h"
#include "ktrie_pretty.h"
#include "ktrie_remove_help.h"

namespace gteitelbaum {

/**
 * @class ktrie_base
 * @brief Core implementation of the trie data structure
 * 
 * This class provides the fundamental trie operations independent of
 * the key type. The public ktrie class templates specialize on key type
 * and delegate to this class for the actual work.
 * 
 * @tparam T Value type stored in the trie
 * @tparam fixed_len Key length for numeric types (0 for variable-length strings)
 * @tparam A Allocator type
 */
template <class T, size_t fixed_len, class A = std::allocator<T>>
class ktrie_base {
 public:
  using node_type = node<T, fixed_len, A>;
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

 private:
  node_type head_{};                  ///< Head node (contains pointer to root array)
  size_t cnt_ = 0;                    ///< Number of elements
  [[no_unique_address]] A alloc_{};   ///< Allocator instance

  /**
   * @brief Recursively destroy a node array and all its children
   * @param start Start of node array
   * @param flags Initial flags for the array
   * 
   * Traverses the entire subtrie rooted at this array, destroying
   * all stored values and freeing all allocated memory.
   * 
   * ============================================================================
   * ALGORITHM EXPLANATION:
   * ============================================================================
   * 
   * Node arrays have a specific structure determined by their flags:
   * 
   * For fixed_len > 0 (numeric keys):
   *   [HOP or SKIP]? -> [EOS]? -> [LIST or POP]?
   *   - At most ONE HOP or SKIP (not both, not repeated)
   *   - At most ONE EOS (only at the fixed key length boundary)
   * 
   * For fixed_len == 0 (string keys):
   *   ([EOS]? -> [HOP or SKIP])* -> [LIST or POP]?
   *   - EOS can appear multiple times (for keys that are prefixes of other keys)
   *   - HOP/SKIP can chain (though typically optimized to single SKIP)
   * 
   * The function walks through the node array following this structure,
   * destroying values at EOS nodes and recursively processing children.
   */
  void destroy_node_array(node_type* start, t_flag flags) {
    if (!start) return;
    node_type* run = start;

    if constexpr (fixed_len > 0) {
      //------------------------------------------------------------------------
      // Fixed-length keys: simpler structure
      // At most one HOP or SKIP, followed by optional EOS
      //------------------------------------------------------------------------
      if (has_bit(flags, hop_bit)) {
        // HOP node: extract new flags and advance past it
        t_hop hop = run->get_hop();
        flags = hop.get_new_flags();
        run++;
      } else if (has_bit(flags, skip_bit)) {
        // SKIP node: extract length and new flags, advance past header + data
        t_skip sk = run->get_skip();
        size_t slen = sk.get_skip_len();
        flags = sk.get_new_flags();
        run++;  // Skip the header node
        run += t_skip::num_skip_nodes(slen);  // Skip the data nodes
      }
      
      if (has_bit(flags, eos_bit)) {
        // EOS node contains value data - destroy it
        node_type::t_data::destroy(run->raw(), alloc_);
        run++;
      }
    } else {
      //------------------------------------------------------------------------
      // Variable-length keys (strings): more complex structure
      // Multiple EOS/HOP/SKIP nodes can appear in sequence
      //------------------------------------------------------------------------
      while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
        if (has_bit(flags, eos_bit)) {
          // EOS node: destroy the stored value
          node_type::t_data::destroy(run->raw(), alloc_);
          run++;
        }
        if (has_bit(flags, hop_bit | skip_bit)) {
          if (has_bit(flags, hop_bit)) {
            // HOP node: 1-6 inline characters
            t_hop hop = run->get_hop();
            flags = hop.get_new_flags();
            run++;
          } else {
            // SKIP node: header + packed character data
            t_skip sk = run->get_skip();
            size_t slen = sk.get_skip_len();
            flags = sk.get_new_flags();
            run++;
            run += t_skip::num_skip_nodes(slen);
          }
        } else {
          // EOS was set but no HOP/SKIP follows - exit loop
          break;
        }
      }
    }

    //--------------------------------------------------------------------------
    // Process children if this is a branch node
    // Both LIST and POP represent branch points with multiple children
    //--------------------------------------------------------------------------
    if (has_bit(flags, list_bit)) {
      // LIST: small branch with ≤7 children
      // Structure: [LIST header] [child ptr 0] [child ptr 1] ... [child ptr n-1]
      auto list = run->get_list();
      int lsz = list.get_list_sz();
      run++;  // Skip the list header
      for (int i = 0; i < lsz; ++i) {
        // Each child pointer is a dirty pointer with embedded flags
        auto [child_ptr, child_flags] = run->get_ptr().get_both();
        destroy_node_array(child_ptr, child_flags);
        run++;
      }
    } else if (has_bit(flags, pop_bit)) {
      // POP: large branch with 8+ children using 256-bit bitmap
      // Structure: [64-bit word 0] [word 1] [word 2] [word 3] [child ptrs...]
      auto pop = reinterpret_cast<const t_val*>(run);
      // Count total children by summing popcount of all 4 bitmap words
      size_t num_children = std::popcount(pop[0]) + std::popcount(pop[1]) +
                            std::popcount(pop[2]) + std::popcount(pop[3]);
      run += 4;  // Skip the 4 bitmap nodes
      for (size_t i = 0; i < num_children; ++i) {
        auto [child_ptr, child_flags] = run->get_ptr().get_both();
        destroy_node_array(child_ptr, child_flags);
        run++;
      }
    }

    // Calculate total array length and deallocate
    size_t array_len = run - start;
    node_type::deallocate(start, alloc_size(array_len), alloc_);
  }

 public:
  //============================================================================
  // Constructors and Assignment
  //============================================================================

  /**
   * @brief Default constructor
   */
  ktrie_base() = default;

  /**
   * @brief Construct with allocator
   * @param allocator Allocator to use
   */
  explicit ktrie_base(const A& allocator) : alloc_(allocator) {}

  /**
   * @brief Move constructor
   * @param other Trie to move from
   */
  ktrie_base(ktrie_base&& other) noexcept
      : head_{other.head_}, cnt_{other.cnt_}, alloc_{std::move(other.alloc_)} {
    other.head_ = node_type{};
    other.cnt_ = 0;
  }

 /**
   * @brief Copy constructor
   * @param other Trie to copy from
   * 
   * Creates a deep copy by iterating through all elements and inserting them.
   * Complexity: O(n * k) where n is element count and k is average key length.
   */
  ktrie_base(const ktrie_base& other) 
      : alloc_(std::allocator_traits<A>::select_on_container_copy_construction(other.alloc_)) {
    for (auto item = other.first_internal(); item.exists; 
         item = other.next_item_internal(item.key.data(), item.key.size())) {
      insert_internal(item.key.data(), item.key.size(), *item.value);
    }
  }

  /**
   * @brief Copy assignment
   * @param other Trie to copy from
   * @return Reference to this
   */
  ktrie_base& operator=(const ktrie_base& other) {
    if (this != &other) {
      clear();
      if constexpr (std::allocator_traits<A>::propagate_on_container_copy_assignment::value) {
        alloc_ = other.alloc_;
      }
      for (auto item = other.first_internal(); item.exists; 
           item = other.next_item_internal(item.key.data(), item.key.size())) {
        insert_internal(item.key.data(), item.key.size(), *item.value);
      }
    }
    return *this;
  }

  /**
   * @brief Merge elements from another trie
   * @param other Trie to merge from
   * 
   * Attempts to insert each element from 'other' into this trie.
   * Elements with keys already in this trie remain in 'other'.
   * This matches std::map::merge behavior.
   */
  void merge(ktrie_base& other) {
    if (this == &other) return;
    
    // Collect keys that were successfully merged (to erase from 'other')
    std::vector<std::string> merged_keys;
    
    for (auto item = other.first_internal(); item.exists; 
         item = other.next_item_internal(item.key.data(), item.key.size())) {
      auto [ptr, inserted] = insert_internal(item.key.data(), item.key.size(), *item.value);
      if (inserted) {
        merged_keys.push_back(item.key);
      }
    }
    
    // Remove merged keys from source
    for (const auto& key : merged_keys) {
      other.erase_internal(key.data(), key.size());
    }
  }

  /**
   * @brief Merge all elements from an rvalue trie
   * @param other Trie to merge from (will be cleared)
   * 
   * For rvalue sources, we insert all elements (overwriting conflicts)
   * and clear the source. Use insert_or_assign semantics if you want
   * source values to win on conflicts.
   */
  void merge(ktrie_base&& other) {
    if (this == &other) return;
    
    for (auto item = other.first_internal(); item.exists; 
         item = other.next_item_internal(item.key.data(), item.key.size())) {
      // Insert only if not present (standard merge behavior)
      insert_internal(item.key.data(), item.key.size(), *item.value);
    }
    other.clear();
  }



  /**
   * @brief Move assignment
   * @param other Trie to move from
   * @return Reference to this
   */
  ktrie_base& operator=(ktrie_base&& other) noexcept {
    if (this != &other) {
      auto [ptr, flags] = head_.get_ptr().get_both();
      destroy_node_array(ptr, flags);
      head_ = other.head_;
      cnt_ = other.cnt_;
      alloc_ = std::move(other.alloc_);
      other.head_ = node_type{};
      other.cnt_ = 0;
    }
    return *this;
  }

  /**
   * @brief Destructor
   * 
   * Destroys all stored values and frees all allocated memory.
   */
  ~ktrie_base() {
    auto [ptr, flags] = head_.get_ptr().get_both();
    destroy_node_array(ptr, flags);
  }

  //============================================================================
  // Allocator Access
  //============================================================================

  /**
   * @brief Get the allocator
   * @return Copy of the allocator
   */
  allocator_type get_allocator() const { return alloc_; }

  //============================================================================
  // Element Access
  //============================================================================

  /**
   * @brief Access element with bounds checking
   * @param key Key data
   * @param sz Key length
   * @return Reference to the value
   * @throws std::out_of_range if key not found
   */
  T& at(const char* key, size_t sz) {
    auto ptr = find_internal(key, sz);
    if (!ptr) throw std::out_of_range("ktrie::at: key not found");
    return const_cast<T&>(*ptr);
  }

  /**
   * @brief Access element with bounds checking (const version)
   * @param key Key data
   * @param sz Key length
   * @return Const reference to the value
   * @throws std::out_of_range if key not found
   */
  const T& at(const char* key, size_t sz) const {
    auto ptr = find_internal(key, sz);
    if (!ptr) throw std::out_of_range("ktrie::at: key not found");
    return *ptr;
  }

  //============================================================================
  // Capacity
  //============================================================================

  /**
   * @brief Check if the trie is empty
   * @return true if empty
   */
  [[nodiscard]] bool empty() const noexcept { return cnt_ == 0; }

  /**
   * @brief Get number of elements
   * @return Element count
   */
  size_type size() const noexcept { return cnt_; }

  /**
   * @brief Get maximum possible size
   * @return Maximum number of elements
   */
  static size_type max_size() noexcept {
    return (1ULL << (64 - num_bits)) / sizeof(node_type);
  }

  //============================================================================
  // Modifiers
  //============================================================================

  /**
   * @brief Clear all elements
   * 
   * Destroys all stored values and frees all allocated memory.
   */
  void clear() noexcept {
    if (cnt_ == 0) return;
    auto [ptr, flags] = head_.get_ptr().get_both();
    destroy_node_array(ptr, flags);
    head_ = node_type{};
    cnt_ = 0;
  }

  /**
   * @brief Insert a key-value pair
   * @param key Key data
   * @param sz Key length
   * @param value Value to insert
   * @return Pair of (pointer to value, true if inserted)
   * 
   * If the key already exists, the value is not modified .
   */
  std::pair<const T*, bool> insert_internal(const char* key, size_t sz, const T& value) {
    using ih = insert_helper<T, fixed_len, A>;
    typename ih::insert_update_ret tail{key, key + sz, &value, &head_};
    if (cnt_ != 0) {
      auto [ptr, flags] = head_.get_ptr().get_both();
      typename ih::modify_data m{&head_, ptr, ptr, flags};
      ih::insert_update_loop(m, tail, false, alloc_);
      if (tail.cnt == 0) return {tail.ret, false};
      if (tail.tail_ptr == nullptr) {
        cnt_ += tail.cnt;
        return {tail.ret, true};
      }
    }
    ih::make_tail(tail, alloc_);
    cnt_ += tail.cnt;
    return {tail.ret, true};
  }

  /**
   * @brief Insert or update a key-value pair
   * @param key Key data
   * @param sz Key length
   * @param value Value to insert or assign
   * @return Pair of (pointer to value, true if inserted, false if updated)
   */
  std::pair<const T*, bool> insert_or_assign_internal(const char* key, size_t sz, const T& value) {
    using ih = insert_helper<T, fixed_len, A>;
    typename ih::insert_update_ret tail{key, key + sz, &value, &head_};
    if (cnt_ != 0) {
      auto [ptr, flags] = head_.get_ptr().get_both();
      typename ih::modify_data m{&head_, ptr, ptr, flags};
      ih::insert_update_loop(m, tail, true, alloc_);
      if (tail.cnt == 0) return {tail.ret, false};
      if (tail.tail_ptr == nullptr) {
        cnt_ += tail.cnt;
        return {tail.ret, true};
      }
    }
    ih::make_tail(tail, alloc_);
    cnt_ += tail.cnt;
    return {tail.ret, true};
  }

  /**
   * @brief Erase a key
   * @param key Key data
   * @param sz Key length
   * @return Number of elements removed (0 or 1)
   */
  size_type erase_internal(const char* key, size_t sz) {
    if (cnt_ == 0) return 0;
    bool removed = remove_helper<T, fixed_len, A>::remove_loop(key, sz, &cnt_, &head_, alloc_);
    return removed ? 1 : 0;
  }

  /**
   * @brief Swap contents with another trie
   * @param other Trie to swap with
   */
  void swap(ktrie_base& other) noexcept {
    std::swap(head_, other.head_);
    std::swap(cnt_, other.cnt_);
    std::swap(alloc_, other.alloc_);
  }

  //============================================================================
  // Lookup - find_internal
  //============================================================================

  /**
   * @brief Find a key in the trie
   * @param in Key data (pointer to first byte)
   * @param sz Key length in bytes
   * @return Pointer to value if found, nullptr otherwise
   * 
   * ============================================================================
   * ALGORITHM OVERVIEW: find_internal
   * ============================================================================
   * 
   * This function traverses the trie to find a specific key. The trie structure
   * uses path compression (HOP/SKIP nodes) to reduce depth and memory usage.
   * 
   * KEY CONCEPTS:
   * 
   * 1. NODE ARRAYS: Each pointer leads to a "node array" - a contiguous block
   *    of nodes. The first node's interpretation depends on flags embedded in
   *    the parent pointer.
   * 
   * 2. FLAGS: 5 bits stored in the high bits of each pointer indicate what
   *    type(s) of data the node array contains:
   *    - eos_bit:  End-of-string - a value is stored here
   *    - hop_bit:  HOP node - 1-6 inline characters
   *    - skip_bit: SKIP node - longer character sequence
   *    - list_bit: LIST - branch point with ≤7 children
   *    - pop_bit:  POP - branch point with 8+ children (bitmap)
   * 
   * 3. TRAVERSAL: We consume input characters by:
   *    a. Matching HOP/SKIP compressed sequences
   *    b. Looking up single characters in LIST/POP branch points
   *    c. Following child pointers to the next 'node' array
   * 
   * NODE ARRAY STRUCTURE (fixed_len > 0, i.e., numeric keys):
   * ┌─────────────────────────────────────────────────────────────────┐
   * │ [HOP or SKIP]? → [EOS]? → [LIST or POP]?                        │
   * │                                                                 │
   * │ - At most ONE path compression node (HOP or SKIP)               │
   * │ - At most ONE EOS (only at fixed key length boundary)           │
   * │ - At most ONE branch node (LIST or POP)                         │
   * └─────────────────────────────────────────────────────────────────┘
   * 
   * NODE ARRAY STRUCTURE (fixed_len == 0, i.e., string keys):
   * ┌─────────────────────────────────────────────────────────────────┐
   * │ ([EOS]? → [HOP or SKIP])* → [LIST or POP]?                      │
   * │                                                                 │
   * │ - EOS can appear multiple times (prefix keys)                   │
   * │ - HOP/SKIP can chain (though typically consolidated)            │
   * │ - Branch node terminates the array                              │
   * └─────────────────────────────────────────────────────────────────┘
   * 
   * EXAMPLE TRAVERSAL for key "hello":
   * 
   *   head_ → [HOP "hel" | LIST "lo"] → ...
   *                │
   *                ├─'l'→ [HOP "o" | EOS] → value for "hello"
   *                └─'o'→ [EOS] → value for "helo"
   * 
   *   1. Start at head_, get pointer + flags
   *   2. flags has hop_bit set → match "hel" against input, advance input by 3
   *   3. flags (from HOP) has list_bit → find 'l' in list
   *   4. Found 'l' at offset 1 → follow child pointer
   *   5. New flags have hop_bit → match "o", advance input by 1
   *   6. flags (from HOP) has eos_bit → input exhausted? Return value pointer
   * 
   * PERFORMANCE NOTES:
   * 
   * - KTRIE_PREFETCH: Before following a pointer, we prefetch the target
   *   memory. This hides memory latency by initiating the fetch while we
   *   do other work (incrementing input pointer, updating flags).
   * 
   * - The 'if constexpr' (fixed_len > 0) branch is optimized for numeric keys
   *   where the structure is simpler (no repeated EOS/HOP/SKIP).
   * 
   * - LIST lookup uses SWAR (SIMD Within A Register) for fast character
   *   matching. POP lookup uses bit manipulation for O(1) character test.
   */
  const T* find_internal(const char* in, size_t sz) const {
    // Early exit if trie is empty
    if (cnt_ == 0) return nullptr;
    
    // Get root pointer and flags from head node
    auto [run, flags] = head_.get_ptr().get_both();
    
    // Track position in input key
    const char* const last = in + sz;
    
    if constexpr (fixed_len > 0) {
      //========================================================================
      // OPTIMIZED PATH FOR FIXED-LENGTH KEYS (numeric types)
      //========================================================================
      // 
      // Fixed-length keys have a simpler node structure:
      // - At most one HOP or SKIP per node array
      // - EOS only appears when we've consumed exactly fixed_len bytes
      // - No need for the complex EOS/HOP/SKIP interleaving loop
      //
      for (;;) {
        //----------------------------------------------------------------------
        // STEP 1: Process path compression (HOP or SKIP)
        //----------------------------------------------------------------------
        // These nodes compress multiple characters into fewer nodes.
        // We must match all characters exactly or the key doesn't exist.
        //
        if (has_bit(flags, hop_bit | skip_bit)) { // one less branch
          if (has_bit(flags, hop_bit)) [[ likely ]] {
            // HOP: 1-6 characters stored inline in a single 64-bit node
            //
            // HOP Memory Layout:
            // ┌──────────────────────────────────────────┬─────────┬─────┐
            // │ chars[0..5] (up to 6 bytes)              │new_flags│ len │
            // └──────────────────────────────────────────┴─────────┴─────┘
            //
            const auto& hop = run->get_hop();

            // matches() performs SWAR comparison:
            // - Broadcasts comparison across all byte positions simultaneously
            // - Returns false if any character differs or if not enough input
            if (!hop.matches(in, last - in)) return nullptr;

            in += hop.get_hop_sz();       // Advance input past matched chars
            flags = hop.get_new_flags();  // Get flags for next part of array
            run++;                        // Move past HOP node

          } else { // (has_bit(flags, skip_bit)) [[unlikely]] {
            // SKIP: Longer strings (7+ characters) stored as header + data
            // nodes
            //
            // SKIP is unlikely for fixed lengths keys since they are short
            // but it is required for edge cases instead of allowing two HOPs
            // Or allowing for later Numerics of longer lengths
            //
            // SKIP Memory Layout:
            // Node 0: [flags (5 bits) | length (59 bits)]  <- header
            // Node 1+: packed character data (8 chars per node)
            //
            t_skip sk = run->get_skip();
            size_t slen = sk.get_skip_len();
            flags = sk.get_new_flags();

            // Check if enough input remains
            if (static_cast<size_t>(last - in) < slen) return nullptr;

            run++;  // Move past SKIP header

            // do_find_skip performs memcmp on the packed character data
            if (!do_find_skip(reinterpret_cast<const char*>(run), in, slen))
              return nullptr;

            run += t_skip::num_skip_nodes(slen);  // Skip past data nodes
            in += slen;                           // Advance input
          }
        }
        
        //----------------------------------------------------------------------
        // STEP 2: Check for End-of-String (value stored here)
        //----------------------------------------------------------------------
        if (has_bit(flags, eos_bit)) {
          // For fixed-length keys, EOS should only occur when we've
          // consumed exactly the right number of bytes
          if (in == last) return run->get_data_ptr();
          return nullptr;  // Key length mismatch
        }
        
        //----------------------------------------------------------------------
        // STEP 3: Process branch point (LIST or POP)
        //----------------------------------------------------------------------
        // If no branch node, and we haven't found EOS, key doesn't exist
        if (!has_bit(flags, list_bit | pop_bit)) return nullptr;
        
        // If no more input but we need to descend, key not found
        if (in >= last) return nullptr;
        
        if (has_bit(flags, list_bit)) {
          //--------------------------------------------------------------------
          // LIST: Small branch with ≤7 children
          //--------------------------------------------------------------------
          // Structure: [LIST header with sorted chars] [child ptr 0] [ptr 1]...
          //
          // offset() returns 1-based position (0 = not found)
          // Uses SWAR to find character in O(1) time
          //
          int off = run->get_list().offset(*in);
          if (off == 0) return nullptr;  // Character not in list
          run += off;  // Jump to the correct child pointer
          
        } else {
          //--------------------------------------------------------------------
          // POP: Large branch with 8+ children using 256-bit bitmap
          //--------------------------------------------------------------------
          // Structure: [64-bit bitmap × 4] [child ptrs in sorted order]
          //
          // The bitmap has one bit per possible byte value (0-255).
          // Child pointers follow in sorted order of their characters.
          //
          // do_find_pop:
          // 1. Tests if the bit for the character is set in bitmap
          // 2. If set, counts bits below it to find child pointer offset
          // 3. Returns offset = 4 (skip bitmap) + popcount of lower bits
          //
          int off;
          if (!do_find_pop(reinterpret_cast<const t_val*>(run), *in, &off))
            return nullptr;
          run += off;
        }
        
        //----------------------------------------------------------------------
        // STEP 4: Follow child pointer to next node array
        //----------------------------------------------------------------------
        // PREFETCH OPTIMIZATION: We know we're about to dereference the
        // child pointer. By prefetching now, the memory fetch can happen
        // in parallel with the pointer arithmetic and flag extraction.
        //
        KTRIE_PREFETCH(run->get_ptr().get_ptr());
        
        in++;  // Consume the branch character
        
        // Extract both pointer and flags from the dirty pointer
        std::tie(run, flags) = run->get_ptr().get_both();
        
        if (!run) return nullptr;  // Null pointer = key not found
      }
      
    } else {
      //========================================================================
      // PATH FOR VARIABLE-LENGTH KEYS (strings)
      //========================================================================
      //
      // String keys have more complex structure because:
      // 1. A key can be a prefix of another key (e.g., "he" and "hello")
      //    This means EOS can appear before HOP/SKIP in the same array
      // 2. Multiple HOP/SKIP nodes might chain (though rare after optimization)
      //
      // The outer loop processes node arrays, the inner loop processes
      // the EOS/HOP/SKIP sequence within each array.
      //
      for (;;) {
        //----------------------------------------------------------------------
        // INNER LOOP: Process EOS/HOP/SKIP sequence
        //----------------------------------------------------------------------
        // This loop handles the interleaved structure where:
        // - EOS can appear (key is stored here)
        // - HOP/SKIP provides path compression
        // - The pattern can repeat for prefix keys with continuations
        //
        while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
          
          if (has_bit(flags, eos_bit)) {
            // End-of-string: If input is exhausted, we found the key!
            if (in == last) return run->get_data_ptr();
            
            // Input not exhausted - this EOS is for a prefix key
            // Skip the EOS node and continue looking for our key
            run++;
          }
          
          if (has_bit(flags, hop_bit | skip_bit)) {
            if (has_bit(flags, hop_bit)) [[ likely ]] {
              // HOP node processing (same as fixed_len path)
              // More hops than skips
              const auto& hop = run->get_hop();
              if (!hop.matches(in, last - in)) return nullptr;
              in += hop.get_hop_sz();
              flags = hop.get_new_flags();
              run++;
              
            } else {  // has_bit(flags, skip_bit)) [[ unlikely ]]
              // SKIP node processing (same as fixed_len path)
              t_skip sk = run->get_skip();
              size_t slen = sk.get_skip_len();
              flags = sk.get_new_flags();
              if (static_cast<size_t>(last - in) < slen) return nullptr;
              run++;
              if (!do_find_skip(reinterpret_cast<const char*>(run), in, slen))
                return nullptr;
              run += t_skip::num_skip_nodes(slen);
              in += slen;
            }
          } else {
            // EOS was set but no HOP/SKIP follows
            // This means the node array ends with an EOS (leaf node)
            break;
          }
        }
        
        //----------------------------------------------------------------------
        // BRANCH PROCESSING (same as fixed_len path)
        //----------------------------------------------------------------------
        if (!has_bit(flags, list_bit | pop_bit)) return nullptr;
        if (in >= last) return nullptr;
        
        if (has_bit(flags, list_bit)) [[ likely ]] {
          int off = run->get_list().offset(*in);
          if (off == 0) return nullptr;
          run += off;
        } else {  // has_bit(flags, pop_bit)) [[ unlikely ]] 
          int off;
          if (!do_find_pop(reinterpret_cast<const t_val*>(run), *in, &off))
            return nullptr;
          run += off;
        }
        
        KTRIE_PREFETCH(run->get_ptr().get_ptr());
        in++;
        std::tie(run, flags) = run->get_ptr().get_both();
        if (!run) return nullptr;
      }
    }
  }

  /**
   * @brief Count keys matching a value
   * @param key Key data
   * @param sz Key length
   * @return 1 if found, 0 otherwise
   */
  size_type count_internal(const char* key, size_t sz) const {
    return find_internal(key, sz) ? 1 : 0;
  }

  /**
   * @brief Check if a key exists
   * @param key Key data
   * @param sz Key length
   * @return true if key exists
   */
  bool contains_internal(const char* key, size_t sz) const {
    return find_internal(key, sz) != nullptr;
  }

  //============================================================================
  // Navigation (for iterators)
  //============================================================================

  /**
   * @brief Get the first (minimum) key
   * @return Result containing key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  first_internal() const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    return nav_helper<T, fixed_len, A>::find_next_impl_static("", 0, true, run, flags);
  }

  /**
   * @brief Get the last (maximum) key
   * @return Result containing key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  last_internal() const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    std::string max_key(256, '\xFF');
    return nav_helper<T, fixed_len, A>::find_prev_impl_static(
        max_key.data(), max_key.size(), true, run, flags);
  }

  /**
   * @brief Get the next key after a given key
   * @param key Current key data
   * @param sz Current key length
   * @return Result containing next key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  next_item_internal(const char* key, size_t sz) const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    return nav_helper<T, fixed_len, A>::find_next_impl_static(key, sz, false, run, flags);
  }

  /**
   * @brief Get the previous key before a given key
   * @param key Current key data
   * @param sz Current key length
   * @return Result containing previous key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  prev_item_internal(const char* key, size_t sz) const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    return nav_helper<T, fixed_len, A>::find_prev_impl_static(key, sz, false, run, flags);
  }

  /**
   * @brief Get the first key >= given key
   * @param key Key data
   * @param sz Key length
   * @return Result containing key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  lower_bound_internal(const char* key, size_t sz) const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    return nav_helper<T, fixed_len, A>::find_next_impl_static(key, sz, true, run, flags);
  }

  /**
   * @brief Get the first key > given key
   * @param key Key data
   * @param sz Key length
   * @return Result containing key and value pointer
   */
  typename nav_helper<T, fixed_len, A>::ktrie_result
  upper_bound_internal(const char* key, size_t sz) const {
    if (cnt_ == 0) return {};
    auto [run, flags] = head_.get_ptr().get_both();
    if (!run) return {};
    return nav_helper<T, fixed_len, A>::find_next_impl_static(key, sz, false, run, flags);
  }

  //============================================================================
  // Debug
  //============================================================================

  /**
   * @brief Print trie structure for debugging
   * @param only_summary If true, only print summary statistics; 
   *                     if false, print full tree structure
   * 
   * This function provides two modes:
   * 
   * Summary mode (only_summary=true):
   *   Collects and displays statistics about the trie structure:
   *   - Total node arrays and 64-bit words allocated
   *   - Maximum tree depth
   *   - Counts of each node type (HOP, SKIP, LIST, POP)
   *   - Average HOP/SKIP lengths (path compression effectiveness)
   *   - Memory efficiency metrics
   * 
   * Full mode (only_summary=false):
   *   Recursively prints the entire tree structure showing:
   *   - Each node array with its flags
   *   - EOS nodes with their stored values
   *   - HOP/SKIP nodes with their character sequences
   *   - LIST/POP branches with their child characters
   *   - Tree structure via indentation
   * 
   * Example summary output:
   *   ktrie count=1000
   *   Memory: 245 arrays, 1832 nodes (14656 bytes)
   *   Depth: max=12
   *   Nodes: 156 HOPs (avg 3.2 chars), 42 SKIPs (avg 18.7 chars)
   *          89 LISTs, 23 POPs
   */
  void pretty_print(bool only_summary = false) const {
    using pretty = ktrie_pretty<T, fixed_len, A>;
    
    std::cout << "ktrie count=" << cnt_ << std::endl;
    
    // Get root pointer and flags
    auto [ptr, flags] = head_.get_ptr().get_both();
    
    if (!ptr) {
      std::cout << "  (empty)" << std::endl;
      return;
    }

     if (!only_summary) {
      //----------------------------------------------------------------------
      // Full mode: print entire tree structure
      //----------------------------------------------------------------------
      pretty::pretty_print_node(0, ptr, flags, "");
      std::cout << std::endl;
    }

      //----------------------------------------------------------------------
      // Summary mode: collect and display statistics
      //----------------------------------------------------------------------
      typename pretty::trie_stats stats;
      pretty::collect_stats(ptr, flags, 0, stats);
      
      // Memory statistics
      std::cout << "  Memory: " << stats.total_arrays << " arrays, "
                << stats.total_uint64s << " nodes ("
                << stats.total_uint64s * 8 << " bytes)" << std::endl;
      
      // Depth
      std::cout << "Depth:" << std::endl;
      for (int d = 0; d < stats.depth.size(); ++d) {
        std::cout << "  Level " << d << ": " << stats.depth[d] << "( "
                  << std::fixed << std::setprecision(2)
                  << (100.00 * stats.depth[d]) / cnt_ << "%)" << std::endl;
      }
      
      // Node type counts with averages for path compression nodes
      std::cout << "  Nodes: ";
      
      if (stats.hop_count > 0) {
        double avg_hop = static_cast<double>(stats.hop_total_len) / stats.hop_count;
        std::cout << stats.hop_count << " HOPs (avg " 
                  << std::fixed << std::setprecision(1) << avg_hop << " chars)";
      } else {
        std::cout << "0 HOPs";
      }
      
      if (stats.skip_count > 0) {
        double avg_skip = static_cast<double>(stats.skip_total_len) / stats.skip_count;
        std::cout << ", " << stats.skip_count << " SKIPs (avg "
                  << std::fixed << std::setprecision(1) << avg_skip << " chars)";
      } else {
        std::cout << ", 0 SKIPs";
      }
      std::cout << std::endl;
      
      std::cout << "         " << stats.list_count << " LISTs, "
                << stats.pop_count << " POPs";
      if (stats.short_pop_count > 0) {
        std::cout << " (" << stats.short_pop_count << " short)";
      }
      std::cout << std::endl;

      std:: cout << "Bytes per Element: " 
                 << std::fixed << std::setprecision(2)
                 << static_cast<double>(stats.total_uint64s * 8) / cnt_
                << " bytes/element"
                << std::endl;

      std::cout << std::endl;
  }

  /**
   * @brief Get pointer to head node (for internal use)
   * @return Pointer to head node
   */
  const node_type* get_head() const { return &head_; }
};

}  // namespace gteitelbaum
