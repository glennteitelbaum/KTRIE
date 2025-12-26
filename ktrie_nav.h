/**
 * @file ktrie_nav.h
 * @brief Navigation helpers for trie traversal (next/prev key operations)
 * 
 * ============================================================================
 * WHY GOTO IS USED (backtrack label)
 * ============================================================================
 * 
 * The find_next/find_prev algorithms use goto for backtracking because:
 * 
 * 1. MULTIPLE ENTRY POINTS: Backtracking triggers from several places:
 *    - Key > HOP/SKIP content
 *    - Character not found in branch  
 *    - Null child pointer
 *    All need the same backtracking logic.
 * 
 * 2. STACK UNWINDING: The backtrack loop pops the stack, trying each
 *    unexplored branch - conceptually similar to exception handling.
 * 
 * @note This is an internal header. Users should include ktrie.h instead.
 */

#pragma once

#include <string>
#include <vector>

#include "ktrie_node.h"

namespace gteitelbaum {

template <class T, size_t fixed_len, class A>
class nav_helper {
 public:
  using node_type = node<T, fixed_len, A>;

  struct ktrie_result {
    std::string key;
    const T* value;
    bool exists;

    ktrie_result() : key{}, value{nullptr}, exists{false} {}
    ktrie_result(std::string k, const T* v)
        : key{std::move(k)}, value{v}, exists{true} {}
  };

 private:
  struct nav_frame {
    node_type* node_start;
    t_flag flags;
    std::string prefix;
    int child_index;
    std::vector<std::pair<char, node_type*>> children;
  };

  static ktrie_result get_min_from(node_type* run, t_flag flags, std::string prefix);
  static ktrie_result get_max_from(node_type* run, t_flag flags, std::string prefix);
  static ktrie_result get_max_recursive(node_type* start, t_flag flags, std::string prefix);
  static ktrie_result get_min_from_current(node_type* run, t_flag flags, std::string prefix);

 public:
  static ktrie_result find_next_impl_static(const char* in, size_t sz,
                                            bool or_equal, node_type* run, t_flag flags);
  static ktrie_result find_prev_impl_static(const char* in, size_t sz,
                                            bool or_equal, node_type* run, t_flag flags);
};

// =============================================================================
// Implementation
// =============================================================================

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::get_min_from(node_type* run, t_flag flags, std::string prefix) {
  if (!run) return {};

  for (;;) {
    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(flags, eos_bit)) {
        return {prefix, run->get_data_ptr()};
      }
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          t_hop hop = run->get_hop();
          prefix += hop.to_string();
          flags = hop.get_new_flags();
          run++;
        } else {
          t_skip sk = run->get_skip();
          size_t slen = sk.get_skip_len();
          const char* sd = reinterpret_cast<const char*>(run + 1);
          prefix += std::string(sd, slen);
          flags = sk.get_new_flags();
          run += 1 + t_skip::num_skip_nodes(slen);
        }
      } else {
        break;
      }
    }

    if (!has_bit(flags, list_bit | pop_bit)) return {};

    char first_char;
    node_type* child_node;
    if (has_bit(flags, list_bit)) {
      auto list = run->get_list();
      first_char = static_cast<char>(list.get_list_at(0));
      child_node = run + 1;
    } else {
      auto pop = reinterpret_cast<const t_val*>(run);
      auto chars = get_pop_chars(pop);
      first_char = chars[0];
      child_node = run + 4;
    }

    prefix += first_char;
    auto [next_run, next_flags] = child_node->get_ptr().get_both();
    if (!next_run) return {};
    run = next_run;
    flags = next_flags;
  }
}

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::get_max_from(node_type* run, t_flag flags, std::string prefix) {
  if (!run) return {};

  for (;;) {
    while (has_bit(flags, hop_bit | skip_bit)) {
      if (has_bit(flags, hop_bit)) {
        t_hop hop = run->get_hop();
        prefix += hop.to_string();
        flags = hop.get_new_flags();
        run++;
      } else {
        t_skip sk = run->get_skip();
        size_t slen = sk.get_skip_len();
        const char* sd = reinterpret_cast<const char*>(run + 1);
        prefix += std::string(sd, slen);
        flags = sk.get_new_flags();
        run += 1 + t_skip::num_skip_nodes(slen);
      }
    }

    if (has_bit(flags, list_bit | pop_bit)) {
      char last_char;
      node_type* child_node;

      if (has_bit(flags, list_bit)) {
        auto list = run->get_list();
        int lsz = list.get_list_sz();
        last_char = static_cast<char>(list.get_list_at(lsz - 1));
        child_node = run + lsz;
      } else {
        auto pop = reinterpret_cast<const t_val*>(run);
        auto chars = get_pop_chars(pop);
        last_char = chars.back();
        child_node = run + 4 + static_cast<int>(chars.size()) - 1;
      }

      prefix += last_char;
      auto [next_run, next_flags] = child_node->get_ptr().get_both();
      if (next_run) {
        run = next_run;
        flags = next_flags;
        continue;
      }
    }
    break;
  }

  return get_max_recursive(run, flags, prefix);
}

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::get_max_recursive(node_type* start, t_flag flags, std::string prefix) {
  if (!start) return {};

  node_type* run = start;
  const T* eos_value = nullptr;
  std::string eos_key;

  while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
    if (has_bit(flags, eos_bit)) {
      eos_value = run->get_data_ptr();
      eos_key = prefix;
      run++;
    }
    if (has_bit(flags, hop_bit | skip_bit)) {
      if (has_bit(flags, hop_bit)) {
        t_hop hop = run->get_hop();
        prefix += hop.to_string();
        flags = hop.get_new_flags();
        run++;
      } else {
        t_skip sk = run->get_skip();
        size_t slen = sk.get_skip_len();
        const char* sd = reinterpret_cast<const char*>(run + 1);
        prefix += std::string(sd, slen);
        flags = sk.get_new_flags();
        run += 1 + t_skip::num_skip_nodes(slen);
      }
    } else {
      break;
    }
  }

  if (!has_bit(flags, list_bit | pop_bit)) {
    if (eos_value) return {eos_key, eos_value};
    return {};
  }

  std::vector<std::pair<char, node_type*>> children;
  if (has_bit(flags, list_bit)) {
    auto list = run->get_list();
    int lsz = list.get_list_sz();
    for (int i = 0; i < lsz; ++i) {
      children.push_back({static_cast<char>(list.get_list_at(i)), run + 1 + i});
    }
  } else {
    auto pop = reinterpret_cast<const t_val*>(run);
    auto chars = get_pop_chars(pop);
    for (size_t i = 0; i < chars.size(); ++i) {
      children.push_back({chars[i], run + 4 + i});
    }
  }

  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    auto [c, child_ptr] = *it;
    auto [child_run, child_flags] = child_ptr->get_ptr().get_both();
    if (child_run) {
      auto result = get_max_recursive(child_run, child_flags, prefix + c);
      if (result.exists) return result;
    }
  }

  if (eos_value) return {eos_key, eos_value};
  return {};
}

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::get_min_from_current(node_type* run, t_flag flags, std::string prefix) {
  while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
    if (has_bit(flags, eos_bit)) {
      return {prefix, run->get_data_ptr()};
    }
    if (has_bit(flags, hop_bit | skip_bit)) {
      if (has_bit(flags, hop_bit)) {
        t_hop hop = run->get_hop();
        prefix += hop.to_string();
        flags = hop.get_new_flags();
        run++;
      } else {
        t_skip sk = run->get_skip();
        size_t slen = sk.get_skip_len();
        const char* sd = reinterpret_cast<const char*>(run + 1);
        prefix += std::string(sd, slen);
        flags = sk.get_new_flags();
        run += 1 + t_skip::num_skip_nodes(slen);
      }
    } else {
      break;
    }
  }

  if (has_bit(flags, list_bit | pop_bit)) {
    char first_char;
    node_type* child_node;
    if (has_bit(flags, list_bit)) {
      auto list = run->get_list();
      first_char = static_cast<char>(list.get_list_at(0));
      child_node = run + 1;
    } else {
      auto pop = reinterpret_cast<const t_val*>(run);
      auto chars = get_pop_chars(pop);
      first_char = chars[0];
      child_node = run + 4;
    }
    auto [child_run, child_flags] = child_node->get_ptr().get_both();
    if (child_run) {
      return get_min_from(child_run, child_flags, prefix + first_char);
    }
  }
  return {};
}

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::find_next_impl_static(const char* in, size_t sz,
                                                   bool or_equal, node_type* run, t_flag flags) {
  const char* key = in;
  const char* const last = in + sz;
  std::string prefix;
  std::vector<nav_frame> stack;

  for (;;) {
    nav_frame frame;
    frame.node_start = run;
    frame.flags = flags;
    frame.prefix = prefix;
    frame.child_index = -1;

    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(flags, eos_bit)) {
        if (key >= last) {
          if (or_equal) {
            return {prefix, run->get_data_ptr()};
          }
          run++;
          flags = flags & ~eos_bit;
          if (has_bit(flags, hop_bit | skip_bit)) {
            if (has_bit(flags, hop_bit)) {
              t_hop hop = run->get_hop();
              prefix += hop.to_string();
              flags = hop.get_new_flags();
              run++;
            } else {
              t_skip sk = run->get_skip();
              size_t slen = sk.get_skip_len();
              const char* sd = reinterpret_cast<const char*>(run + 1);
              prefix += std::string(sd, slen);
              flags = sk.get_new_flags();
              run += 1 + t_skip::num_skip_nodes(slen);
            }
            return get_min_from_current(run, flags, prefix);
          }
          if (has_bit(flags, list_bit | pop_bit)) {
            return get_min_from_current(run, flags, prefix);
          }
          goto backtrack;
        }
        run++;
      }
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          t_hop hop = run->get_hop();
          size_t hsz = hop.get_hop_sz();
          size_t rem = last - key;
          auto hop_str = hop.to_string();

          for (size_t i = 0; i < hsz; ++i) {
            if (i >= rem) {
              prefix += hop_str.substr(i);
              flags = hop.get_new_flags();
              run++;
              return get_min_from_current(run, flags, prefix);
            }
            uint8_t kc = static_cast<uint8_t>(key[i]);
            uint8_t hc = static_cast<uint8_t>(hop_str[i]);
            if (kc < hc) {
              prefix += hop_str.substr(i);
              flags = hop.get_new_flags();
              run++;
              return get_min_from_current(run, flags, prefix);
            }
            if (kc > hc) {
              goto backtrack;
            }
            prefix += hop_str[i];
          }
          key += hsz;
          flags = hop.get_new_flags();
          run++;
        } else {
          t_skip sk = run->get_skip();
          size_t slen = sk.get_skip_len();
          run++;
          const char* sd = reinterpret_cast<const char*>(run);
          size_t rem = last - key;

          for (size_t i = 0; i < slen; ++i) {
            if (i >= rem) {
              prefix += std::string(sd + i, slen - i);
              flags = sk.get_new_flags();
              run += t_skip::num_skip_nodes(slen);
              return get_min_from_current(run, flags, prefix);
            }
            uint8_t kc = static_cast<uint8_t>(key[i]);
            uint8_t sc = static_cast<uint8_t>(sd[i]);
            if (kc < sc) {
              prefix += std::string(sd + i, slen - i);
              flags = sk.get_new_flags();
              run += t_skip::num_skip_nodes(slen);
              return get_min_from_current(run, flags, prefix);
            }
            if (kc > sc) {
              goto backtrack;
            }
            prefix += sd[i];
          }
          key += slen;
          flags = sk.get_new_flags();
          run += t_skip::num_skip_nodes(slen);
        }
      } else {
        break;
      }
    }

    if (key >= last) {
      if (has_bit(flags, list_bit | pop_bit)) {
        return get_min_from_current(run, flags, prefix);
      }
      goto backtrack;
    }

    if (!has_bit(flags, list_bit | pop_bit)) {
      goto backtrack;
    }

    {
      char c = *key;
      uint8_t uc = static_cast<uint8_t>(c);
      std::vector<std::pair<char, node_type*>> children;

      if (has_bit(flags, list_bit)) {
        auto list = run->get_list();
        int lsz = list.get_list_sz();
        for (int i = 0; i < lsz; ++i) {
          children.push_back({static_cast<char>(list.get_list_at(i)), run + 1 + i});
        }
      } else {
        auto pop = reinterpret_cast<const t_val*>(run);
        auto chars = get_pop_chars(pop);
        for (size_t i = 0; i < chars.size(); ++i) {
          children.push_back({chars[i], run + 4 + i});
        }
      }

      frame.prefix = prefix;
      frame.children = children;

      int found_idx = -1;
      int next_greater_idx = -1;
      for (size_t i = 0; i < children.size(); ++i) {
        uint8_t cc = static_cast<uint8_t>(children[i].first);
        if (cc == uc) {
          found_idx = static_cast<int>(i);
          break;
        }
        if (cc > uc && next_greater_idx < 0) {
          next_greater_idx = static_cast<int>(i);
          break;
        }
      }

      if (found_idx >= 0) {
        frame.child_index = found_idx;
        stack.push_back(frame);

        auto [child_run, child_flags] = children[found_idx].second->get_ptr().get_both();
        if (!child_run) goto backtrack;

        prefix += c;
        key++;
        run = child_run;
        flags = child_flags;
        continue;
      }

      if (next_greater_idx >= 0) {
        char gc = children[next_greater_idx].first;
        auto [child_run, child_flags] = children[next_greater_idx].second->get_ptr().get_both();
        if (child_run) {
          return get_min_from(child_run, child_flags, prefix + gc);
        }
      }

      goto backtrack;
    }

    continue;

  //============================================================================
  // BACKTRACK: Pop stack and try next siblings
  //============================================================================
  backtrack:
    while (!stack.empty()) {
      auto& f = stack.back();
      for (size_t i = f.child_index + 1; i < f.children.size(); ++i) {
        auto [child_run, child_flags] = f.children[i].second->get_ptr().get_both();
        if (child_run) {
          return get_min_from(child_run, child_flags, f.prefix + f.children[i].first);
        }
      }
      stack.pop_back();
    }
    return {};
  }
}

template <class T, size_t fixed_len, class A>
typename nav_helper<T, fixed_len, A>::ktrie_result
nav_helper<T, fixed_len, A>::find_prev_impl_static(const char* in, size_t sz,
                                                   bool or_equal, node_type* run, t_flag flags) {
  const char* key = in;
  const char* const last = in + sz;
  std::string prefix;
  std::vector<nav_frame> stack;

  ktrie_result last_less;
  for (;;) {
    nav_frame frame{};
    frame.node_start = run;
    frame.flags = flags;
    frame.prefix = prefix;
    frame.child_index = -1;

    while (has_bit(flags, eos_bit | hop_bit | skip_bit)) {
      if (has_bit(flags, eos_bit)) {
        if (key >= last) {
          if (or_equal) {
            return {prefix, run->get_data_ptr()};
          }
          return last_less;
        }
        last_less = {prefix, run->get_data_ptr()};
        run++;
      }
      if (has_bit(flags, hop_bit | skip_bit)) {
        if (has_bit(flags, hop_bit)) {
          t_hop hop = run->get_hop();
          size_t hsz = hop.get_hop_sz();
          size_t rem = last - key;
          auto hop_str = hop.to_string();

          for (size_t i = 0; i < hsz; ++i) {
            if (i >= rem) {
              return last_less;
            }
            uint8_t kc = static_cast<uint8_t>(key[i]);
            uint8_t hc = static_cast<uint8_t>(hop_str[i]);
            if (kc < hc) {
              return last_less;
            }
            if (kc > hc) {
              prefix += hop_str.substr(i);
              flags = hop.get_new_flags();
              run++;
              return get_max_recursive(run, flags, prefix);
            }
            prefix += hop_str[i];
          }
          key += hsz;
          flags = hop.get_new_flags();
          run++;
        } else {
          t_skip sk = run->get_skip();
          size_t slen = sk.get_skip_len();
          run++;
          const char* sd = reinterpret_cast<const char*>(run);
          size_t rem = last - key;

          for (size_t i = 0; i < slen; ++i) {
            if (i >= rem) {
              return last_less;
            }
            uint8_t kc = static_cast<uint8_t>(key[i]);
            uint8_t sc = static_cast<uint8_t>(sd[i]);
            if (kc < sc) {
              return last_less;
            }
            if (kc > sc) {
              prefix += std::string(sd + i, slen - i);
              flags = sk.get_new_flags();
              run += t_skip::num_skip_nodes(slen);
              return get_max_recursive(run, flags, prefix);
            }
            prefix += sd[i];
          }
          key += slen;
          flags = sk.get_new_flags();
          run += t_skip::num_skip_nodes(slen);
        }
      } else {
        break;
      }
    }

    if (key >= last) {
      return last_less;
    }

    if (!has_bit(flags, list_bit | pop_bit)) {
      return last_less;
    }

    {
      char c = *key;
      uint8_t uc = static_cast<uint8_t>(c);
      std::vector<std::pair<char, node_type*>> children;

      if (has_bit(flags, list_bit)) {
        auto list = run->get_list();
        int lsz = list.get_list_sz();
        for (int i = 0; i < lsz; ++i) {
          children.push_back({static_cast<char>(list.get_list_at(i)), run + 1 + i});
        }
      } else {
        auto pop = reinterpret_cast<const t_val*>(run);
        auto chars = get_pop_chars(pop);
        for (size_t i = 0; i < chars.size(); ++i) {
          children.push_back({chars[i], run + 4 + i});
        }
      }

      frame.prefix = prefix;
      frame.children = children;

      int found_idx = -1;
      int last_less_idx = -1;
      for (size_t i = 0; i < children.size(); ++i) {
        uint8_t cc = static_cast<uint8_t>(children[i].first);
        if (cc == uc) {
          found_idx = static_cast<int>(i);
          break;
        }
        if (cc < uc) {
          last_less_idx = static_cast<int>(i);
        } else {
          break;
        }
      }

      if (found_idx >= 0) {
        frame.child_index = found_idx;
        stack.push_back(frame);

        auto [child_run, child_flags] = children[found_idx].second->get_ptr().get_both();
        if (!child_run) {
          goto backtrack_prev;
        }

        prefix += c;
        key++;
        run = child_run;
        flags = child_flags;
        continue;
      }

      if (last_less_idx >= 0) {
        char lc = children[last_less_idx].first;
        auto [child_run, child_flags] = children[last_less_idx].second->get_ptr().get_both();
        if (child_run) {
          return get_max_recursive(child_run, child_flags, prefix + lc);
        }
      }

      return last_less;
    }

    continue;

  //============================================================================
  // BACKTRACK_PREV: Try smaller siblings in reverse order
  //============================================================================
  backtrack_prev:
    while (!stack.empty()) {
      auto& f = stack.back();
      for (int i = f.child_index - 1; i >= 0; --i) {
        auto [child_run, child_flags] = f.children[i].second->get_ptr().get_both();
        if (child_run) {
          return get_max_recursive(child_run, child_flags, f.prefix + f.children[i].first);
        }
      }
      if (has_bit(f.flags, eos_bit)) {
        node_type* n = f.node_start;
        return {f.prefix, n->get_data_ptr()};
      }
      stack.pop_back();
    }
    return last_less;
  }
}

}  // namespace gteitelbaum
