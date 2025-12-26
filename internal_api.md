# KTRIE Internal API Reference

This document provides complete API documentation for all internal classes, structures, and functions in the KTRIE library. This is intended for contributors and those who want to understand or extend the implementation.

For user-facing API documentation, see [README.md](./README.md).

## Table of Contents

- [Namespace](#namespace)
- [Core Classes](#core-classes)
  - [ktrie](#ktrie)
  - [ktrie_base](#ktrie_base)
  - [node](#node)
- [Node Type Classes](#node-type-classes)
  - [t_hop](#t_hop)
  - [t_skip](#t_skip)
  - [t_small_list](#t_small_list)
- [Pointer and Data Classes](#pointer-and-data-classes)
  - [dirty_high_pointer](#dirty_high_pointer)
  - [data_ptr](#data_ptr)
  - [flags_location](#flags_location)
- [Helper Classes](#helper-classes)
  - [insert_helper](#insert_helper)
  - [remove_helper](#remove_helper)
  - [nav_helper](#nav_helper)
  - [ktrie_pretty](#ktrie_pretty)
- [Iterator Classes](#iterator-classes)
  - [ktrie_iterator_impl](#ktrie_iterator_impl)
  - [ktrie_reverse_iterator](#ktrie_reverse_iterator)
- [Numeric Conversion](#numeric-conversion)
  - [cvt_numeric](#cvt_numeric)
- [Type Aliases and Enums](#type-aliases-and-enums)
- [Concepts](#concepts)
- [Utility Functions](#utility-functions)

---

## Namespace

All KTRIE components are defined within the `gteitelbaum` namespace.

```cpp
namespace gteitelbaum {
    // All classes, functions, and types
}
```

---

## Core Classes

### ktrie

```cpp
template <class Key, class T, class A = std::allocator<T>>
class ktrie;
```

**File:** `ktrie.h`

**Description:**  
Primary public interface template. Specialized for `std::string`, `char*`, and numeric key types.

#### Template Parameters

| Parameter | Description |
|-----------|-------------|
| `Key` | Key type (`std::string`, `char*`, or numeric type) |
| `T` | Mapped value type |
| `A` | Allocator type (default: `std::allocator<T>`) |

#### Specialization: ktrie\<std::string, T, A\>

**Member Types:**

| Type | Definition |
|------|------------|
| `key_type` | `std::string` |
| `mapped_type` | `T` |
| `value_type` | `std::pair<const key_type, T>` |
| `size_type` | `std::size_t` |
| `difference_type` | `std::ptrdiff_t` |
| `allocator_type` | `A` |
| `reference` | `value_type&` |
| `const_reference` | `const value_type&` |
| `pointer` | `typename std::allocator_traits<A>::pointer` |
| `const_pointer` | `typename std::allocator_traits<A>::const_pointer` |
| `iterator` | `ktrie_iterator_impl<std::string, T, 0, A, false>` |
| `const_iterator` | `ktrie_iterator_impl<std::string, T, 0, A, true>` |
| `reverse_iterator` | `ktrie_reverse_iterator<iterator>` |
| `const_reverse_iterator` | `ktrie_reverse_iterator<const_iterator>` |

**Constructors:**

```cpp
/// @brief Default constructor
ktrie();

/// @brief Construct with allocator
/// @param alloc Allocator instance
explicit ktrie(const A& alloc);

/// @brief Construct from initializer list
/// @param init Initializer list of key-value pairs
/// @param alloc Allocator instance (default constructed)
ktrie(std::initializer_list<value_type> init, const A& alloc = A());

/// @brief Move constructor
ktrie(ktrie&&) = default;

/// @brief Move assignment
ktrie& operator=(ktrie&&) = default;

// Copy construction/assignment deleted
ktrie(const ktrie&) = delete;
ktrie& operator=(const ktrie&) = delete;
```

**Element Access:**

```cpp
/// @brief Access element with bounds checking
/// @param key Key to look up
/// @return Reference to mapped value
/// @throws std::out_of_range if key not found
T& at(const key_type& key);
const T& at(const key_type& key) const;

/// @brief Access or insert element
/// @param key Key to look up or insert
/// @return Reference to mapped value (default constructed if new)
T& operator[](const key_type& key);
```

**Iterators:**

```cpp
iterator begin();
const_iterator begin() const;
const_iterator cbegin() const;
iterator end();
const_iterator end() const;
const_iterator cend() const;
reverse_iterator rbegin();
const_reverse_iterator rbegin() const;
const_reverse_iterator crbegin() const;
reverse_iterator rend();
const_reverse_iterator rend() const;
const_reverse_iterator crend() const;
```

**Capacity:**

```cpp
/// @brief Check if container is empty
/// @return true if size() == 0
[[nodiscard]] bool empty() const noexcept;

/// @brief Get number of elements
/// @return Number of key-value pairs
size_type size() const noexcept;

/// @brief Get maximum possible size
/// @return Theoretical maximum number of elements
size_type max_size() const noexcept;
```

**Modifiers:**

```cpp
/// @brief Remove all elements
void clear() noexcept;

/// @brief Insert element
/// @param value Key-value pair to insert
/// @return Pair of iterator to element and bool (true if inserted)
std::pair<iterator, bool> insert(const value_type& value);
std::pair<iterator, bool> insert(value_type&& value);

/// @brief Insert with hint (hint ignored)
/// @param hint Iterator hint (unused)
/// @param value Value to insert
/// @return Iterator to inserted or existing element
iterator insert(const_iterator hint, const value_type& value);

/// @brief Insert range of elements
/// @param first Begin iterator
/// @param last End iterator
template <class InputIt>
void insert(InputIt first, InputIt last);

/// @brief Insert from initializer list
void insert(std::initializer_list<value_type> ilist);

/// @brief Emplace element
/// @param args Arguments forwarded to value_type constructor
/// @return Pair of iterator and insertion success
template <class... Args>
std::pair<iterator, bool> emplace(Args&&... args);

/// @brief Emplace with hint (hint ignored)
template <class... Args>
iterator emplace_hint(const_iterator hint, Args&&... args);

/// @brief Try to emplace (no overwrite)
/// @param key Key for element
/// @param args Arguments for value construction
/// @return Pair of iterator and success
template <class... Args>
std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args);

/// @brief Insert or update element
/// @param key Key for element
/// @param value Value to insert or assign
/// @return Pair of iterator and bool (true if inserted, false if assigned)
std::pair<iterator, bool> insert_or_assign(const key_type& key, const T& value);

/// @brief Erase element by iterator
/// @param pos Iterator to element
/// @return Iterator to following element
iterator erase(const_iterator pos);

/// @brief Erase range
/// @param first Begin of range
/// @param last End of range
/// @return Iterator to element following last erased
iterator erase(const_iterator first, const_iterator last);

/// @brief Erase element by key
/// @param key Key to erase
/// @return Number of elements erased (0 or 1)
size_type erase(const key_type& key);

/// @brief Swap contents with another ktrie
/// @param other ktrie to swap with
void swap(ktrie& other) noexcept;
```

**Lookup:**

```cpp
/// @brief Count elements with key
/// @param key Key to count
/// @return 0 or 1
size_type count(const key_type& key) const;

/// @brief Find element by key
/// @param key Key to find
/// @return Iterator to element or end()
iterator find(const key_type& key);
const_iterator find(const key_type& key) const;

/// @brief Check if key exists
/// @param key Key to check
/// @return true if key exists
bool contains(const key_type& key) const;

/// @brief Get range of elements matching key
/// @param key Key to match
/// @return Pair of iterators (both equal if not found, or element and next)
std::pair<iterator, iterator> equal_range(const key_type& key);
std::pair<const_iterator, const_iterator> equal_range(const key_type& key) const;

/// @brief Find first element not less than key
/// @param key Key to compare
/// @return Iterator to first element >= key, or end()
iterator lower_bound(const key_type& key);
const_iterator lower_bound(const key_type& key) const;

/// @brief Find first element greater than key
/// @param key Key to compare
/// @return Iterator to first element > key, or end()
iterator upper_bound(const key_type& key);
const_iterator upper_bound(const key_type& key) const;
```

**Debug:**

```cpp
/// @brief Print trie structure for debugging
/// @param only_summary If true, print statistics only
void pretty_print(bool only_summary = false) const;
```

---

#### Specialization: ktrie\<char*, T, A\>

Minimal interface for raw pointer keys.

```cpp
/// @brief Insert with explicit length
/// @param key Pointer to key data
/// @param len Length of key in bytes
/// @param value Value to insert
/// @return Pointer to stored value, or nullptr on failure
const T* insert(const char* key, size_t len, const T& value);

/// @brief Insert or update
const T* insert_or_assign(const char* key, size_t len, const T& value);

/// @brief Find by key
/// @return Pointer to value, or nullptr if not found
const T* find(const char* key, size_t len) const;

/// @brief Erase by key
/// @return Number erased (0 or 1)
size_type erase(const char* key, size_t len);

/// @brief Check if key exists
bool contains(const char* key, size_t len) const;
```

---

#### Specialization: ktrie\<N, T, A\> (numeric)

Full interface for numeric keys (int, int64_t, uint64_t, etc.).

Same interface as `ktrie<std::string, T, A>` but with numeric `key_type`.

---

### ktrie_base

```cpp
template <class T, size_t fixed_len, class A = std::allocator<T>>
class ktrie_base;
```

**File:** `ktrie_base.h`

**Description:**  
Core implementation of trie operations. Used internally by `ktrie` specializations.

#### Template Parameters

| Parameter | Description |
|-----------|-------------|
| `T` | Value type |
| `fixed_len` | Fixed key length in bytes (0 for variable-length strings) |
| `A` | Allocator type |

#### Member Types

| Type | Definition |
|------|------------|
| `node_type` | `node<T, fixed_len, A>` |
| `key_type` | `std::string` |
| `mapped_type` | `T` |
| `value_type` | `std::pair<const key_type, T>` |
| `size_type` | `std::size_t` |
| `allocator_type` | `A` |

#### Internal Methods

```cpp
/// @brief Insert key-value pair
/// @param key Pointer to key bytes
/// @param sz Key length
/// @param value Value to insert
/// @return Pair of (pointer to value, was_inserted)
std::pair<const T*, bool> insert_internal(const char* key, size_t sz, const T& value);

/// @brief Insert or update
/// @return Pair of (pointer to value, was_inserted)
std::pair<const T*, bool> insert_or_assign_internal(const char* key, size_t sz, const T& value);

/// @brief Erase key
/// @return Number erased (0 or 1)
size_type erase_internal(const char* key, size_t sz);

/// @brief Find key
/// @return Pointer to value or nullptr
const T* find_internal(const char* in, size_t sz) const;

/// @brief Count keys
/// @return 0 or 1
size_type count_internal(const char* key, size_t sz) const;

/// @brief Check key existence
bool contains_internal(const char* key, size_t sz) const;

/// @brief Get first key
/// @return Result with key and value, or empty result
typename nav_helper<T, fixed_len, A>::ktrie_result first_internal() const;

/// @brief Get last key
typename nav_helper<T, fixed_len, A>::ktrie_result last_internal() const;

/// @brief Get next key after given key
typename nav_helper<T, fixed_len, A>::ktrie_result 
next_item_internal(const char* key, size_t sz) const;

/// @brief Get previous key before given key
typename nav_helper<T, fixed_len, A>::ktrie_result 
prev_item_internal(const char* key, size_t sz) const;

/// @brief Lower bound
typename nav_helper<T, fixed_len, A>::ktrie_result 
lower_bound_internal(const char* key, size_t sz) const;

/// @brief Upper bound
typename nav_helper<T, fixed_len, A>::ktrie_result 
upper_bound_internal(const char* key, size_t sz) const;
```

#### Private Methods

```cpp
/// @brief Recursively destroy node array and free memory
/// @param start Start of node array
/// @param flags Flags for interpreting array
void destroy_node_array(node_type* start, t_flag flags);
```

---

### node

```cpp
template <class T, size_t fixed_len, class A = std::allocator<T>>
class node;
```

**File:** `ktrie_node.h`

**Description:**  
Fundamental 8-byte storage unit. Can be interpreted as various node types based on context flags.

#### Template Parameters

| Parameter | Description |
|-----------|-------------|
| `T` | Value type |
| `fixed_len` | Fixed key length (0 for strings) |
| `A` | Allocator type |

#### Member Types

```cpp
using node_type = node<T, fixed_len, A>;
using t_ptr = dirty_high_pointer<node_type, t_flag>;
using t_data = data_ptr<T, A>;
```

#### Constructors

```cpp
/// @brief Default constructor (zero-initialized)
node();
```

#### Pointer Accessors

```cpp
/// @brief Get as dirty pointer
/// @return Pointer with embedded flags
t_ptr get_ptr() const;

/// @brief Set as dirty pointer
/// @param c Pointer value with flags
void set_ptr(const t_ptr& c);
```

#### Value Data Accessors

```cpp
/// @brief Store value in node
/// @param c Pointer to value to store
/// @param alloc Allocator for large value allocation
void set_data(const T* c, A& alloc);

/// @brief Update existing value
/// @param c Pointer to new value
/// @param alloc Allocator for memory management
void update_data(const T* c, A& alloc);

/// @brief Get value by copy
/// @return Copy of stored value
T get_value() const;

/// @brief Get pointer to stored value
/// @return Const pointer to value
const T* get_data_ptr() const;

/// @brief Get mutable pointer to stored value
T* get_data_ptr_mutable();
```

#### HOP/SKIP/LIST/POP Accessors

```cpp
/// @brief Get as HOP node
t_hop get_hop() const;
void set_hop(const t_hop& c);

/// @brief Get as SKIP header
t_skip get_skip() const;
void set_skip(const t_skip& c);

/// @brief Get as LIST node
t_small_list get_list() const;
void set_list(const t_small_list& c);

/// @brief Get as POP bitmap value
t_val get_pop() const;
void set_pop(t_val c);
```

#### Raw Access

```cpp
/// @brief Get raw 64-bit value
t_val raw() const;

/// @brief Set raw 64-bit value
void set_raw(t_val v);
```

#### Static Methods

```cpp
/// @brief Allocate node array
/// @param len Number of nodes
/// @param alloc Allocator to use
/// @return Pointer to allocated array
static node_type* allocate(size_t len, A& alloc);

/// @brief Deallocate node array
/// @param array Array to deallocate (may be null)
/// @param len Number of nodes
/// @param alloc Allocator to use
static void deallocate(node_type* array, size_t len, A& alloc);

/// @brief Copy string data into nodes (for SKIP)
/// @param ptr Destination nodes
/// @param src Source string
/// @param sz Number of bytes
static void skip_copy(node_type* ptr, const char* src, size_t sz);
```

---

## Node Type Classes

### t_hop

```cpp
class t_hop;
```

**File:** `ktrie_hop.h`

**Description:**  
Inline storage for 1-6 character sequences within a single 64-bit node.

#### Constants

```cpp
static constexpr size_t max_hop = 6;         ///< Maximum inline characters
static constexpr size_t sz_offset = 7;       ///< Byte offset of length field
static constexpr size_t new_flags_offset = 6; ///< Byte offset of flags field
```

#### Constructors

```cpp
/// @brief Default constructor (empty)
t_hop();

/// @brief Construct from character data
/// @param c Pointer to characters
/// @param len Number of characters (1-6)
/// @param flags Flags for what follows
t_hop(const char* c, size_t len, uint8_t flags);
```

#### Methods

```cpp
/// @brief Get character at position
/// @param pos Position (0-based)
/// @return Character at position
char get_char_at(size_t pos) const;

/// @brief Get flags for following content
/// @return Flag bits
uint8_t get_new_flags() const;

/// @brief Get number of characters
/// @return Length (1-6)
uint8_t get_hop_sz() const;

/// @brief Create suffix HOP from position
/// @param start Starting position
/// @return New HOP with characters from start
t_hop get_suffix(size_t start) const;

/// @brief Convert to string
/// @return String containing characters
std::string to_string() const;

/// @brief Check if matches key prefix
/// @param c Key data
/// @param remaining Remaining key length
/// @return true if matches
bool matches(const char* c, size_t remaining) const;

/// @brief Find first mismatch position
/// @param c Key data
/// @param remaining Remaining key length
/// @return Position of first mismatch
size_t find_mismatch(const char* c, size_t remaining) const;

/// @brief Get raw 64-bit value
uint64_t to_u64() const;

/// @brief Reconstruct from raw value
static t_hop from_u64(uint64_t v);
```

---

### t_skip

```cpp
class t_skip;
```

**File:** `ktrie_skip.h`

**Description:**  
Header for long strings (>6 characters). Character data follows in subsequent nodes.

#### Constants

```cpp
static constexpr int flag_shift = 59;  ///< Bit position of flags
static constexpr uint64_t len_mask = (1ULL << 59) - 1;  ///< Mask for length
```

#### Constructors

```cpp
/// @brief Default constructor
t_skip();

/// @brief Construct with length and flags
/// @param len Number of characters
/// @param flags Flags for what follows
t_skip(uint64_t len, uint8_t flags);
```

#### Methods

```cpp
/// @brief Get flags
uint8_t get_new_flags() const;

/// @brief Get character count
uint64_t get_skip_len() const;

/// @brief Calculate nodes needed for n characters
/// @param n Character count
/// @return Number of 8-byte nodes required
static size_t num_skip_nodes(size_t n);

/// @brief Get raw 64-bit value
uint64_t to_u64() const;

/// @brief Reconstruct from raw value
static t_skip from_u64(uint64_t v);
```

---

### t_small_list

```cpp
class t_small_list;
```

**File:** `ktrie_small_list.h`

**Description:**  
Compact sorted character list for branch points with ≤7 children.

#### Constants

```cpp
static constexpr int max_list = 7;  ///< Maximum children in LIST
```

#### Constructors

```cpp
/// @brief Default constructor (empty)
t_small_list();

/// @brief Construct from raw value
explicit t_small_list(uint64_t x);

/// @brief Construct 2-element list
/// @param c1 First character
/// @param c2 Second character (sorted automatically)
t_small_list(char c1, char c2);
```

#### Methods

```cpp
/// @brief Get number of characters
/// @return Count (0-7)
int get_list_sz() const;

/// @brief Get character at position
/// @param pos Position (0-based)
/// @return Character
uint8_t get_list_at(int pos) const;

/// @brief Set character at position
/// @param pos Position
/// @param c Character
/// @warning Does not maintain sort order
void set_list_at(int pos, char c);

/// @brief Set count
/// @param len New count
void set_list_sz(int len);

/// @brief Convert to string
std::string to_string() const;

/// @brief Find character offset (SWAR)
/// @param c Character to find
/// @return 1-based offset, or 0 if not found
int offset(char c) const;

/// @brief Insert character in sorted order (SWAR)
/// @param len Current list size
/// @param c Character to insert
/// @return Position where inserted
int insert(int len, char c);

/// @brief Get raw 64-bit value
uint64_t to_u64() const;

/// @brief Reconstruct from raw value
static t_small_list from_u64(uint64_t v);
```

---

## Pointer and Data Classes

### dirty_high_pointer

```cpp
template <class T, class U>
class dirty_high_pointer;
```

**File:** `ktrie_dirty_high_pointer.h`

**Description:**  
64-bit pointer with 5 flag bits embedded in high bits. Exploits unused address space bits on 64-bit systems.

#### Template Parameters

| Parameter | Description |
|-----------|-------------|
| `T` | Pointed-to type |
| `U` | Flag byte type (typically `uint8_t`) |

#### Constructors

```cpp
/// @brief Default constructor (null, no flags)
dirty_high_pointer();

/// @brief Construct with pointer and flags
/// @param p Pointer value
/// @param x Flag bits
dirty_high_pointer(T* p, U x);
```

#### Methods

```cpp
/// @brief Get flag byte
/// @return Flag bits from high 5 bits
U get_byte() const;

/// @brief Set flag byte
/// @param x New flag bits
void set_byte(U x);

/// @brief Get pointer
/// @return Pointer with high bits masked off
T* get_ptr() const;

/// @brief Set pointer
/// @param p New pointer value
void set_ptr(T* p);

/// @brief Get both pointer and flags
/// @return Pair of (pointer, flags)
std::pair<T*, U> get_both() const;

/// @brief Get raw 64-bit value
uint64_t to_u64() const;

/// @brief Reconstruct from raw value
static dirty_high_pointer from_u64(uint64_t v);
```

---

### data_ptr

```cpp
template <class T, class A = std::allocator<T>>
class data_ptr;
```

**File:** `ktrie_data_ptr.h`

**Description:**  
Type-erased value storage. Small values (≤8 bytes) stored inline; large values heap-allocated.

#### Specialization: data_ptr\<small_class T, A\>

For types where `sizeof(T) <= sizeof(T*)`:

```cpp
/// @brief Reconstruct from raw uint64_t
static data_ptr make_data(uint64_t val);

/// @brief Convert value to uint64_t
static uint64_t make_val(const T* data);

/// @brief Destroy (no-op for inline)
static void destroy(uint64_t, A&);

/// @brief Get pointer to value
const T* get() const;
```

#### Specialization: data_ptr\<big_class T, A\>

For types where `sizeof(T) > sizeof(T*)`:

```cpp
/// @brief Reconstruct from raw uint64_t (pointer)
static data_ptr make_data(uint64_t val);

/// @brief Convert value pointer to uint64_t
static uint64_t make_val(const T* data);

/// @brief Destroy heap-allocated value
/// @param val Raw uint64_t containing pointer
/// @param alloc Allocator for deallocation
static void destroy(uint64_t val, A& alloc);

/// @brief Get pointer to value
const T* get() const;
```

---

### flags_location

```cpp
template <class T, size_t fixed_len, class A>
class flags_location;
```

**File:** `ktrie_flags_loc.h`

**Description:**  
Tracks where flags are stored during tree modifications. Flags can be in parent pointer, HOP node, or SKIP node.

#### Enums

```cpp
enum class type {
    in_ptr,   ///< Flags in parent pointer's high bits
    in_hop,   ///< Flags in HOP node's new_flags field
    in_skip   ///< Flags in SKIP node's new_flags field
};
```

#### Constructors

```cpp
/// @brief Default constructor
flags_location();

/// @brief Construct with location type and node
flags_location(type t, node_type* n);
```

#### Factory Methods

```cpp
/// @brief Create for pointer location
static flags_location in_ptr(node_type* n);

/// @brief Create for HOP location
static flags_location in_hop(node_type* n);

/// @brief Create for SKIP location
static flags_location in_skip(node_type* n);
```

#### Methods

```cpp
/// @brief Get location type
type location_type() const;

/// @brief Get node pointer
node_type* get_node() const;

/// @brief Check if valid
bool valid() const;

/// @brief Read flags
t_flag get() const;

/// @brief Write flags
void set(t_flag f);

/// @brief Add flag bits
void add_flags(t_flag f);

/// @brief Remove flag bits
void remove_flags(t_flag f);
```

---

## Helper Classes

### insert_helper

```cpp
template <class T, size_t fixed_len, class A>
class insert_helper;
```

**File:** `ktrie_insert_help.h`

**Description:**  
Static helper functions for insert and update operations.

#### Nested Types

```cpp
/// @brief Return value from insert operations
struct insert_update_ret {
    const char* add_val;    ///< Remaining key to add
    const char* add_last;   ///< End of key
    const T* add_ptr;       ///< Value to insert
    node_type* tail_ptr;    ///< Where to attach new tail
    const T* ret;           ///< Pointer to inserted value
    int cnt;                ///< Count change (0 or 1)
    
    size_t get_size() const;  ///< Remaining key length
};

/// @brief Modification context
struct modify_data {
    node_type* ref;           ///< Node with pointer to array
    node_type* node_start;    ///< Start of current array
    node_type* run;           ///< Current position
    t_flag flags;             ///< Current flags
    t_flag initial_flags;     ///< Flags at array start
    flags_loc flags_writer;   ///< Where to write flag updates
};
```

#### Static Methods

```cpp
/// @brief Calculate node array size
/// @param start Array start
/// @param flags Initial flags
/// @return Number of nodes in array
static size_t node_array_sz(const node_type* start, t_flag flags);

/// @brief Create tail nodes for remaining key
/// @param t Insert context (modified in place)
/// @param alloc Allocator
static void make_tail(insert_update_ret& t, A& alloc);

/// @brief Main insert/update loop
/// @param m Modification context
/// @param t Insert context
/// @param do_update If true, update existing values
/// @param alloc Allocator
static void insert_update_loop(modify_data& m, insert_update_ret& t,
                               bool do_update, A& alloc);
```

#### Private Static Methods

```cpp
static void break_hop_at(...);      ///< Split HOP at mismatch
static void break_skip_at(...);     ///< Split SKIP at mismatch
static void list2pop(...);          ///< Convert LIST to POP
static void add_list(...);          ///< Add char to LIST
static void add_pop(...);           ///< Add char to POP
static void add_eos_at(...);        ///< Insert EOS node
static void add_branch_at_end(...); ///< Add branch at path end
```

---

### remove_helper

```cpp
template <class T, size_t fixed_len, class A>
class remove_helper;
```

**File:** `ktrie_remove_help.h`

**Description:**  
Static helper functions for remove operations.

#### Nested Types

```cpp
/// @brief Path entry for backtracking
struct remove_path_entry {
    node_type* ref;           ///< Reference node
    node_type* node_start;    ///< Array start
    t_flag initial_flags;     ///< Initial flags
    int child_index;          ///< Index in branch
    node_type* branch_node;   ///< Branch node pointer
};
```

#### Static Methods

```cpp
/// @brief Main remove loop
/// @param in Key to remove
/// @param sz Key length
/// @param counter Pointer to element counter
/// @param head_ptr Head node
/// @param alloc Allocator
/// @return true if removed
static bool remove_loop(const char* in, size_t sz, size_t* counter,
                        node_type* head_ptr, A& alloc);
```

#### Private Static Methods

```cpp
static bool rebuild_without_eos(...);     ///< Remove EOS from array
static bool remove_child_from_branch(...); ///< Remove child from branch
static bool remove_from_list(...);         ///< Remove from LIST
static bool remove_from_pop(...);          ///< Remove from POP
static bool pop_to_list_on_remove(...);    ///< Convert POP to LIST
static bool remove_last_branch(...);       ///< Handle last child removal
```

---

### nav_helper

```cpp
template <class T, size_t fixed_len, class A>
class nav_helper;
```

**File:** `ktrie_nav.h`

**Description:**  
Static helper functions for navigation (next/previous key operations).

#### Nested Types

```cpp
/// @brief Navigation result
struct ktrie_result {
    std::string key;     ///< Found key bytes
    const T* value;      ///< Pointer to value
    bool exists;         ///< true if found
    
    ktrie_result();
    ktrie_result(std::string k, const T* v);
};

/// @brief Stack frame for backtracking
struct nav_frame {
    node_type* node_start;
    t_flag flags;
    std::string prefix;
    int child_index;
    std::vector<std::pair<char, node_type*>> children;
};
```

#### Static Methods

```cpp
/// @brief Find next key >= or > given key
/// @param in Key bytes
/// @param sz Key length
/// @param or_equal If true, include exact match
/// @param run Root node
/// @param flags Root flags
/// @return Next key result
static ktrie_result find_next_impl_static(const char* in, size_t sz,
                                          bool or_equal, node_type* run,
                                          t_flag flags);

/// @brief Find previous key <= or < given key
static ktrie_result find_prev_impl_static(const char* in, size_t sz,
                                          bool or_equal, node_type* run,
                                          t_flag flags);
```

#### Private Static Methods

```cpp
static ktrie_result get_min_from(...);         ///< Find minimum in subtrie
static ktrie_result get_max_from(...);         ///< Find maximum in subtrie
static ktrie_result get_max_recursive(...);    ///< Recursive max finder
static ktrie_result get_min_from_current(...); ///< Min from current position
```

---

### ktrie_pretty

```cpp
template <class T, size_t fixed_len, class A>
class ktrie_pretty;
```

**File:** `ktrie_pretty.h`

**Description:**  
Debug visualization and statistics utilities.

#### Nested Types

```cpp
/// @brief Trie statistics
struct trie_stats {
    size_t total_uint64s;    ///< Total 64-bit words
    size_t total_arrays;     ///< Number of arrays
    size_t max_depth;        ///< Maximum depth
    size_t hop_count;        ///< HOP node count
    size_t hop_total_len;    ///< Sum of HOP lengths
    size_t skip_count;       ///< SKIP node count
    size_t skip_total_len;   ///< Sum of SKIP lengths
    size_t list_count;       ///< LIST node count
    size_t pop_count;        ///< POP node count
    size_t short_pop_count;  ///< POP with 8-15 children
};

/// @brief Child branch info
struct child_info {
    char label;
    node_type* ptr;
    t_flag flags;
};
```

#### Static Methods

```cpp
/// @brief Print character safely
static void print_char_safe(char c);

/// @brief Format key for display
static std::string format_key(const std::string& key);

/// @brief Print flag names
static void pretty_flags(t_flag f);

/// @brief Count array size
static size_t count_node_array_size(const node_type* start, t_flag flags);

/// @brief Collect statistics recursively
static void collect_stats(const node_type* start, t_flag flags,
                          size_t depth, trie_stats& stats);

/// @brief Print tree structure
static void pretty_print_node(int indent, const node_type* start,
                              t_flag flags, std::string key);
```

---

## Iterator Classes

### ktrie_iterator_impl

```cpp
template <class Key, class T, size_t fixed_len, class A, bool is_const>
class ktrie_iterator_impl;
```

**File:** `ktrie_iter.h`

**Description:**  
Bidirectional iterator for KTRIE. Supports both const and non-const iteration.

#### Template Parameters

| Parameter | Description |
|-----------|-------------|
| `Key` | External key type |
| `T` | Value type |
| `fixed_len` | Fixed key length (0 for strings) |
| `A` | Allocator type |
| `is_const` | true for const_iterator |

#### Member Types

```cpp
using iterator_category = std::bidirectional_iterator_tag;
using difference_type = std::ptrdiff_t;
using value_type = std::pair<const Key, T>;
using pointer = std::conditional_t<is_const, const value_type*, value_type*>;
using reference = std::conditional_t<is_const, const value_type&, value_type&>;
```

#### Constructors

```cpp
/// @brief Default constructor (end iterator)
ktrie_iterator_impl();

/// @brief Construct at specific key
/// @param trie Pointer to trie
/// @param key Current key bytes
/// @param at_end true if at end position
ktrie_iterator_impl(const base_type* trie, const std::string& key, 
                    bool at_end = false);

/// @brief Copy constructor
ktrie_iterator_impl(const ktrie_iterator_impl& other);

/// @brief Copy assignment
ktrie_iterator_impl& operator=(const ktrie_iterator_impl& other);

/// @brief Move constructor/assignment
ktrie_iterator_impl(ktrie_iterator_impl&&) noexcept = default;
ktrie_iterator_impl& operator=(ktrie_iterator_impl&&) noexcept = default;

/// @brief Convert non-const to const iterator
template <bool other_const>
ktrie_iterator_impl(const ktrie_iterator_impl<..., other_const>& other);
```

#### Operators

```cpp
/// @brief Dereference
reference operator*() const;

/// @brief Member access
pointer operator->() const;

/// @brief Pre-increment
ktrie_iterator_impl& operator++();

/// @brief Post-increment
ktrie_iterator_impl operator++(int);

/// @brief Pre-decrement
ktrie_iterator_impl& operator--();

/// @brief Post-decrement
ktrie_iterator_impl operator--(int);

/// @brief Equality
bool operator==(const ktrie_iterator_impl& other) const;
bool operator!=(const ktrie_iterator_impl& other) const;
```

#### Methods

```cpp
/// @brief Get current key as bytes
const std::string& key_bytes() const;

/// @brief Check if at end
bool is_end() const;
```

---

### ktrie_reverse_iterator

```cpp
template <class Iterator>
class ktrie_reverse_iterator;
```

**File:** `ktrie_iter.h`

**Description:**  
Reverse iterator adapter.

#### Member Types

```cpp
using iterator_type = Iterator;
using iterator_category = std::bidirectional_iterator_tag;
using difference_type = typename Iterator::difference_type;
using value_type = typename Iterator::value_type;
using pointer = typename Iterator::pointer;
using reference = typename Iterator::reference;
```

#### Methods

```cpp
/// @brief Get underlying iterator
Iterator base() const;

/// @brief Dereference (element before current)
reference operator*() const;
pointer operator->() const;

/// @brief Increment (moves backward)
ktrie_reverse_iterator& operator++();
ktrie_reverse_iterator operator++(int);

/// @brief Decrement (moves forward)
ktrie_reverse_iterator& operator--();
ktrie_reverse_iterator operator--(int);

bool operator==(const ktrie_reverse_iterator& other) const;
bool operator!=(const ktrie_reverse_iterator& other) const;
```

---

## Numeric Conversion

### cvt_numeric

```cpp
template <class T>
struct cvt_numeric;
```

**File:** `ktrie_num_cvt.h`

**Description:**  
Converts numeric types to byte arrays that sort correctly in lexicographic order.

#### Specialization: cvt_numeric\<unsigned_numeric T\>

For unsigned types (endian conversion only):

```cpp
/// @brief Convert to sortable byte array
static auto bitcvt(T inp);

/// @brief Convert back from bytes
auto uncvt() const;
```

#### Specialization: cvt_numeric\<signed_numeric T\>

For signed types (sign bit flip + endian conversion):

```cpp
/// @brief Transform to sortable representation
static unsign make_sortable(T inp);

/// @brief Reverse transformation
static T unmake_sortable(unsign inp);

/// @brief Convert to sortable byte array
static auto bitcvt(T inp);

/// @brief Convert back from bytes
auto uncvt() const;
```

---

## Type Aliases and Enums

**File:** `ktrie_defines.h`

### Type Aliases

```cpp
using t_flag = uint8_t;           ///< Flag type (5 bits used)
using intptr_type = std::uintptr_t;  ///< Integer type for pointers
```

**File:** `ktrie_node.h`

```cpp
using t_val = std::uint64_t;      ///< Raw 64-bit node value
```

### Enums

```cpp
/// @brief Flag bits for node interpretation
enum flags : t_flag {
    eos_bit  = 1 << 0,  ///< End-of-string (value follows)
    skip_bit = 1 << 1,  ///< SKIP node (long string)
    hop_bit  = 1 << 2,  ///< HOP node (1-6 chars)
    list_bit = 1 << 3,  ///< LIST branch (≤7 children)
    pop_bit  = 1 << 4,  ///< POP branch (8+ children)
};

/// @brief Number of flag bits
enum flag_count { 
    num_bits = 5 
};
```

---

## Concepts

**File:** `ktrie_num_cvt.h`

```cpp
/// @brief Any arithmetic type
template <typename T>
concept numeric = std::is_arithmetic_v<T>;

/// @brief Signed arithmetic types
template <typename T>
concept signed_numeric = std::is_arithmetic_v<T> && std::is_signed_v<T>;

/// @brief Unsigned arithmetic types
template <typename T>
concept unsigned_numeric = std::is_arithmetic_v<T> && std::is_unsigned_v<T>;
```

**File:** `ktrie_data_ptr.h`

```cpp
/// @brief Types small enough for inline storage
template <class T>
concept small_class = sizeof(T) <= sizeof(T*);

/// @brief Types requiring heap allocation
template <class T>
concept big_class = sizeof(T) > sizeof(T*);
```

---

## Utility Functions

**File:** `ktrie_defines.h`

```cpp
/// @brief Convert pointer to integer
template <class T>
intptr_type as_int(const T* p);

/// @brief Convert integer to pointer
template <class T>
T* as_ptr(intptr_type i);

/// @brief Byte swap if little-endian
uint64_t byteswap_if_le(uint64_t x);

/// @brief Convert uint64 to char array (big-endian)
std::array<char, 8> to_char_static(uint64_t x);

/// @brief Convert char array to uint64 (big-endian)
uint64_t from_char_static(std::array<char, 8> from);
uint64_t from_char_static(const char* from, size_t len);

/// @brief Calculate allocation size class
size_t alloc_size(size_t n);

/// @brief Check if any flag bits are set
bool has_bit(t_flag u, t_flag c);
```

**File:** `ktrie_node.h`

```cpp
/// @brief Extract characters from POP bitmap
std::vector<char> get_pop_chars(const t_val* pop);

/// @brief Compare SKIP data with key
bool do_find_skip(const char* search, const char* in, size_t len);

/// @brief Find character in POP bitmap
/// @param search Pointer to 4 POP nodes
/// @param c Character to find
/// @param[out] run_add Offset to child pointer
/// @return true if found
bool do_find_pop(const t_val* search, char c, int* run_add);
```

---

## See Also

- [README.md](./README.md) - Main documentation, quick start, public API
- [Architecture Guide](./architecture.md) - Node layouts, algorithms, state machines
- [Implementation Concepts](./concepts.md) - Dirty pointers, SWAR, numeric conversion
- [Comparison Guide](./comparisons.md) - vs std::map/unordered_map, migration

---

*Document Version: 1.0*  
*Last Updated: December 2025*
