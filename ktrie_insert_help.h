/**
 * @file ktrie_insert_help.h
 * @brief Insert operation helpers for KTRIE
 * 
 * This file contains the core insert/update logic for KTRIE. It handles:
 * - Creating new node arrays for keys
 * - Breaking HOP/SKIP nodes when keys diverge
 * - Converting LIST to POP when branches grow
 * - Managing flags across node array modifications
 * 
 * The insert algorithm traverses the trie matching key characters,
 * and when it needs to modify the structure, it creates new node arrays
 * with the appropriate changes (breaking HOPs, adding branches, etc.).
 * 
 * ============================================================================
 * WHY EARLY RETURNS INSTEAD OF GOTOS
 * ============================================================================
 * 
 * The insert algorithm uses helper functions with early returns rather than
 * gotos for several reasons:
 * 
 * 1. Each modification case (break_hop, break_skip, add_list, etc.) is
 *    complex enough to warrant its own function for clarity and testing.
 * 
 * 2. After any modification, the insert is complete - we don't need to
 *    continue the main loop. Early returns make this clear.
 * 
 * 3. The tail creation phase (make_tail) happens in the caller after
 *    the main loop returns, providing clean separation of concerns.
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <memory>
#include <tuple>
#include <vector>

#include "ktrie_flags_loc.h"
#include "ktrie_node.h"

namespace gteitelbaum {

/**
 * @class insert_helper
 * @brief Static helper functions for insert operations
 * 
 * ============================================================================
 * INSERT ALGORITHM OVERVIEW
 * ============================================================================
 * 
 * The insert algorithm has several distinct phases:
 * 
 * PHASE 1: TRAVERSAL
 *   Navigate down the trie, matching input characters against:
 *   - HOP nodes (1-6 inline characters)
 *   - SKIP nodes (7+ characters)
 *   - Branch points (LIST or POP)
 *   
 *   During traversal, we track:
 *   - Current position in the node array (m.run)
 *   - Current flags being processed (m.flags)
 *   - Where to write flag updates (m.flags_writer)
 *   - Reference node for the current array (m.ref)
 * 
 * PHASE 2: MODIFICATION (one of these cases)
 * 
 *   Case A: Key already exists (EOS found with no remaining input)
 *     → If do_update is true, update the value
 *     → Return existing value pointer, cnt = 0
 * 
 *   Case B: Key diverges in the middle of a HOP
 *     → Call break_hop_at() to split the HOP
 *     → Create a branch point at the divergence
 *     → One branch continues the existing HOP suffix
 *     → Other branch gets the new key's remaining characters
 * 
 *   Case C: Key diverges in the middle of a SKIP
 *     → Call break_skip_at() similar to HOP
 *     → SKIP may be converted to HOP if suffix is short enough
 * 
 *   Case D: Key needs a new branch at a LIST
 *     → If LIST has room (< 7 children): add_list()
 *     → If LIST is full (7 children): list2pop() converts to bitmap
 * 
 *   Case E: Key needs a new branch at a POP
 *     → Call add_pop() to set bit and add child pointer
 * 
 *   Case F: Key extends past current node array (no more HOP/SKIP/branch)
 *     → Call add_branch_at_end() or add_eos_at()
 * 
 * PHASE 3: TAIL CREATION
 *   If tail_ptr is set after modification, we need to create nodes for
 *   the remaining key characters. make_tail() creates:
 *   - HOP node if remaining <= 6 characters
 *   - SKIP node if remaining > 6 characters
 *   - Final EOS node with the stored value
 * 
 * ============================================================================
 * FLAG MANAGEMENT
 * ============================================================================
 * 
 * Flags can be stored in three locations:
 * 1. In the parent pointer's high bits (most common)
 * 2. In a HOP node's new_flags field (after HOP processing)
 * 3. In a SKIP node's new_flags field (after SKIP processing)
 * 
 * The flags_writer tracks where to update flags when modifying the tree.
 * This is crucial because when we add an EOS or change HOP/SKIP to LIST,
 * we need to update the flags in the correct location.
 * 
 * @tparam T Value type
 * @tparam fixed_len Key length for numeric types (0 for strings)
 * @tparam A Allocator type
 */
template <class T, size_t fixed_len, class A>
class insert_helper {
 public:
  using node_type = node<T, fixed_len, A>;
  using flags_loc = flags_location<T, fixed_len, A>;

  /**
   * @struct insert_update_ret
   * @brief Return value and state for insert operations
   * 
   * This structure serves dual purposes:
   * 1. Input: Tracks remaining key to insert and where to attach nodes
   * 2. Output: Returns result pointer and count delta
   * 
   * After insert_update_loop returns:
   * - If cnt == 0: Key already existed, ret points to existing value
   * - If cnt == 1 && tail_ptr == nullptr: Key was fully inserted
   * - If cnt == 1 && tail_ptr != nullptr: Need to call make_tail()
   */
  struct insert_update_ret {
    const char* add_val;    ///< Pointer to remaining key data
    const char* add_last;   ///< Pointer to end of key data
    const T* add_ptr;       ///< Pointer to value to insert
    node_type* tail_ptr;    ///< Where to attach new tail nodes
    const T* ret;           ///< Pointer to inserted/found value
    int cnt;                ///< Count delta (1 if inserted, 0 if existed)

    insert_update_ret(const char* i, const char* l, const T* a, node_type* t)
        : add_val(i), add_last(l), add_ptr(a), tail_ptr(t), ret(nullptr), cnt(1) {}

    KTRIE_FORCE_INLINE size_t get_size() const { return add_last - add_val; }
  };

  /**
   * @struct modify_data
   * @brief State tracked during tree modification
   * 
   * Key invariants:
   * - ref always points to a node containing a pointer to node_start
   * - node_start is the first node of the current array
   * - run is our current position within the array (run >= node_start)
   * - flags reflects what type of data run is pointing at
   * - initial_flags is flags at the start of this array (for size calculation)
   * - flags_writer tracks where to update flags if we modify the structure
   */
  struct modify_data {
    node_type* ref;           ///< Node containing pointer to current array
    node_type* node_start;    ///< Start of current node array
    node_type* run;           ///< Current position in array
    t_flag flags;             ///< Current flags being processed
    t_flag initial_flags;     ///< Flags at start of this array
    flags_loc flags_writer;   ///< Where to write flag updates

    modify_data(node_type* r, node_type* s, node_type* rp, t_flag f)
        : ref(r), node_start(s), run(rp), flags(f), initial_flags(f),
          flags_writer(flags_loc::in_ptr(r)) {}

    void set_flags_in_hop(node_type* hop_node) {
      flags_writer = flags_loc::in_hop(hop_node);
    }

    void set_flags_in_skip(node_type* skip_node) {
      flags_writer = flags_loc::in_skip(skip_node);
    }

    void set_flags_in_ref() {
      flags_writer = flags_loc::in_ptr(ref);
    }
  };

 private:
  /**
   * @brief Set a bit in the POP bitmap for a character
   * 
   * POP BITMAP BIT MANIPULATION:
   * ┌────────────────────────────────────────────────────────────────┐
   * │ The 256-bit bitmap uses 4 x 64-bit words.                      │
   * │                                                                │
   * │ For character c (0-255):                                       │
   * │   word index = c >> 6     (divide by 64, gives 0-3)           │
   * │   bit position = c & 63   (mod 64, gives 0-63)                │
   * │   mask = 1ULL << bit_position                                  │
   * │                                                                │
   * │ Example: c = 'A' (65)                                          │
   * │   word = 65 >> 6 = 1                                          │
   * │   bit = 65 & 63 = 1                                           │
   * │   Set pop[1] |= (1ULL << 1)                                   │
   * └────────────────────────────────────────────────────────────────┘
   */
  static KTRIE_FORCE_INLINE void set_pop_for(char c, t_val* pop) {
    uint8_t v = static_cast<uint8_t>(c);
    pop[v >> 6] |= 1ULL << (v & 63);
  }

 public:
  /**
   * @brief Calculate the size of a node array
   */
  static size_t node_array_sz(const node_type* start, t_flag flags) {
    const node_type* run = start;
    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      run += has_bit(flags, eos_bit);
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          flags = run->get_hop().get_new_flags();
          run++;
        } else {
          auto len = run->get_skip().get_skip_len();
          flags = run->get_skip().get_new_flags();
          run += 1 + t_skip::num_skip_nodes(len);
        }
      } else {
        break;
      }
    }
    if (has_bit(flags, list_bit | pop_bit)) {
      if (has_bit(flags, pop_bit)) {
        auto pop = reinterpret_cast<const t_val*>(run);
        run += 4 + std::popcount(pop[0]) + std::popcount(pop[1]) +
               std::popcount(pop[2]) + std::popcount(pop[3]);
      } else {
        run += 1 + run->get_list().get_list_sz();
      }
    }
    return run - start;
  }

 private:
  static node_type* make_new(node_type* ref, t_flag flags, size_t len, A& alloc) {
    auto nn = node_type::allocate(alloc_size(len), alloc);
    auto ptr = ref->get_ptr();
    ptr.set_ptr(nn);
    ptr.set_byte(flags);
    ref->set_ptr(ptr);
    return nn;
  }

  static void update_flags_in_new_array(node_type* nn, size_t pos, 
                                        typename flags_loc::type loc_type,
                                        t_flag new_flags) {
    switch (loc_type) {
      case flags_loc::type::in_ptr:
        break;
      case flags_loc::type::in_hop: {
        t_hop hop = nn[pos].get_hop();
        auto arr = to_char_static(hop.to_u64());
        arr[t_hop::new_flags_offset] = static_cast<char>(new_flags);
        nn[pos].set_hop(t_hop::from_u64(from_char_static(arr)));
        break;
      }
      case flags_loc::type::in_skip: {
        t_skip sk = nn[pos].get_skip();
        nn[pos].set_skip(t_skip{sk.get_skip_len(), static_cast<uint8_t>(new_flags)});
        break;
      }
    }
  }

 public:
  /**
   * @brief Create tail nodes for remaining key
   * 
   * TAIL STRUCTURE:
   * 
   * Case 1: No remaining characters (sz == 0)
   *   [EOS with value]
   * 
   * Case 2: 1-6 remaining characters
   *   [HOP with chars + eos_bit flag] [EOS with value]
   * 
   * Case 3: 7+ remaining characters
   *   [SKIP header] [SKIP data...] [EOS with value]
   */
  static void make_tail(insert_update_ret& t, A& alloc) {
    size_t sz = t.get_size();
    t_flag flags;
    size_t len;
    if (sz == 0) {
      flags = eos_bit;
      len = 1;
    } else if (sz <= t_hop::max_hop) {
      flags = hop_bit;
      len = 2;
    } else {
      flags = skip_bit;
      len = 2 + t_skip::num_skip_nodes(sz);
    }
    node_type* ptr = make_new(t.tail_ptr, flags, len, alloc);
    if (sz == 0) {
      ptr[0].set_data(t.add_ptr, alloc);
    } else if (sz <= t_hop::max_hop) {
      ptr[0].set_hop(t_hop{t.add_val, sz, eos_bit});
      ptr[1].set_data(t.add_ptr, alloc);
    } else {
      ptr[0].set_skip(t_skip{sz, eos_bit});
      node_type::skip_copy(ptr + 1, t.add_val, sz);
      ptr[1 + t_skip::num_skip_nodes(sz)].set_data(t.add_ptr, alloc);
    }
    t.ret = ptr[len - 1].get_data_ptr();
  }

  /**
   * @brief Main insert/update loop
   * 
   * ============================================================================
   * INSERT_UPDATE_LOOP: DETAILED ALGORITHM
   * ============================================================================
   * 
   * This is the heart of the insert algorithm. It traverses the existing
   * trie structure, matching input characters, and when it needs to
   * diverge (mismatch or end of key), it modifies the structure.
   * 
   * THE MAIN LOOP:
   * 
   * The outer for(;;) loop processes node arrays. Each iteration:
   * 1. Processes any HOP/SKIP path compression
   * 2. Checks for EOS (existing key ends here)
   * 3. Processes branch points (LIST/POP)
   * 4. Follows child pointer to next array
   * 
   * CONTROL FLOW:
   * 
   * Instead of deeply nested if-else, we use early returns from
   * helper functions. When a modification is needed:
   * 
   *   break_hop_at()    → Splits HOP, creates branch, RETURNS
   *   break_skip_at()   → Splits SKIP, creates branch, RETURNS
   *   add_list()        → Adds child to LIST, sets tail_ptr, RETURNS
   *   list2pop()        → Converts LIST to POP, sets tail_ptr, RETURNS
   *   add_pop()         → Adds child to POP, sets tail_ptr, RETURNS
   *   add_eos_at()      → Inserts EOS for new key, RETURNS
   *   add_branch_at_end() → Adds new HOP/SKIP at end, RETURNS
   * 
   * After any of these returns, if tail_ptr is set, the caller must
   * call make_tail() to create nodes for remaining key characters.
   */
  static void insert_update_loop(modify_data& m, insert_update_ret& t,
                                 bool do_update, A& alloc) {
    if constexpr (fixed_len > 0) {
      //========================================================================
      // OPTIMIZED PATH FOR FIXED-LENGTH KEYS (numeric types)
      //========================================================================
      for (;;) {
        // Process at most one HOP or SKIP
        if (has_bit(m.flags, hop_bit)) {
          node_type* hn = m.run;
          t_hop hop = m.run->get_hop();
          size_t rem = t.add_last - t.add_val;
          size_t mm = hop.find_mismatch(t.add_val, rem);
          if (mm < static_cast<size_t>(hop.get_hop_sz())) {
            break_hop_at(m, t, hop, mm, alloc);
            return;
          }
          t.add_val += hop.get_hop_sz();
          m.flags = hop.get_new_flags();
          m.set_flags_in_hop(hn);
          m.run++;
        } else if (has_bit(m.flags, skip_bit)) {
          node_type* sn = m.run;
          t_skip sk = m.run->get_skip();
          size_t slen = sk.get_skip_len();
          m.run++;
          const char* sd = reinterpret_cast<const char*>(m.run);
          size_t rem = t.add_last - t.add_val;
          size_t i = 0, cl = std::min(slen, rem);
          while (i < cl && t.add_val[i] == sd[i]) i++;
          if (i < slen) {
            break_skip_at(m, t, i, slen, sd[i], sn, alloc);
            return;
          }
          t.add_val += slen;
          m.flags = sk.get_new_flags();
          m.set_flags_in_skip(sn);
          m.run += t_skip::num_skip_nodes(slen);
        }
        
        if (has_bit(m.flags, eos_bit)) {
          if (t.add_val >= t.add_last) {
            if (do_update) m.run->update_data(t.add_ptr, alloc);
            t.cnt = 0;
            t.ret = m.run->get_data_ptr();
            return;
          }
          m.run++;
        }
        
        if (t.add_val >= t.add_last) {
          add_eos_at(m, t, alloc);
          return;
        }
        
        if (has_bit(m.flags, list_bit | pop_bit)) {
          if (has_bit(m.flags, pop_bit)) {
            auto sr = reinterpret_cast<const t_val*>(m.run);
            int off;
            if (!do_find_pop(sr, *t.add_val, &off)) {
              add_pop(m, t, alloc);
              return;
            }
            m.run += off;
          } else {
            auto list = m.run->get_list();
            int lsz = list.get_list_sz();
            int mt = list.offset(*t.add_val);
            if (mt == 0) {
              if (lsz >= t_small_list::max_list) {
                list2pop(m, t, alloc);
                return;
              }
              add_list(m, t, lsz, alloc);
              return;
            }
            m.run += mt;
          }
          t.add_val++;
          m.ref = m.run;
          std::tie(m.run, m.flags) = m.run->get_ptr().get_both();
          m.node_start = m.run;
          m.initial_flags = m.flags;
          m.set_flags_in_ref();
          
          if (t.add_val >= t.add_last) {
            if (has_bit(m.flags, eos_bit)) {
              if (do_update) m.run->update_data(t.add_ptr, alloc);
              t.cnt = 0;
              t.ret = m.run->get_data_ptr();
              return;
            }
            add_eos_at(m, t, alloc);
            return;
          }
          continue;
        }
        
        add_branch_at_end(m, t, alloc);
        return;
      }
    } else {
      //========================================================================
      // PATH FOR VARIABLE-LENGTH KEYS (strings)
      //========================================================================
      for (;;) {
        while (has_bit(m.flags, eos_bit | hop_bit | skip_bit)) {
          if (has_bit(m.flags, eos_bit)) {
            if (t.add_val >= t.add_last) {
              if (do_update) m.run->update_data(t.add_ptr, alloc);
              t.cnt = 0;
              t.ret = m.run->get_data_ptr();
              return;
            }
            m.run++;
          }
          if (has_bit(m.flags, hop_bit | skip_bit)) {
            if (has_bit(m.flags, hop_bit)) {
              node_type* hn = m.run;
              t_hop hop = m.run->get_hop();
              size_t rem = t.add_last - t.add_val;
              size_t mm = hop.find_mismatch(t.add_val, rem);
              if (mm < static_cast<size_t>(hop.get_hop_sz())) {
                break_hop_at(m, t, hop, mm, alloc);
                return;
              }
              t.add_val += hop.get_hop_sz();
              m.flags = hop.get_new_flags();
              m.set_flags_in_hop(hn);
              m.run++;
            } else {
              node_type* sn = m.run;
              t_skip sk = m.run->get_skip();
              size_t slen = sk.get_skip_len();
              m.run++;
              const char* sd = reinterpret_cast<const char*>(m.run);
              size_t rem = t.add_last - t.add_val;
              size_t i = 0, cl = std::min(slen, rem);
              while (i < cl && t.add_val[i] == sd[i]) i++;
              if (i < slen) {
                break_skip_at(m, t, i, slen, sd[i], sn, alloc);
                return;
              }
              t.add_val += slen;
              m.flags = sk.get_new_flags();
              m.set_flags_in_skip(sn);
              m.run += t_skip::num_skip_nodes(slen);
            }
          } else {
            break;
          }
        }
        if (t.add_val >= t.add_last) {
          add_eos_at(m, t, alloc);
          return;
        }
        if (has_bit(m.flags, list_bit | pop_bit)) {
          if (has_bit(m.flags, pop_bit)) {
            auto sr = reinterpret_cast<const t_val*>(m.run);
            int off;
            if (!do_find_pop(sr, *t.add_val, &off)) {
              add_pop(m, t, alloc);
              return;
            }
            m.run += off;
          } else {
            auto list = m.run->get_list();
            int lsz = list.get_list_sz();
            int mt = list.offset(*t.add_val);
            if (mt == 0) {
              if (lsz >= t_small_list::max_list) {
                list2pop(m, t, alloc);
                return;
              }
              add_list(m, t, lsz, alloc);
              return;
            }
            m.run += mt;
          }
          t.add_val++;
          m.ref = m.run;
          std::tie(m.run, m.flags) = m.run->get_ptr().get_both();
          m.node_start = m.run;
          m.initial_flags = m.flags;
          m.set_flags_in_ref();
          if (t.add_val >= t.add_last) {
            if (has_bit(m.flags, eos_bit)) {
              if (do_update) m.run->update_data(t.add_ptr, alloc);
              t.cnt = 0;
              t.ret = m.run->get_data_ptr();
              return;
            }
            add_eos_at(m, t, alloc);
            return;
          }
          continue;
        }
        add_branch_at_end(m, t, alloc);
        return;
      }
    }
  }

 private:
  /**
   * @brief Break a HOP node at a mismatch point
   * 
   * ============================================================================
   * BREAK_HOP_AT: ALGORITHM EXPLANATION
   * ============================================================================
   * 
   * This function is called when the input key diverges from the existing
   * HOP sequence. We need to restructure the node array.
   * 
   * CASE 1: Input key ends in the middle of HOP (break_pos >= remaining input)
   * 
   *   Before: [HOP "hello" | flags]
   *   Insert key "hel"
   *   After:  [HOP "hel" | eos | hop | flags] where new HOP is "lo"
   * 
   * CASE 2: Keys diverge in the middle of HOP (most common)
   * 
   *   Before: [HOP "hello" | flags]
   *   Insert key "helps"
   *   break_pos = 3 (diverge at 'l' vs 'p')
   *   
   *   After:  [HOP "hel" | LIST "lp"]
   *              ├─'l'→ [HOP "o" | flags]  (existing suffix)
   *              └─'p'→ [HOP "s" | EOS]    (new key tail)
   */
  static void break_hop_at(modify_data& m, insert_update_ret& t,
                           const t_hop& cur, size_t break_pos, A& alloc) {
    int hop_sz = cur.get_hop_sz();
    size_t remaining = t.add_last - t.add_val;
    t_flag old_cont = cur.get_new_flags();
    auto hop_chars = to_char_static(cur.to_u64());
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t hop_off = m.run - m.node_start;
    size_t nodes_after = orig_len - hop_off - 1;

    if (break_pos >= remaining) {
      // CASE 1: Key ends in middle of HOP - insert EOS
      size_t plen = break_pos, slen = hop_sz - break_pos;
      size_t nodes_before = hop_off;
      t_flag nf = m.initial_flags;
      size_t nsz = nodes_before;
      if (plen > 0) nsz += 1;
      nsz += 1;  // EOS
      if (slen > 0) nsz += 1;
      nsz += nodes_after;

      node_type* nn = node_type::allocate(alloc_size(nsz), alloc);
      node_type* w = nn;
      if (nodes_before > 0) {
        memcpy(w, m.node_start, nodes_before * sizeof(node_type));
        w += nodes_before;
      }

      t_flag after_prefix_or_eos = eos_bit;
      if (slen > 0)
        after_prefix_or_eos |= hop_bit;
      else
        after_prefix_or_eos |= (old_cont & (hop_bit | skip_bit | list_bit | pop_bit));

      if (plen > 0) {
        w->set_hop(t_hop{hop_chars.data(), plen, after_prefix_or_eos});
        w++;
      } else if (nodes_before > 0) {
        nf = (nf & ~hop_bit) | eos_bit;
        if (slen > 0)
          nf |= hop_bit;
        else
          nf |= (old_cont & (hop_bit | skip_bit | list_bit | pop_bit));
      }

      node_type* eos = w;
      w++;
      if (slen > 0) {
        w->set_hop(t_hop{hop_chars.data() + break_pos, slen, old_cont});
        w++;
      }
      memcpy(w, m.run + 1, nodes_after * sizeof(node_type));

      auto ptr = m.ref->get_ptr();
      ptr.set_byte(nf);
      ptr.set_ptr(nn);
      m.ref->set_ptr(ptr);
      
      eos->set_data(t.add_ptr, alloc);
      t.ret = eos->get_data_ptr();
      t.cnt = 1;
      t.tail_ptr = nullptr;
      node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
      return;
    }

    // CASE 2: Keys diverge - create branch
    char ac = t.add_val[break_pos], hc = hop_chars[break_pos];
    bool af = static_cast<uint8_t>(ac) < static_cast<uint8_t>(hc);
    size_t plen = break_pos, slen = hop_sz - break_pos - 1;
    size_t nodes_before = hop_off;
    
    t_flag nf;
    if (plen > 0) {
      nf = (m.initial_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | hop_bit | list_bit;
    } else if (nodes_before > 0 && m.flags_writer.location_type() != flags_loc::type::in_ptr) {
      nf = m.initial_flags;
    } else {
      nf = (m.initial_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
    }
    size_t nsz = nodes_before + 3;
    if (plen > 0) {
      nsz++;
    }

    node_type* nn = node_type::allocate(alloc_size(nsz), alloc);
    node_type* w = nn;
    if (nodes_before > 0) {
      memcpy(w, m.node_start, nodes_before * sizeof(node_type));
      w += nodes_before;
    }
    if (plen > 0) {
      w->set_hop(t_hop{hop_chars.data(), plen, list_bit});
      w++;
    } else if (nodes_before > 0 && m.flags_writer.location_type() != flags_loc::type::in_ptr) {
      size_t flags_pos = m.flags_writer.get_node() - m.node_start;
      if (m.flags_writer.location_type() == flags_loc::type::in_hop) {
        t_hop hop = nn[flags_pos].get_hop();
        t_flag old_flags = hop.get_new_flags();
        t_flag new_hop_flags = (old_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
        auto arr = to_char_static(hop.to_u64());
        arr[t_hop::new_flags_offset] = static_cast<char>(new_hop_flags);
        nn[flags_pos].set_hop(t_hop::from_u64(from_char_static(arr)));
      } else {
        t_skip sk = nn[flags_pos].get_skip();
        t_flag new_skip_flags = (sk.get_new_flags() & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
        nn[flags_pos].set_skip(t_skip{sk.get_skip_len(), static_cast<uint8_t>(new_skip_flags)});
      }
    }
    w->set_list(t_small_list{ac, hc});
    node_type* br = w + 1;
    
    t_flag of = 0;
    node_type* ot = nullptr;
    if (slen > 0) {
      of = hop_bit;
      ot = node_type::allocate(alloc_size(1 + nodes_after), alloc);
      ot[0].set_hop(t_hop{hop_chars.data() + break_pos + 1, slen, old_cont});
      memcpy(ot + 1, m.run + 1, nodes_after * sizeof(node_type));
    } else {
      of = old_cont;
      if (nodes_after > 0) {
        ot = node_type::allocate(alloc_size(nodes_after), alloc);
        memcpy(ot, m.run + 1, nodes_after * sizeof(node_type));
      }
    }
    
    int oi = af ? 1 : 0, ni = af ? 0 : 1;
    {
      auto p = br[oi].get_ptr();
      p.set_byte(of);
      p.set_ptr(ot);
      br[oi].set_ptr(p);
    }
    {
      auto p = m.ref->get_ptr();
      p.set_byte(nf);
      p.set_ptr(nn);
      m.ref->set_ptr(p);
    }
    t.tail_ptr = &br[ni];
    t.add_val += break_pos + 1;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Break a SKIP node at a mismatch point
   * 
   * Similar to break_hop_at but handles SKIP format.
   * SKIP can become HOP if the remaining suffix is short enough (≤6 chars).
   */
  static void break_skip_at(modify_data& m, insert_update_ret& t,
                            size_t break_at, size_t skip_len, char cur_char,
                            node_type* skip_hdr, A& alloc) {
    size_t remaining = t.add_last - t.add_val;
    t_flag old_cont = skip_hdr->get_skip().get_new_flags();
    const char* sd = reinterpret_cast<const char*>(m.run);
    size_t sn = t_skip::num_skip_nodes(skip_len);
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t sho = skip_hdr - m.node_start;
    size_t nodes_after = orig_len - sho - 1 - sn;

    if (break_at >= remaining) {
      // Key ends in middle of skip - insert EOS
      size_t plen = break_at, slen = skip_len - break_at;
      t_flag nf = 0;
      size_t nsz = 0;
      t_flag after_prefix = eos_bit;
      if (slen > 0)
        after_prefix |= (slen <= t_hop::max_hop) ? hop_bit : skip_bit;
      else
        after_prefix |= (old_cont & (hop_bit | skip_bit | list_bit | pop_bit));

      if (plen > 0) {
        if (plen <= t_hop::max_hop) {
          nf = hop_bit;
          nsz = 1;
        } else {
          nf = skip_bit;
          nsz = 1 + t_skip::num_skip_nodes(plen);
        }
      } else {
        nf = eos_bit;
        if (slen > 0)
          nf |= (slen <= t_hop::max_hop) ? hop_bit : skip_bit;
        else
          nf |= (old_cont & (hop_bit | skip_bit | list_bit | pop_bit));
      }
      nsz += 1;
      if (slen > 0) {
        nsz += (slen <= t_hop::max_hop) ? 1 : 1 + t_skip::num_skip_nodes(slen);
      }
      nsz += nodes_after;

      node_type* nn = node_type::allocate(alloc_size(nsz), alloc);
      node_type* w = nn;
      if (plen > 0) {
        if (plen <= t_hop::max_hop) {
          w->set_hop(t_hop{sd, plen, after_prefix});
          w++;
        } else {
          w->set_skip(t_skip{plen, after_prefix});
          w++;
          node_type::skip_copy(w, sd, plen);
          w += t_skip::num_skip_nodes(plen);
        }
      }
      node_type* eos = w;
      w++;
      if (slen > 0) {
        if (slen <= t_hop::max_hop) {
          w->set_hop(t_hop{sd + break_at, slen, old_cont});
          w++;
        } else {
          w->set_skip(t_skip{slen, old_cont});
          w++;
          node_type::skip_copy(w, sd + break_at, slen);
          w += t_skip::num_skip_nodes(slen);
        }
      }
      memcpy(w, m.run + sn, nodes_after * sizeof(node_type));
      
      auto p = m.ref->get_ptr();
      p.set_byte(nf);
      p.set_ptr(nn);
      m.ref->set_ptr(p);
      
      eos->set_data(t.add_ptr, alloc);
      t.ret = eos->get_data_ptr();
      t.cnt = 1;
      t.tail_ptr = nullptr;
      node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
      return;
    }

    // Keys diverge - create branch
    char ac = t.add_val[break_at];
    bool af = static_cast<uint8_t>(ac) < static_cast<uint8_t>(cur_char);
    size_t plen = break_at, slen = skip_len - break_at - 1;
    size_t nodes_before = sho;
    
    t_flag nf;
    size_t nsz = nodes_before + 3;
    if (plen > 0) {
      if (plen <= t_hop::max_hop) {
        nf = (m.initial_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | hop_bit | list_bit;
        nsz++;
      } else {
        nf = (m.initial_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | skip_bit | list_bit;
        nsz += 1 + t_skip::num_skip_nodes(plen);
      }
    } else if (nodes_before > 0 && m.flags_writer.location_type() != flags_loc::type::in_ptr) {
      nf = m.initial_flags;
    } else {
      nf = (m.initial_flags & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
    }

    node_type* nn = node_type::allocate(alloc_size(nsz), alloc);
    node_type* w = nn;
    if (nodes_before > 0) {
      memcpy(w, m.node_start, nodes_before * sizeof(node_type));
      w += nodes_before;
    }
    if (plen > 0) {
      if (plen <= t_hop::max_hop) {
        w->set_hop(t_hop{sd, plen, list_bit});
        w++;
      } else {
        w->set_skip(t_skip{plen, list_bit});
        w++;
        node_type::skip_copy(w, sd, plen);
        w += t_skip::num_skip_nodes(plen);
      }
    } else if (nodes_before > 0 && m.flags_writer.location_type() != flags_loc::type::in_ptr) {
      size_t flags_pos = m.flags_writer.get_node() - m.node_start;
      if (m.flags_writer.location_type() == flags_loc::type::in_hop) {
        t_hop hop = nn[flags_pos].get_hop();
        t_flag new_hop_flags = (hop.get_new_flags() & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
        auto arr = to_char_static(hop.to_u64());
        arr[t_hop::new_flags_offset] = static_cast<char>(new_hop_flags);
        nn[flags_pos].set_hop(t_hop::from_u64(from_char_static(arr)));
      } else {
        t_skip sk = nn[flags_pos].get_skip();
        t_flag new_skip_flags = (sk.get_new_flags() & ~(hop_bit | skip_bit | list_bit | pop_bit)) | list_bit;
        nn[flags_pos].set_skip(t_skip{sk.get_skip_len(), static_cast<uint8_t>(new_skip_flags)});
      }
    }
    w->set_list(t_small_list{ac, cur_char});
    node_type* br = w + 1;
    
    t_flag of = 0;
    node_type* ot = nullptr;
    if (slen > 0) {
      of = (slen <= t_hop::max_hop) ? hop_bit : skip_bit;
      size_t suff_n = (slen <= t_hop::max_hop) ? 1 : 1 + t_skip::num_skip_nodes(slen);
      ot = node_type::allocate(alloc_size(suff_n + nodes_after), alloc);
      if (slen <= t_hop::max_hop) {
        ot[0].set_hop(t_hop{sd + break_at + 1, slen, old_cont});
      } else {
        ot[0].set_skip(t_skip{slen, old_cont});
        node_type::skip_copy(ot + 1, sd + break_at + 1, slen);
      }
      memcpy(ot + suff_n, m.run + sn, nodes_after * sizeof(node_type));
    } else {
      of = old_cont;
      if (nodes_after > 0) {
        ot = node_type::allocate(alloc_size(nodes_after), alloc);
        memcpy(ot, m.run + sn, nodes_after * sizeof(node_type));
      }
    }
    
    int oi = af ? 1 : 0, ni = af ? 0 : 1;
    {
      auto p = br[oi].get_ptr();
      p.set_byte(of);
      p.set_ptr(ot);
      br[oi].set_ptr(p);
    }
    {
      auto p = m.ref->get_ptr();
      p.set_byte(nf);
      p.set_ptr(nn);
      m.ref->set_ptr(p);
    }
    t.tail_ptr = &br[ni];
    t.add_val += break_at + 1;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Convert a LIST to POP when it becomes full (7 -> 8 children)
   */
  static void list2pop(modify_data& m, insert_update_ret& t, A& alloc) {
    t_val np[4] = {0, 0, 0, 0};
    auto list = m.run->get_list();
    int lsz = list.get_list_sz();
    for (int i = 0; i < lsz; ++i) set_pop_for(list.get_list_at(i), np);
    set_pop_for(*t.add_val, np);
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t lo = m.run - m.node_start;
    size_t nlen = orig_len + 3 + 1;
    t_flag nf = (m.initial_flags & ~list_bit) | pop_bit;
    node_type* nn = node_type::allocate(alloc_size(nlen), alloc);
    memcpy(nn, m.node_start, lo * sizeof(node_type));
    nn[lo].set_pop(np[0]);
    nn[lo + 1].set_pop(np[1]);
    nn[lo + 2].set_pop(np[2]);
    nn[lo + 3].set_pop(np[3]);
    int no;
    do_find_pop(np, *t.add_val, &no);
    auto pchars = get_pop_chars(np);
    int obi = 0;
    for (size_t i = 0; i < pchars.size(); ++i) {
      if (static_cast<int>(i) == no - 4)
        t.tail_ptr = nn + lo + 4 + i;
      else {
        nn[lo + 4 + i].set_raw(m.node_start[lo + 1 + obi].raw());
        obi++;
      }
    }
    
    if (m.flags_writer.location_type() != flags_loc::type::in_ptr) {
      size_t flags_pos = m.flags_writer.get_node() - m.node_start;
      flags_loc new_floc(m.flags_writer.location_type(), nn + flags_pos);
      new_floc.set((new_floc.get() & ~list_bit) | pop_bit);
    }
    
    auto p = m.ref->get_ptr();
    p.set_byte(nf);
    p.set_ptr(nn);
    m.ref->set_ptr(p);
    
    t.add_val++;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Add a new entry to a LIST (when LIST has room)
   */
  static void add_list(modify_data& m, insert_update_ret& t, int lsz, A& alloc) {
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t lo = m.run - m.node_start;
    size_t nlen = orig_len + 1;
    node_type* nn = node_type::allocate(alloc_size(nlen), alloc);
    memcpy(nn, m.node_start, (lo + 1) * sizeof(node_type));
    auto list = nn[lo].get_list();
    int pos = list.insert(lsz, *t.add_val);
    nn[lo].set_list(list);
    for (int i = 0; i < pos; ++i)
      nn[lo + 1 + i].set_raw(m.node_start[lo + 1 + i].raw());
    t.tail_ptr = nn + lo + 1 + pos;
    for (int i = pos; i < lsz; ++i)
      nn[lo + 2 + i].set_raw(m.node_start[lo + 1 + i].raw());
    
    auto p = m.ref->get_ptr();
    p.set_ptr(nn);
    m.ref->set_ptr(p);
    
    t.add_val++;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Add a new entry to a POP bitmap
   */
  static void add_pop(modify_data& m, insert_update_ret& t, A& alloc) {
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t po = m.run - m.node_start;
    size_t nlen = orig_len + 1;
    node_type* nn = node_type::allocate(alloc_size(nlen), alloc);
    memcpy(nn, m.node_start, po * sizeof(node_type));
    
    t_val pop[4];
    pop[0] = m.node_start[po].get_pop();
    pop[1] = m.node_start[po + 1].get_pop();
    pop[2] = m.node_start[po + 2].get_pop();
    pop[3] = m.node_start[po + 3].get_pop();
    
    auto old_pchars = get_pop_chars(pop);
    
    set_pop_for(*t.add_val, pop);
    nn[po].set_pop(pop[0]);
    nn[po + 1].set_pop(pop[1]);
    nn[po + 2].set_pop(pop[2]);
    nn[po + 3].set_pop(pop[3]);
    
    int no;
    do_find_pop(pop, *t.add_val, &no);
    auto pchars = get_pop_chars(pop);
    
    int oi = 0;
    for (size_t i = 0; i < pchars.size(); ++i) {
      size_t d = po + 4 + i;
      if (static_cast<int>(i) == no - 4) {
        t.tail_ptr = nn + d;
      } else {
        nn[d].set_raw(m.node_start[po + 4 + oi].raw());
        oi++;
      }
    }
    
    auto p = m.ref->get_ptr();
    p.set_ptr(nn);
    m.ref->set_ptr(p);
    
    t.add_val++;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Add an EOS at the current position
   */
  static void add_eos_at(modify_data& m, insert_update_ret& t, A& alloc) {
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t pos = m.run - m.node_start;
    size_t nlen = orig_len + 1;
    t_flag nif = m.initial_flags;
    node_type* nn = node_type::allocate(alloc_size(nlen), alloc);
    memcpy(nn, m.node_start, pos * sizeof(node_type));
    memcpy(nn + pos + 1, m.node_start + pos, (orig_len - pos) * sizeof(node_type));
    
    if (m.flags_writer.location_type() == flags_loc::type::in_ptr) {
      nif |= eos_bit;
    } else {
      size_t flags_node_pos = m.flags_writer.get_node() - m.node_start;
      if (m.flags_writer.location_type() == flags_loc::type::in_hop) {
        t_hop hop = nn[flags_node_pos].get_hop();
        auto arr = to_char_static(hop.to_u64());
        arr[t_hop::new_flags_offset] = static_cast<char>(hop.get_new_flags() | eos_bit);
        nn[flags_node_pos].set_hop(t_hop::from_u64(from_char_static(arr)));
      } else {
        t_skip sk = nn[flags_node_pos].get_skip();
        nn[flags_node_pos].set_skip(t_skip{sk.get_skip_len(), 
            static_cast<uint8_t>(sk.get_new_flags() | eos_bit)});
      }
    }

    nn[pos].set_data(t.add_ptr, alloc);
    
    auto p = m.ref->get_ptr();
    p.set_byte(nif);
    p.set_ptr(nn);
    m.ref->set_ptr(p);
    
    t.ret = nn[pos].get_data_ptr();
    t.cnt = 1;
    t.tail_ptr = nullptr;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }

  /**
   * @brief Add a branch at the end of the current node array
   */
  static void add_branch_at_end(modify_data& m, insert_update_ret& t, A& alloc) {
    size_t remaining = t.add_last - t.add_val;
    auto orig_len = node_array_sz(m.node_start, m.initial_flags);
    size_t pos = m.run - m.node_start;
    bool use_skip = remaining > t_hop::max_hop;
    size_t add_n = use_skip ? (1 + t_skip::num_skip_nodes(remaining) + 1) : 2;
    size_t nlen = orig_len + add_n;
    t_flag nif = m.initial_flags;
    node_type* nn = node_type::allocate(alloc_size(nlen), alloc);
    memcpy(nn, m.node_start, pos * sizeof(node_type));
    memcpy(nn + pos + add_n, m.node_start + pos, (orig_len - pos) * sizeof(node_type));
    
    t_flag hsb = use_skip ? skip_bit : hop_bit;
    
    if (m.flags_writer.location_type() == flags_loc::type::in_ptr) {
      nif |= hsb;
    } else {
      size_t flags_node_pos = m.flags_writer.get_node() - m.node_start;
      if (m.flags_writer.location_type() == flags_loc::type::in_hop) {
        t_hop hop = nn[flags_node_pos].get_hop();
        auto arr = to_char_static(hop.to_u64());
        arr[t_hop::new_flags_offset] = static_cast<char>(hop.get_new_flags() | hsb);
        nn[flags_node_pos].set_hop(t_hop::from_u64(from_char_static(arr)));
      } else {
        t_skip sk = nn[flags_node_pos].get_skip();
        nn[flags_node_pos].set_skip(t_skip{sk.get_skip_len(),
            static_cast<uint8_t>(sk.get_new_flags() | hsb)});
      }
    }

    node_type* w = nn + pos;
    if (use_skip) {
      w->set_skip(t_skip{remaining, eos_bit});
      w++;
      node_type::skip_copy(w, t.add_val, remaining);
      w += t_skip::num_skip_nodes(remaining);
      w->set_data(t.add_ptr, alloc);
      t.ret = w->get_data_ptr();
    } else {
      w->set_hop(t_hop{t.add_val, remaining, eos_bit});
      w++;
      w->set_data(t.add_ptr, alloc);
      t.ret = w->get_data_ptr();
    }
    
    auto p = m.ref->get_ptr();
    p.set_byte(nif);
    p.set_ptr(nn);
    m.ref->set_ptr(p);
    
    t.cnt = 1;
    t.tail_ptr = nullptr;
    node_type::deallocate(m.node_start, alloc_size(orig_len), alloc);
  }
};

}  // namespace gteitelbaum
