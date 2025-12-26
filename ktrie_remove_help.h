/**
 * @file ktrie_remove_help.h
 * @brief Remove operation helpers for KTRIE
 *
 * This file contains the core removal logic for KTRIE. It handles:
 * - Removing EOS nodes and their associated values
 * - Cleaning up empty branches
 * - Converting POP back to LIST when children decrease
 * - Rebuilding node arrays after removal
 *
 * ============================================================================
 * REMOVAL ALGORITHM OVERVIEW
 * ============================================================================
 *
 * The remove operation has two phases:
 *
 * PHASE 1: NAVIGATION (in remove_loop)
 *   - Traverse the trie matching key characters
 *   - Build a "path" stack recording each branch point visited
 *   - If key not found, return false
 *   - If found, proceed to Phase 2
 *
 * PHASE 2: CLEANUP (rebuild_without_eos + branch cleanup)
 *   - Remove the EOS node containing the value
 *   - Rebuild the node array without the EOS
 *   - If removal leaves a branch empty, propagate cleanup upward
 *   - May convert POP back to LIST if children fall below threshold
 *
 * ============================================================================
 * PATH STACK FOR BACKTRACKING
 * ============================================================================
 *
 * During traversal, we build a stack of remove_path_entry structures:
 *
 *   struct remove_path_entry {
 *     node_type* ref;           // Parent pointer to this node array
 *     node_type* node_start;    // Start of this node array
 *     t_flag initial_flags;     // Flags at array start
 *     int child_index;          // Which child we descended into
 *     node_type* branch_node;   // The LIST or POP node
 *   };
 *
 * WHY WE NEED THE PATH:
 *
 * After removing an EOS, the branch might become empty. For example:
 *
 *   Before: "cat" ? value1, "car" ? value2
 *   Remove: "cat"
 *   After:  The 't' branch under "ca" is now empty
 *
 * We need to:
 * 1. Remove the 't' entry from the LIST/POP
 * 2. If the branch now has only one child, potentially merge nodes
 * 3. Propagate upward if necessary
 *
 * The path stack lets us "unwind" back up the tree making these fixes.
 *
 * ============================================================================
 * NODE ARRAY RECONSTRUCTION
 * ============================================================================
 *
 * When removing an EOS, we must rebuild the node array without it.
 * The rebuild_without_eos function handles several cases:
 *
 * CASE 1: Simple EOS removal
 *   Before: [EOS, HOP, LIST, ptr1, ptr2]
 *   After:  [HOP, LIST, ptr1, ptr2]
 *   Action: Shift nodes, clear EOS flag
 *
 * CASE 2: EOS was the only content
 *   Before: [EOS]
 *   After:  (empty - set parent pointer to null)
 *   Action: Deallocate array, trigger parent cleanup
 *
 * CASE 3: Trailing HOP/SKIP becomes unnecessary
 *   Before: [HOP "cat", EOS]  (key "cat" removed)
 *   After:  (empty or parent cleanup needed)
 *   Action: May truncate the HOP/SKIP that led nowhere
 *
 * ============================================================================
 * BRANCH CLEANUP FUNCTIONS
 * ============================================================================
 *
 * remove_child_from_branch: Entry point for branch cleanup
 *   - Determines if we have LIST or POP
 *   - Dispatches to appropriate handler
 *
 * remove_from_list: Remove child from LIST branch
 *   - If LIST has 1 child, call remove_last_branch
 *   - Otherwise, rebuild LIST with child removed
 *   - Shift remaining child pointers
 *
 * remove_from_pop: Remove child from POP branch
 *   - If POP has 1 child, call remove_last_branch
 *   - If POP will have ?7 children, convert to LIST
 *   - Otherwise, clear bit and rebuild POP
 *
 * pop_to_list_on_remove: Convert POP back to LIST
 *   - Called when POP children fall to 7 or fewer
 *   - Creates new LIST with remaining characters
 *   - Updates flags from pop_bit to list_bit
 *
 * remove_last_branch: Handle removal of last child
 *   - If no other content, deallocate and propagate up
 *   - If EOS exists before branch, keep the prefix
 *   - Update flags to remove list_bit/pop_bit
 *
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include "ktrie_flags_loc.h"
#include "ktrie_insert_help.h"
#include "ktrie_node.h"

namespace gteitelbaum {

/**
 * @class remove_helper
 * @brief Static helper functions for remove operations
 *
 * @tparam T Value type
 * @tparam fixed_len Key length for numeric types (0 for strings)
 * @tparam A Allocator type
 */
template <class T, size_t fixed_len, class A>
class remove_helper {
 public:
  using node_type = node<T, fixed_len, A>;
  using flags_loc = flags_location<T, fixed_len, A>;

 private:
  /**
   * @struct remove_path_entry
   * @brief Stack entry for tracking path during removal
   *
   * Each entry captures the state at a branch point, allowing us to
   * backtrack and clean up parent nodes after removing a child.
   */
  struct remove_path_entry {
    node_type* ref;          ///< Node containing pointer to this array
    node_type* node_start;   ///< Start of this node array
    t_flag initial_flags;    ///< Initial flags for this array
    int child_index;         ///< Index of child we descended into
    node_type* branch_node;  ///< Branch node (LIST or POP)
  };

  /**
   * @brief Rebuild a node array without an EOS
   *
   * ============================================================================
   * REBUILD_WITHOUT_EOS: Core reconstruction logic
   * ============================================================================
   *
   * This function removes an EOS node from a node array and handles all
   * the edge cases that arise.
   *
   * PARAMETERS:
   *   ref          - Node containing pointer to this array (for updating)
   *   node_start   - Start of the node array being modified
   *   initial_flags- Flags at the start of this array
   *   eos_position - Index of the EOS node to remove
   *   counter      - Pointer to trie's element count
   *   head_ptr     - Trie head (for special root handling)
   *   path         - Stack of parent branch points
   *   alloc        - Allocator for memory management
   *
   * ALGORITHM:
   *
   * 1. DESTROY VALUE: Call t_data::destroy on the EOS node's value
   *    (Frees heap memory for big_class types)
   *
   * 2. CHECK IF ARRAY BECOMES EMPTY:
   *    If orig_len == 1 (just the EOS):
   *    - Deallocate the array
   *    - Set parent pointer to null
   *    - Trigger parent branch cleanup if needed
   *
   * 3. CHECK FOR TRAILING HOP/SKIP TRUNCATION:
   *    If EOS was at the end and preceded by HOP/SKIP:
   *    - The HOP/SKIP now leads nowhere
   *    - Truncate the array at the HOP/SKIP start
   *    - Update the preceding node's flags
   *
   * 4. NORMAL RECONSTRUCTION:
   *    - Allocate new array of size (orig_len - 1)
   *    - Copy nodes before EOS position
   *    - Copy nodes after EOS position
   *    - Update flags appropriately
   *
   * FLAG UPDATE COMPLEXITY:
   *
   * The EOS flag can be stored in three places:
   * - In the ref pointer (if EOS is first in array)
   * - In a HOP's new_flags (if HOP precedes EOS)
   * - In a SKIP's new_flags (if SKIP precedes EOS)
   *
   * We must trace through the array to find where the EOS flag is stored
   * and clear it there.
   */
  static bool rebuild_without_eos(node_type* ref, node_type* node_start,
                                  t_flag initial_flags, size_t eos_position,
                                  size_t* counter, node_type* head_ptr,
                                  std::vector<remove_path_entry>& path,
                                  A& alloc) {
    //--------------------------------------------------------------------------
    // Calculate original array size for deallocation
    //--------------------------------------------------------------------------
    auto orig_len = insert_helper<T, fixed_len, A>::node_array_sz(
        node_start, initial_flags);

    //--------------------------------------------------------------------------
    // Destroy the stored value (important for big_class types)
    //--------------------------------------------------------------------------
    node_type::t_data::destroy(node_start[eos_position].raw(), alloc);

    //--------------------------------------------------------------------------
    // CASE: Array had only the EOS - becomes completely empty
    //--------------------------------------------------------------------------
    if (orig_len == 1) {
      node_type::deallocate(node_start, alloc_size(orig_len), alloc);

      // Set parent pointer to null
      auto p = ref->get_ptr();
      p.set_ptr(nullptr);
      p.set_byte(0);
      ref->set_ptr(p);

      (*counter)--;

      // If we have a parent branch, it now has an empty child - clean it up
      if (path.size() > 0) {
        auto& parent = path.back();
        if (parent.branch_node != nullptr) {
          return remove_child_from_branch(path, counter, alloc);
        }
      }
      return true;
    }

    //--------------------------------------------------------------------------
    // Calculate what's before and after the EOS
    //--------------------------------------------------------------------------
    size_t before = eos_position;
    size_t after = orig_len - eos_position - 1;

    //--------------------------------------------------------------------------
    // CHECK FOR TRAILING HOP/SKIP TRUNCATION
    //--------------------------------------------------------------------------
    // If there's nothing after the EOS (after == 0) and something before,
    // check if the content before is just a HOP/SKIP leading to this EOS.
    // If so, we can truncate the entire HOP/SKIP.
    //
    // Example: Array is [HOP "xyz", EOS]
    // After removing EOS: [HOP "xyz"] leads nowhere - truncate it
    //--------------------------------------------------------------------------
    bool need_truncate = false;
    size_t truncate_at = before;

    if (after == 0 && before > 0) {
      size_t pos = 0;
      t_flag scan_flags = initial_flags;
      size_t last_hop_skip_start = 0;
      bool found_hop_skip_before_eos = false;

      // Scan through to find if a HOP/SKIP leads directly to this EOS
      while (pos < before) {
        if (has_bit(scan_flags, eos_bit)) {
          pos++;
          scan_flags &= ~eos_bit;
        }
        if (has_bit(scan_flags, hop_bit)) {
          last_hop_skip_start = pos;
          t_hop hop = node_start[pos].get_hop();
          scan_flags = hop.get_new_flags();
          pos++;
          if (pos == before && has_bit(scan_flags, eos_bit)) {
            found_hop_skip_before_eos = true;
            truncate_at = last_hop_skip_start;
          }
        } else if (has_bit(scan_flags, skip_bit)) {
          last_hop_skip_start = pos;
          t_skip sk = node_start[pos].get_skip();
          size_t slen = sk.get_skip_len();
          scan_flags = sk.get_new_flags();
          pos += 1 + t_skip::num_skip_nodes(slen);
          if (pos == before && has_bit(scan_flags, eos_bit)) {
            found_hop_skip_before_eos = true;
            truncate_at = last_hop_skip_start;
          }
        } else {
          break;
        }
      }
      need_truncate = found_hop_skip_before_eos;
    }

    //--------------------------------------------------------------------------
    // Calculate new array size
    //--------------------------------------------------------------------------
    size_t new_len = need_truncate ? (truncate_at + after) : (before + after);

    //--------------------------------------------------------------------------
    // CASE: Nothing remains after removal
    //--------------------------------------------------------------------------
    if (new_len == 0) {
      node_type::deallocate(node_start, alloc_size(orig_len), alloc);
      auto p = ref->get_ptr();
      p.set_ptr(nullptr);
      p.set_byte(0);
      ref->set_ptr(p);

      (*counter)--;
      if (path.size() > 0) {
        auto& parent = path.back();
        if (parent.branch_node != nullptr) {
          return remove_child_from_branch(path, counter, alloc);
        }
      }
      return true;
    }

    //--------------------------------------------------------------------------
    // Allocate new array and copy content
    //--------------------------------------------------------------------------
    node_type* nn = node_type::allocate(alloc_size(new_len), alloc);

    if (need_truncate) {
      //------------------------------------------------------------------------
      // TRUNCATION PATH: Remove trailing HOP/SKIP that led to EOS
      //------------------------------------------------------------------------
      if (truncate_at > 0)
        memcpy(nn, node_start, truncate_at * sizeof(node_type));
      if (after > 0)
        memcpy(nn + truncate_at, node_start + eos_position + 1,
               after * sizeof(node_type));

      // Update flags in the HOP/SKIP that precedes the truncation point
      if (truncate_at > 0) {
        size_t pos = 0;
        t_flag scan_flags = initial_flags;
        while (pos < truncate_at) {
          if (has_bit(scan_flags, eos_bit)) {
            pos++;
            scan_flags &= ~eos_bit;
          }
          if (has_bit(scan_flags, hop_bit)) {
            t_hop hop = nn[pos].get_hop();
            t_flag next_flags = hop.get_new_flags();
            size_t next_pos = pos + 1;
            if (has_bit(next_flags, eos_bit)) next_pos++;
            if (next_pos >= truncate_at) {
              // This HOP's continuation is being truncated - clear path bits
              auto arr = to_char_static(hop.to_u64());
              arr[t_hop::new_flags_offset] =
                  static_cast<char>(next_flags & ~(hop_bit | skip_bit));
              nn[pos].set_hop(t_hop::from_u64(from_char_static(arr)));
            }
            scan_flags = next_flags;
            pos++;
          } else if (has_bit(scan_flags, skip_bit)) {
            t_skip sk = nn[pos].get_skip();
            size_t slen = sk.get_skip_len();
            t_flag next_flags = sk.get_new_flags();
            size_t next_pos = pos + 1 + t_skip::num_skip_nodes(slen);
            if (has_bit(next_flags, eos_bit)) next_pos++;
            if (next_pos >= truncate_at) {
              nn[pos].set_skip(t_skip{
                  slen,
                  static_cast<uint8_t>(next_flags & ~(hop_bit | skip_bit))});
            }
            scan_flags = next_flags;
            pos += 1 + t_skip::num_skip_nodes(slen);
          } else {
            break;
          }
        }
      }

      t_flag new_initial_flags = initial_flags;
      if (truncate_at == 0) new_initial_flags &= ~(hop_bit | skip_bit);
      auto p = ref->get_ptr();
      p.set_byte(new_initial_flags);
      p.set_ptr(nn);
      ref->set_ptr(p);
    } else {
      //------------------------------------------------------------------------
      // NORMAL PATH: Simple EOS removal
      //------------------------------------------------------------------------
      if (before > 0) memcpy(nn, node_start, before * sizeof(node_type));
      if (after > 0)
        memcpy(nn + before, node_start + eos_position + 1,
               after * sizeof(node_type));

      t_flag new_initial_flags = initial_flags;

      //------------------------------------------------------------------------
      // Update flags to clear the EOS bit
      //------------------------------------------------------------------------
      // We need to find where the EOS flag is stored and clear it there.
      // This could be:
      // - In initial_flags (if EOS is at position 0)
      // - In a HOP's new_flags
      // - In a SKIP's new_flags
      //------------------------------------------------------------------------
      if (eos_position == 0) {
        // EOS was first - clear it from initial flags
        new_initial_flags &= ~eos_bit;
      } else {
        // EOS came after something - find what set the EOS flag
        size_t pos = 0;
        t_flag scan_flags = initial_flags;
        size_t last_hop_skip_pos = 0;
        bool is_hop = false;

        while (pos < eos_position) {
          if (has_bit(scan_flags, eos_bit)) {
            pos++;
            scan_flags &= ~eos_bit;
          }
          if (has_bit(scan_flags, hop_bit)) {
            last_hop_skip_pos = pos;
            is_hop = true;
            t_hop hop = node_start[pos].get_hop();
            scan_flags = hop.get_new_flags();
            pos++;
          } else if (has_bit(scan_flags, skip_bit)) {
            last_hop_skip_pos = pos;
            is_hop = false;
            t_skip sk = node_start[pos].get_skip();
            size_t slen = sk.get_skip_len();
            scan_flags = sk.get_new_flags();
            pos += 1 + t_skip::num_skip_nodes(slen);
          } else {
            break;
          }
        }

        // If we found a HOP/SKIP whose new_flags contained EOS, clear it
        if (pos == eos_position && has_bit(scan_flags, eos_bit)) {
          if (is_hop) {
            t_hop hop = nn[last_hop_skip_pos].get_hop();
            auto arr = to_char_static(hop.to_u64());
            t_flag old_flags =
                static_cast<uint8_t>(arr[t_hop::new_flags_offset]);
            arr[t_hop::new_flags_offset] =
                static_cast<char>(old_flags & ~eos_bit);
            nn[last_hop_skip_pos].set_hop(
                t_hop::from_u64(from_char_static(arr)));
          } else {
            t_skip sk = nn[last_hop_skip_pos].get_skip();
            nn[last_hop_skip_pos].set_skip(
                t_skip{sk.get_skip_len(),
                       static_cast<uint8_t>(sk.get_new_flags() & ~eos_bit)});
          }
        } else if (has_bit(initial_flags, eos_bit) && eos_position == 1) {
          // Edge case: EOS was at position 1 with initial EOS flag
          new_initial_flags &= ~eos_bit;
        }
      }

      auto p = ref->get_ptr();
      p.set_byte(new_initial_flags);
      p.set_ptr(nn);
      ref->set_ptr(p);
    }

    node_type::deallocate(node_start, alloc_size(orig_len), alloc);
    (*counter)--;
    return true;
  }

  /**
   * @brief Remove a child from a branch (LIST or POP)
   *
   * Entry point for branch cleanup after a child becomes empty.
   * Determines the branch type and dispatches to the appropriate handler.
   */
  static bool remove_child_from_branch(std::vector<remove_path_entry>& path,
                                       size_t* counter, A& alloc) {
    auto& parent = path.back();

    // Scan through HOP/SKIP/EOS to find the LIST/POP flags
    t_flag branch_flags = parent.initial_flags;
    node_type* run = parent.node_start;
    while (has_bit(branch_flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(branch_flags, eos_bit)) {
        run++;
        branch_flags &= ~eos_bit;
      }
      if (has_bit(branch_flags, hop_bit)) {
        branch_flags = run->get_hop().get_new_flags();
        run++;
      } else if (has_bit(branch_flags, skip_bit)) {
        t_skip sk = run->get_skip();
        branch_flags = sk.get_new_flags();
        run += 1 + t_skip::num_skip_nodes(sk.get_skip_len());
      } else {
        break;
      }
    }

    if (has_bit(branch_flags, list_bit)) {
      return remove_from_list(path, parent.child_index, counter, alloc);
    } else {
      return remove_from_pop(path, parent.child_index, counter, alloc);
    }
  }

  /**
   * @brief Remove a child from a LIST branch
   *
   * ============================================================================
   * LIST CHILD REMOVAL
   * ============================================================================
   *
   * When removing a child from a LIST:
   *
   * 1. If LIST has only 1 child ? call remove_last_branch
   *
   * 2. Otherwise:
   *    - Allocate new array (orig_len - 1)
   *    - Copy content up to and including LIST node
   *    - Update LIST: remove character, decrement count
   *    - Copy child pointers, skipping the removed one
   *
   * LIST UPDATE:
   * The LIST stores characters in big-endian order in bytes 0-6, count in
   * byte 7. To remove character at index i:
   *   - Shift chars[i+1..n-1] left by one position
   *   - Decrement count
   */
  static bool remove_from_list(std::vector<remove_path_entry>& path,
                               int child_index, size_t* counter, A& alloc) {
    auto& parent = path.back();
    node_type* node_start = parent.node_start;
    t_flag initial_flags = parent.initial_flags;
    node_type* ref = parent.ref;
    node_type* branch_node = parent.branch_node;

    auto list = branch_node->get_list();
    int lsz = list.get_list_sz();
    size_t list_pos = branch_node - node_start;

    //--------------------------------------------------------------------------
    // Special case: removing last child from LIST
    //--------------------------------------------------------------------------
    if (lsz == 1) return remove_last_branch(path, list_pos, counter, alloc);

    //--------------------------------------------------------------------------
    // Rebuild array with one less child
    //--------------------------------------------------------------------------
    auto orig_len = insert_helper<T, fixed_len, A>::node_array_sz(
        node_start, initial_flags);
    size_t new_len = orig_len - 1;
    node_type* nn = node_type::allocate(alloc_size(new_len), alloc);

    // Copy everything up to and including the LIST header
    memcpy(nn, node_start, (list_pos + 1) * sizeof(node_type));

    //--------------------------------------------------------------------------
    // Update LIST: shift characters left, decrement count
    //--------------------------------------------------------------------------
    auto arr = to_char_static(list.to_u64());
    for (int i = child_index; i < lsz - 1; ++i) arr[i] = arr[i + 1];
    arr[7] = static_cast<char>(lsz - 1);  // New count
    nn[list_pos].set_list(t_small_list::from_u64(from_char_static(arr)));

    //--------------------------------------------------------------------------
    // Copy child pointers, skipping the removed one
    //--------------------------------------------------------------------------
    size_t src = list_pos + 1, dst = list_pos + 1;
    for (int i = 0; i < lsz; ++i) {
      if (i != child_index) nn[dst++].set_raw(node_start[src].raw());
      src++;
    }

    auto p = ref->get_ptr();
    p.set_ptr(nn);
    ref->set_ptr(p);
    node_type::deallocate(node_start, alloc_size(orig_len), alloc);
    return true;
  }

  /**
   * @brief Remove a child from a POP bitmap branch
   *
   * ============================================================================
   * POP CHILD REMOVAL
   * ============================================================================
   *
   * When removing a child from a POP:
   *
   * 1. If POP has only 1 child ? call remove_last_branch
   *
   * 2. If POP will have ?7 children after removal ? convert to LIST
   *    (POP is only efficient with 8+ children)
   *
   * 3. Otherwise:
   *    - Allocate new array (orig_len - 1)
   *    - Copy content up to POP
   *    - Update POP bitmap: clear the removed character's bit
   *    - Copy child pointers, skipping the removed one
   *
   * BIT CLEARING:
   * To remove character c from POP:
   *   word = c >> 6          (which 64-bit word)
   *   bit = c & 63           (which bit in word)
   *   pop[word] &= ~(1ULL << bit)
   */
  static bool remove_from_pop(std::vector<remove_path_entry>& path,
                              int child_index, size_t* counter, A& alloc) {
    auto& parent = path.back();
    node_type* node_start = parent.node_start;
    t_flag initial_flags = parent.initial_flags;
    node_type* ref = parent.ref;
    node_type* branch_node = parent.branch_node;

    auto pop = reinterpret_cast<t_val*>(branch_node);
    auto chars = get_pop_chars(pop);
    size_t pop_pos = branch_node - node_start;
    char removed_char = chars[child_index];

    //--------------------------------------------------------------------------
    // Special case: removing last child from POP
    //--------------------------------------------------------------------------
    if (chars.size() == 1)
      return remove_last_branch(path, pop_pos, counter, alloc);

    //--------------------------------------------------------------------------
    // Convert to LIST if children will be ?7
    //--------------------------------------------------------------------------
    if (chars.size() - 1 <= t_small_list::max_list) {
      return pop_to_list_on_remove(path, child_index, chars, counter, alloc);
    }

    //--------------------------------------------------------------------------
    // Rebuild array with one less child
    //--------------------------------------------------------------------------
    auto orig_len = insert_helper<T, fixed_len, A>::node_array_sz(
        node_start, initial_flags);
    size_t new_len = orig_len - 1;
    node_type* nn = node_type::allocate(alloc_size(new_len), alloc);
    memcpy(nn, node_start, pop_pos * sizeof(node_type));

    //--------------------------------------------------------------------------
    // Update POP bitmap: clear the bit for removed character
    //--------------------------------------------------------------------------
    t_val new_pop[4] = {pop[0], pop[1], pop[2], pop[3]};
    uint8_t v = static_cast<uint8_t>(removed_char);
    new_pop[v >> 6] &= ~(1ULL << (v & 63));  // Clear the bit

    nn[pop_pos].set_pop(new_pop[0]);
    nn[pop_pos + 1].set_pop(new_pop[1]);
    nn[pop_pos + 2].set_pop(new_pop[2]);
    nn[pop_pos + 3].set_pop(new_pop[3]);

    //--------------------------------------------------------------------------
    // Copy child pointers, skipping the removed one
    //--------------------------------------------------------------------------
    size_t src = pop_pos + 4, dst = pop_pos + 4;
    for (size_t i = 0; i < chars.size(); ++i) {
      if (static_cast<int>(i) != child_index)
        nn[dst++].set_raw(node_start[src].raw());
      src++;
    }

    auto p = ref->get_ptr();
    p.set_ptr(nn);
    ref->set_ptr(p);
    node_type::deallocate(node_start, alloc_size(orig_len), alloc);
    return true;
  }

  /**
   * @brief Convert POP to LIST when children decrease below threshold
   *
   * ============================================================================
   * POP TO LIST CONVERSION
   * ============================================================================
   *
   * When a POP has ?7 children remaining, it's more efficient to use a LIST.
   *
   * CONVERSION STEPS:
   * 1. Allocate new array with LIST structure (smaller than POP)
   * 2. Copy content before POP
   * 3. Create LIST header with remaining characters
   * 4. Copy child pointers (in same sorted order)
   * 5. Update flags: clear pop_bit, set list_bit
   *
   * FLAGS UPDATE:
   * The pop_bit/list_bit may be in:
   * - The ref pointer's flags
   * - A preceding HOP's new_flags
   * - A preceding SKIP's new_flags
   * We must update in the correct location.
   */
  static bool pop_to_list_on_remove(std::vector<remove_path_entry>& path,
                                    int child_index,
                                    const std::vector<char>& chars,
                                    size_t* counter, A& alloc) {
    auto& parent = path.back();
    node_type* node_start = parent.node_start;
    t_flag initial_flags = parent.initial_flags;
    node_type* ref = parent.ref;
    node_type* branch_node = parent.branch_node;

    size_t pop_pos = branch_node - node_start;
    size_t new_children = chars.size() - 1;

    // New array: content before POP + LIST header + child pointers
    // LIST takes 1 node vs POP's 4, so we save 3 nodes
    size_t new_len = pop_pos + 1 + new_children;

    t_flag new_flags = (initial_flags & ~pop_bit) | list_bit;
    node_type* nn = node_type::allocate(alloc_size(new_len), alloc);
    memcpy(nn, node_start, pop_pos * sizeof(node_type));

    //--------------------------------------------------------------------------
    // Update flags in HOP/SKIP if present
    //--------------------------------------------------------------------------
    if (has_bit(initial_flags, hop_bit)) {
      t_hop hop = nn[0].get_hop();
      auto arr = to_char_static(hop.to_u64());
      t_flag hop_flags = static_cast<uint8_t>(arr[t_hop::new_flags_offset]);
      arr[t_hop::new_flags_offset] =
          static_cast<char>((hop_flags & ~pop_bit) | list_bit);
      nn[0].set_hop(t_hop::from_u64(from_char_static(arr)));
    } else if (has_bit(initial_flags, skip_bit)) {
      t_skip sk = nn[0].get_skip();
      nn[0].set_skip(t_skip{
          sk.get_skip_len(),
          static_cast<uint8_t>((sk.get_new_flags() & ~pop_bit) | list_bit)});
    }

    //--------------------------------------------------------------------------
    // Build LIST header with remaining characters
    //--------------------------------------------------------------------------
    auto list_arr = to_char_static(0ULL);
    int list_idx = 0;
    for (size_t i = 0; i < chars.size(); ++i) {
      if (static_cast<int>(i) != child_index) list_arr[list_idx++] = chars[i];
    }
    list_arr[7] = static_cast<char>(new_children);  // Count
    nn[pop_pos].set_list(t_small_list::from_u64(from_char_static(list_arr)));

    //--------------------------------------------------------------------------
    // Copy child pointers (skip the removed one)
    //--------------------------------------------------------------------------
    size_t src = pop_pos + 4, dst = pop_pos + 1;
    for (size_t i = 0; i < chars.size(); ++i) {
      if (static_cast<int>(i) != child_index)
        nn[dst++].set_raw(node_start[src].raw());
      src++;
    }

    auto orig_len = insert_helper<T, fixed_len, A>::node_array_sz(
        node_start, initial_flags);
    auto p = ref->get_ptr();
    p.set_byte(new_flags);
    p.set_ptr(nn);
    ref->set_ptr(p);
    node_type::deallocate(node_start, alloc_size(orig_len), alloc);
    return true;
  }

  /**
   * @brief Remove the last branch from a node array
   *
   * ============================================================================
   * REMOVING LAST BRANCH
   * ============================================================================
   *
   * When the last child is removed from a branch, the branch itself should
   * be removed. However, we must check if there's other content (like an EOS)
   * that should remain.
   *
   * CASES:
   *
   * 1. No content before branch:
   *    Array was just the branch - deallocate and propagate up
   *
   * 2. EOS exists before branch:
   *    Keep the EOS and any preceding HOP/SKIP
   *    Just remove the LIST/POP and its pointer
   *
   * 3. Only HOP/SKIP before branch (no EOS):
   *    The HOP/SKIP led only to this branch - deallocate and propagate up
   */
  static bool remove_last_branch(std::vector<remove_path_entry>& path,
                                 size_t branch_pos, size_t* counter, A& alloc) {
    auto& parent = path.back();
    node_type* node_start = parent.node_start;
    t_flag initial_flags = parent.initial_flags;
    node_type* ref = parent.ref;

    auto orig_len = insert_helper<T, fixed_len, A>::node_array_sz(
        node_start, initial_flags);
    bool has_content_before = (branch_pos > 0);

    //--------------------------------------------------------------------------
    // CASE 1: No content before branch
    //--------------------------------------------------------------------------
    if (!has_content_before) {
      node_type::deallocate(node_start, alloc_size(orig_len), alloc);
      auto p = ref->get_ptr();
      p.set_ptr(nullptr);
      p.set_byte(0);
      ref->set_ptr(p);

      // Propagate cleanup to parent
      if (path.size() > 1) {
        path.pop_back();
        return remove_child_from_branch(path, counter, alloc);
      }
      return true;
    }

    //--------------------------------------------------------------------------
    // Check if remaining content has an EOS
    //--------------------------------------------------------------------------
    t_flag remaining_flags = initial_flags & ~(list_bit | pop_bit);
    bool has_eos = has_bit(remaining_flags, eos_bit);

    if (!has_eos) {
      // Scan through HOP/SKIP to check for EOS in their new_flags
      t_flag scan_flags = remaining_flags;
      node_type* run = node_start;
      while (has_bit(scan_flags, hop_bit | skip_bit)) {
        if (has_bit(scan_flags, hop_bit)) {
          scan_flags = run->get_hop().get_new_flags();
          run++;
        } else if (has_bit(scan_flags, skip_bit)) {
          t_skip sk = run->get_skip();
          scan_flags = sk.get_new_flags();
          run += 1 + t_skip::num_skip_nodes(sk.get_skip_len());
        }
        if (has_bit(scan_flags, eos_bit)) {
          has_eos = true;
          break;
        }
      }
    }

    //--------------------------------------------------------------------------
    // CASE 3: No EOS - everything here was just path to branch
    //--------------------------------------------------------------------------
    if (!has_eos) {
      node_type::deallocate(node_start, alloc_size(orig_len), alloc);
      auto p = ref->get_ptr();
      p.set_ptr(nullptr);
      p.set_byte(0);
      ref->set_ptr(p);
      if (path.size() > 1) {
        path.pop_back();
        return remove_child_from_branch(path, counter, alloc);
      }
      return true;
    }

    //--------------------------------------------------------------------------
    // CASE 2: Has EOS - keep content before branch, remove branch
    //--------------------------------------------------------------------------
    t_flag new_flags = remaining_flags;
    size_t new_len = branch_pos;
    node_type* nn = node_type::allocate(alloc_size(new_len), alloc);
    memcpy(nn, node_start, new_len * sizeof(node_type));

    // Update the last HOP/SKIP's flags to remove LIST/POP bits
    size_t pos = 0;
    t_flag scan_flags = initial_flags;
    size_t last_hop_skip_pos = 0;
    bool found_hop_skip = false;
    bool is_hop = false;

    while (pos < branch_pos) {
      if (has_bit(scan_flags, eos_bit)) {
        pos++;
        scan_flags &= ~eos_bit;
      }
      if (has_bit(scan_flags, hop_bit)) {
        last_hop_skip_pos = pos;
        found_hop_skip = true;
        is_hop = true;
        t_hop hop = node_start[pos].get_hop();
        scan_flags = hop.get_new_flags();
        pos++;
      } else if (has_bit(scan_flags, skip_bit)) {
        last_hop_skip_pos = pos;
        found_hop_skip = true;
        is_hop = false;
        t_skip sk = node_start[pos].get_skip();
        size_t slen = sk.get_skip_len();
        scan_flags = sk.get_new_flags();
        pos += 1 + t_skip::num_skip_nodes(slen);
      } else {
        break;
      }
    }

    if (found_hop_skip) {
      if (is_hop) {
        t_hop hop = nn[last_hop_skip_pos].get_hop();
        auto arr = to_char_static(hop.to_u64());
        t_flag old_flags = static_cast<uint8_t>(arr[t_hop::new_flags_offset]);
        arr[t_hop::new_flags_offset] =
            static_cast<char>(old_flags & ~(list_bit | pop_bit));
        nn[last_hop_skip_pos].set_hop(t_hop::from_u64(from_char_static(arr)));
      } else {
        t_skip sk = nn[last_hop_skip_pos].get_skip();
        nn[last_hop_skip_pos].set_skip(t_skip{
            sk.get_skip_len(),
            static_cast<uint8_t>(sk.get_new_flags() & ~(list_bit | pop_bit))});
      }
    }

    auto p = ref->get_ptr();
    p.set_byte(new_flags);
    p.set_ptr(nn);
    ref->set_ptr(p);
    node_type::deallocate(node_start, alloc_size(orig_len), alloc);
    return true;
  }

 public:
  /**
   * @brief Main remove loop
   *
   * ============================================================================
   * REMOVE_LOOP: Main entry point for key removal
   * ============================================================================
   *
   * PARAMETERS:
   *   in       - Key data to remove
   *   sz       - Key length
   *   counter  - Pointer to trie's element count (decremented on success)
   *   head_ptr - Trie's head node
   *   alloc    - Allocator for memory operations
   *
   * RETURNS: true if key was found and removed, false if not found
   *
   * ALGORITHM OVERVIEW:
   *
   * 1. INITIALIZATION:
   *    - Get root pointer and flags from head_ptr
   *    - Initialize empty path stack
   *    - Set up key pointer and traversal state
   *
   * 2. TRAVERSAL LOOP:
   *    For each node array:
   *
   *    a) Process HOP/SKIP path compression:
   *       - Check if key matches the compressed path
   *       - If mismatch: return false (key doesn't exist)
   *       - If match: advance key pointer and continue
   *
   *    b) Check for EOS:
   *       - If key is exhausted and EOS exists: FOUND IT!
   *       - Call rebuild_without_eos to remove
   *
   *    c) Process branch (LIST or POP):
   *       - Look up current key character in branch
   *       - If not found: return false
   *       - If found: push path entry and descend
   *
   * 3. PATH TRACKING:
   *    At each branch point, we push a remove_path_entry containing:
   *    - ref: parent node (for updating pointer)
   *    - node_start: this array's start
   *    - initial_flags: flags for this array
   *    - child_index: which child we're descending into
   *    - branch_node: the LIST or POP node
   *
   *    This enables cleanup propagation after removal.
   *
   * FIXED_LEN VS VARIABLE_LEN:
   *
   * For fixed_len keys (numeric types):
   * - Simpler structure: at most one HOP or SKIP per array
   * - EOS only appears when all bytes consumed
   * - Slightly optimized traversal
   *
   * For variable_len keys (strings):
   * - May have multiple EOS points (prefixes can be keys)
   * - More complex flag handling
   * - Must check EOS at each position
   */
  static bool remove_loop(const char* in, size_t sz, size_t* counter,
                          node_type* head_ptr, A& alloc) {
    //--------------------------------------------------------------------------
    // Get root from head pointer
    //--------------------------------------------------------------------------
    auto [ptr, flags] = head_ptr->get_ptr().get_both();
    if (!ptr) return false;  // Empty trie

    //--------------------------------------------------------------------------
    // Initialize traversal state
    //--------------------------------------------------------------------------
    std::vector<remove_path_entry> path;
    node_type* ref = head_ptr;
    node_type* run = ptr;
    node_type* node_start = ptr;
    t_flag flags_now = flags;
    t_flag initial_flags = flags;
    const char* key = in;
    const char* const last = in + sz;

    //==========================================================================
    // FIXED-LENGTH KEY PATH (numeric types)
    //==========================================================================
    if constexpr (fixed_len > 0) {
      for (;;) {
        size_t eos_position = run - node_start;

        //----------------------------------------------------------------------
        // Process HOP if present
        //----------------------------------------------------------------------
        if (has_bit(flags_now, hop_bit)) {
          const auto& hop = run->get_hop();
          size_t hop_sz = hop.get_hop_sz();
          size_t remaining = last - key;

          // Check length
          if (remaining < hop_sz) return false;

          // Check match
          if (!hop.matches(key, remaining)) return false;

          key += hop_sz;
          flags_now = hop.get_new_flags();
          run++;
          eos_position = run - node_start;
        }
        //----------------------------------------------------------------------
        // Process SKIP if present
        //----------------------------------------------------------------------
        else if (has_bit(flags_now, skip_bit)) {
          t_skip sk = run->get_skip();
          size_t slen = sk.get_skip_len();
          size_t remaining = last - key;

          if (remaining < slen) return false;

          run++;
          if (!do_find_skip(reinterpret_cast<const char*>(run), key, slen))
            return false;

          run += t_skip::num_skip_nodes(slen);
          key += slen;
          flags_now = sk.get_new_flags();
          eos_position = run - node_start;
        }

        //----------------------------------------------------------------------
        // Check for EOS (key found!)
        //----------------------------------------------------------------------
        if (has_bit(flags_now, eos_bit)) {
          if (key == last) {
            // Key matches exactly - remove it!
            return rebuild_without_eos(ref, node_start, initial_flags,
                                       eos_position, counter, head_ptr, path,
                                       alloc);
          }
          // For fixed_len, EOS means we consumed all bytes but key continues
          return false;
        }

        //----------------------------------------------------------------------
        // Must have a branch to continue
        //----------------------------------------------------------------------
        if (!has_bit(flags_now, list_bit | pop_bit)) return false;
        if (key >= last) return false;

        //----------------------------------------------------------------------
        // Look up character in branch
        //----------------------------------------------------------------------
        node_type* branch_node = run;
        int child_index;

        if (has_bit(flags_now, list_bit)) {
          int off = run->get_list().offset(*key);
          if (off == 0) return false;  // Character not in LIST
          child_index = off - 1;       // Convert to 0-based index
          run += off;
        } else {
          int off;
          if (!do_find_pop(reinterpret_cast<const t_val*>(run), *key, &off))
            return false;         // Character not in POP
          child_index = off - 4;  // Subtract 4 for POP header words
          run += off;
        }

        //----------------------------------------------------------------------
        // Push path entry and descend
        //----------------------------------------------------------------------
        path.push_back(
            {ref, node_start, initial_flags, child_index, branch_node});
        key++;
        ref = run;
        std::tie(run, flags_now) = run->get_ptr().get_both();
        if (!run) return false;  // Null child pointer
        node_start = run;
        initial_flags = flags_now;
      }
    }
    //==========================================================================
    // VARIABLE-LENGTH KEY PATH (strings)
    //==========================================================================
    else {
      for (;;) {
        size_t eos_position = run - node_start;

        //----------------------------------------------------------------------
        // Process EOS/HOP/SKIP sequence
        // (Strings can have EOS at any point, not just at fixed length)
        //----------------------------------------------------------------------
        while (has_bit(flags_now, eos_bit | hop_bit | skip_bit)) {
          if (has_bit(flags_now, eos_bit)) {
            if (key == last) {
              // Key matches exactly - remove it!
              return rebuild_without_eos(ref, node_start, initial_flags,
                                         eos_position, counter, head_ptr, path,
                                         alloc);
            }
            run++;
            eos_position = run - node_start;
            flags_now &= ~eos_bit;
          }

          if (has_bit(flags_now, hop_bit)) {
            const auto& hop = run->get_hop();
            size_t hop_sz = hop.get_hop_sz();
            size_t remaining = last - key;
            if (remaining < hop_sz) return false;
            if (!hop.matches(key, remaining)) return false;
            key += hop_sz;
            flags_now = hop.get_new_flags();
            run++;
            eos_position = run - node_start;
          } else if (has_bit(flags_now, skip_bit)) {
            t_skip sk = run->get_skip();
            size_t slen = sk.get_skip_len();
            size_t remaining = last - key;
            if (remaining < slen) return false;
            run++;
            if (!do_find_skip(reinterpret_cast<const char*>(run), key, slen))
              return false;
            run += t_skip::num_skip_nodes(slen);
            key += slen;
            flags_now = sk.get_new_flags();
            eos_position = run - node_start;
          } else {
            break;
          }
        }

        //----------------------------------------------------------------------
        // Check for EOS after HOP/SKIP processing
        //----------------------------------------------------------------------
        if (has_bit(flags_now, eos_bit)) {
          if (key == last) {
            return rebuild_without_eos(ref, node_start, initial_flags,
                                       eos_position, counter, head_ptr, path,
                                       alloc);
          }
          run++;
          flags_now &= ~eos_bit;
        }

        //----------------------------------------------------------------------
        // Must have a branch to continue
        //----------------------------------------------------------------------
        if (!has_bit(flags_now, list_bit | pop_bit)) return false;
        if (key >= last) return false;

        //----------------------------------------------------------------------
        // Look up character in branch
        //----------------------------------------------------------------------
        node_type* branch_node = run;
        int child_index;

        if (has_bit(flags_now, list_bit)) {
          int off = run->get_list().offset(*key);
          if (off == 0) return false;
          child_index = off - 1;
          run += off;
        } else {
          int off;
          if (!do_find_pop(reinterpret_cast<const t_val*>(run), *key, &off))
            return false;
          child_index = off - 4;
          run += off;
        }

        //----------------------------------------------------------------------
        // Push path entry and descend
        //----------------------------------------------------------------------
        path.push_back(
            {ref, node_start, initial_flags, child_index, branch_node});
        key++;
        ref = run;
        std::tie(run, flags_now) = run->get_ptr().get_both();
        if (!run) return false;
        node_start = run;
        initial_flags = flags_now;
      }
    }
  }
};

}  // namespace gteitelbaum