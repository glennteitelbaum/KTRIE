/**
 * @file ktrie_pretty.h
 * @brief Debug visualization and statistics for KTRIE
 * 
 * This file provides utilities for debugging and analyzing trie structure:
 * - Pretty-printing the tree structure
 * - Collecting memory and node statistics
 * - Visualizing node types and their contents
 * 
 * These functions are primarily intended for development and debugging.
 * They can help understand:
 * - How keys are distributed in the trie
 * - Memory usage patterns
 * - Effectiveness of path compression (HOP/SKIP nodes)
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ktrie_node.h"

namespace gteitelbaum {

/**
 * @class ktrie_pretty
 * @brief Static utility functions for trie visualization and statistics
 */
template <class T, size_t fixed_len, class A>
class ktrie_pretty {
 public:
  using node_type = node<T, fixed_len, A>;

  /**
   * @brief Print a character safely (escape non-printable)
   * @param c Character to print
   */
  static void print_char_safe(char c);

  /**
   * @brief Format a key for display
   * @param key Raw key bytes
   * @return Human-readable key representation
   * 
   * For string keys: quoted string
   * For numeric keys: decoded integer value
   */
  static std::string format_key(const std::string& key);

  /**
   * @brief Helper to print a single flag
   * @param f Current flags
   * @param b Bit to check
   * @param l Label to print
   * @param s Whether separator needed
   * @return true if bit was set
   */
  static bool pretty_flg(t_flag f, t_flag b, const char* l, bool s);

  /**
   * @brief Print all set flags
   * @param f Flags to print
   */
  static void pretty_flags(t_flag f);

  /**
   * @brief Information about a child branch
   */
  struct child_info {
    char label;        ///< Branch character
    node_type* ptr;    ///< Pointer to child
    t_flag flags;      ///< Child flags

    /**
     * @brief Construct child info
     * @param l Branch character
     * @param p Pointer to child
     * @param f Child flags
     */
    child_info(char l, node_type* p, t_flag f) : label(l), ptr(p), flags(f) {}
  };

  /**
   * @struct trie_stats
   * @brief Statistics about trie structure and memory usage
   */
  struct trie_stats {
    size_t total_uint64s = 0;    ///< Total 64-bit words allocated
    size_t total_arrays = 0;     ///< Number of node arrays
    size_t max_depth = 0;        ///< Maximum tree depth
    
    size_t hop_count = 0;        ///< Number of HOP nodes
    size_t hop_total_len = 0;    ///< Sum of all HOP lengths
    size_t skip_count = 0;       ///< Number of SKIP nodes
    size_t skip_total_len = 0;   ///< Sum of all SKIP lengths
    size_t list_count = 0;       ///< Number of LIST nodes
    size_t pop_count = 0;        ///< Number of POP nodes
    size_t short_pop_count = 0;  ///< POP nodes with 8-15 children
  };

  /**
   * @brief Count number of nodes in a node array
   * @param start Start of node array
   * @param flags Initial flags
   * @return Number of nodes in array
   */
  static size_t count_node_array_size(const node_type* start, t_flag flags);

  /**
   * @brief Recursively collect statistics about the trie
   * @param start Current node array
   * @param flags Current flags
   * @param depth Current depth
   * @param stats Statistics accumulator
   */
  static void collect_stats(const node_type* start, t_flag flags, 
                           size_t depth, trie_stats& stats);

  /**
   * @brief Print tree structure recursively
   * @param indent Current indentation level
   * @param start Current node array
   * @param flags Current flags
   * @param key Key prefix accumulated so far
   */
  static void pretty_print_node(int indent, const node_type* start,
                                t_flag flags, std::string key);
};

// =============================================================================
// ktrie_pretty - Inline Definitions
// =============================================================================

template <class T, size_t fixed_len, class A>
void ktrie_pretty<T, fixed_len, A>::print_char_safe(char c) {
  if (fixed_len == 0 && c >= 32 && c < 127)
    std::cout << c;
  else
    std::cout << "\\x" << std::hex << std::setfill('0') << std::setw(2)
              << (static_cast<unsigned>(c) & 0xFF) << std::dec;
}

template <class T, size_t fixed_len, class A>
std::string ktrie_pretty<T, fixed_len, A>::format_key(const std::string& key) {
  if constexpr (fixed_len == 0) {
    return "'" + key + "'";
  } else {
    // Numeric key: interpret as big-endian number
    std::ostringstream oss;
    if (key.size() == fixed_len) {
      if constexpr (fixed_len == 1) {
        oss << static_cast<int>(static_cast<uint8_t>(key[0]));
      } else if constexpr (fixed_len == 2) {
        uint16_t val = 0;
        for (size_t i = 0; i < 2; ++i)
          val = (val << 8) | static_cast<uint8_t>(key[i]);
        oss << val;
      } else if constexpr (fixed_len == 4) {
        uint32_t val = 0;
        for (size_t i = 0; i < 4; ++i)
          val = (val << 8) | static_cast<uint8_t>(key[i]);
        // Undo signed transformation
        oss << static_cast<int32_t>(val ^ 0x80000000u);
      } else if constexpr (fixed_len == 8) {
        uint64_t val = 0;
        for (size_t i = 0; i < 8; ++i)
          val = (val << 8) | static_cast<uint8_t>(key[i]);
        oss << static_cast<int64_t>(val ^ 0x8000000000000000ULL);
      } else {
        // Unknown size: show hex
        oss << "0x";
        for (char c : key)
          oss << std::hex << std::setfill('0') << std::setw(2)
              << (static_cast<unsigned>(c) & 0xFF);
      }
    } else {
      // Partial key
      oss << "(partial: ";
      for (char c : key)
        oss << std::hex << std::setfill('0') << std::setw(2)
            << (static_cast<unsigned>(c) & 0xFF);
      oss << ")";
    }
    return oss.str();
  }
}

template <class T, size_t fixed_len, class A>
bool ktrie_pretty<T, fixed_len, A>::pretty_flg(t_flag f, t_flag b, 
                                                const char* l, bool s) {
  if (!has_bit(f, b)) return false;
  if (s) std::cout << " | ";
  std::cout << l;
  return true;
}

template <class T, size_t fixed_len, class A>
void ktrie_pretty<T, fixed_len, A>::pretty_flags(t_flag f) {
  bool s = false;
  s |= pretty_flg(f, eos_bit, "EOS", s);
  s |= pretty_flg(f, skip_bit, "SKIP", s);
  s |= pretty_flg(f, hop_bit, "HOP", s);
  s |= pretty_flg(f, list_bit, "LIST", s);
  s |= pretty_flg(f, pop_bit, "POP", s);
  if (!s) std::cout << "(none)";
}

template <class T, size_t fixed_len, class A>
size_t ktrie_pretty<T, fixed_len, A>::count_node_array_size(
    const node_type* start, t_flag flags) {
  const node_type* ptr = start;
  
  if constexpr (fixed_len > 0) {
    // Numeric keys: simpler structure
    if (has_bit(flags, hop_bit)) {
      t_hop hop = ptr->get_hop();
      flags = hop.get_new_flags();
      ptr++;
    } else if (has_bit(flags, skip_bit)) {
      t_skip sk = ptr->get_skip();
      auto slen = sk.get_skip_len();
      flags = sk.get_new_flags();
      ptr++;
      ptr += t_skip::num_skip_nodes(slen);
    }
    
    if (has_bit(flags, eos_bit)) {
      ptr++;
    }
  } else {
    // String keys: may have multiple EOS/HOP/SKIP
    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(flags, eos_bit)) {
        ptr++;
      }
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          t_hop hop = ptr->get_hop();
          flags = hop.get_new_flags();
          ptr++;
        } else {
          t_skip sk = ptr->get_skip();
          auto slen = sk.get_skip_len();
          flags = sk.get_new_flags();
          ptr++;
          ptr += t_skip::num_skip_nodes(slen);
        }
      } else {
        break;
      }
    }
  }

  if (has_bit(flags, list_bit)) {
    auto list = ptr->get_list();
    int lsz = list.get_list_sz();
    ptr += 1 + lsz;
  } else if (has_bit(flags, pop_bit)) {
    auto pop = reinterpret_cast<const t_val*>(ptr);
    auto chars = get_pop_chars(pop);
    ptr += 4 + chars.size();
  }
  
  return ptr - start;
}

template <class T, size_t fixed_len, class A>
void ktrie_pretty<T, fixed_len, A>::collect_stats(
    const node_type* start, t_flag flags, size_t depth, trie_stats& stats) {
  if (!start) return;
  
  stats.total_uint64s += count_node_array_size(start, flags);
  stats.total_arrays++;
  stats.max_depth = std::max(stats.max_depth, depth + 1);
  
  const node_type* ptr = start;

  if constexpr (fixed_len > 0) {
    if (has_bit(flags, hop_bit)) {
      t_hop hop = ptr->get_hop();
      stats.hop_count++;
      stats.hop_total_len += hop.get_hop_sz();
      flags = hop.get_new_flags();
      ptr++;
    } else if (has_bit(flags, skip_bit)) {
      t_skip sk = ptr->get_skip();
      auto slen = sk.get_skip_len();
      stats.skip_count++;
      stats.skip_total_len += slen;
      flags = sk.get_new_flags();
      ptr++;
      ptr += t_skip::num_skip_nodes(slen);
    }
    
    if (has_bit(flags, eos_bit)) {
      ptr++;
    }
  } else {
    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(flags, eos_bit)) {
        ptr++;
      }
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          t_hop hop = ptr->get_hop();
          stats.hop_count++;
          stats.hop_total_len += hop.get_hop_sz();
          flags = hop.get_new_flags();
          ptr++;
        } else {
          t_skip sk = ptr->get_skip();
          auto slen = sk.get_skip_len();
          stats.skip_count++;
          stats.skip_total_len += slen;
          flags = sk.get_new_flags();
          ptr++;
          ptr += t_skip::num_skip_nodes(slen);
        }
      } else {
        break;
      }
    }
  }

  if (has_bit(flags, list_bit)) {
    stats.list_count++;
    auto list = ptr->get_list();
    int lsz = list.get_list_sz();
    ptr++;
    for (int i = 0; i < lsz; ++i) {
      auto [cp, cf] = ptr->get_ptr().get_both();
      if (cp) {
        collect_stats(cp, cf, depth + 1, stats);
      }
      ptr++;
    }
  } else if (has_bit(flags, pop_bit)) {
    stats.pop_count++;
    auto pop = reinterpret_cast<const t_val*>(ptr);
    auto chars = get_pop_chars(pop);
    if (chars.size() >= 8 && chars.size() <= 15) {
      stats.short_pop_count++;
    }
    ptr += 4;
    for (size_t i = 0; i < chars.size(); ++i) {
      auto [cp, cf] = ptr->get_ptr().get_both();
      if (cp) {
        collect_stats(cp, cf, depth + 1, stats);
      }
      ptr++;
    }
  }
}

template <class T, size_t fixed_len, class A>
void ktrie_pretty<T, fixed_len, A>::pretty_print_node(
    int indent, const node_type* start, t_flag flags, std::string key) {
  std::string pad(indent, ' ');
  const node_type* ptr = start;
  std::cout << pad << "Node [flags: ";
  pretty_flags(flags);
  std::cout << "]" << std::endl;

  while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
    if (has_bit(flags, eos_bit)) {
      std::cout << pad << "  EOS: " << format_key(key) << " = "
                << ptr->get_value() << std::endl;
      ptr++;
    }
    if (has_bit(flags, hop_bit | skip_bit)) {
      if (has_bit(flags, hop_bit)) {
        t_hop hop = ptr->get_hop();
        flags = hop.get_new_flags();
        std::cout << pad << "  HOP[" << static_cast<int>(hop.get_hop_sz()) << " ";
        pretty_flags(flags);
        std::cout << "]: '";
        auto hs = hop.to_string();
        for (char c : hs) print_char_safe(c);
        std::cout << "'" << std::endl;
        key += hs;
        ptr++;
      } else {
        t_skip sk = ptr->get_skip();
        auto slen = sk.get_skip_len();
        flags = sk.get_new_flags();
        ptr++;
        std::cout << pad << "  SKIP[" << slen << " ";
        pretty_flags(flags);
        std::cout << "]: '";
        const char* sd = reinterpret_cast<const char*>(ptr);
        for (size_t i = 0; i < slen; ++i) print_char_safe(sd[i]);
        key += std::string(sd, slen);
        std::cout << "'" << std::endl;
        ptr += t_skip::num_skip_nodes(slen);
      }
    } else {
      break;
    }
  }

  std::vector<child_info> children;
  if (has_bit(flags, list_bit)) {
    auto list = ptr->get_list();
    int lsz = list.get_list_sz();
    std::cout << pad << "  LIST[" << lsz << "]: '";
    for (int i = 0; i < lsz; ++i) print_char_safe(list.get_list_at(i));
    std::cout << "'" << std::endl;
    ptr++;
    for (int i = 0; i < lsz; ++i) {
      char c = static_cast<char>(list.get_list_at(i));
      auto [cp, cf] = ptr->get_ptr().get_both();
      children.push_back({c, cp, cf});
      ptr++;
    }
  } else if (has_bit(flags, pop_bit)) {
    auto pop = reinterpret_cast<const t_val*>(ptr);
    auto chars = get_pop_chars(pop);
    std::cout << pad << "  POP[" << chars.size() << "]: '";
    for (char c : chars) print_char_safe(c);
    std::cout << "'" << std::endl;
    ptr += 4;
    for (size_t i = 0; i < chars.size(); ++i) {
      auto [cp, cf] = ptr->get_ptr().get_both();
      children.push_back({chars[i], cp, cf});
      ptr++;
    }
  }

  for (const auto& ch : children) {
    std::cout << pad << "  -> '";
    print_char_safe(ch.label);
    std::cout << "':" << std::endl;
    if (ch.ptr)
      pretty_print_node(indent + 4, ch.ptr, ch.flags, key + ch.label);
    else
      std::cout << pad << "    (null)" << std::endl;
  }
}

}  // namespace gteitelbaum
