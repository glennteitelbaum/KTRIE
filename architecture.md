# KTRIE Architecture Document

This document provides a detailed technical explanation of KTRIE's internal architecture, including node layouts, state machines, algorithms, and the transformations that occur during insert, delete, and search operations.

## Table of Contents

1. [Node Layout and Structure](#node-layout-and-structure)
2. [Flag System and State Machine](#flag-system-and-state-machine)
3. [Node Types Deep Dive](#node-types-deep-dive)
4. [Path Compression](#path-compression)
5. [Example: Storing "hello"](#example-storing-hello)
6. [Insert Algorithm](#insert-algorithm)
7. [Delete Algorithm](#delete-algorithm)
8. [Search Algorithm](#search-algorithm)
9. [Traversal and Iteration](#traversal-and-iteration)
10. [Numeric Key Optimization](#numeric-key-optimization)

---

## Node Layout and Structure

### Universal Node Format

Every KTRIE node is exactly **64 bits (8 bytes)**. The interpretation depends on context (flags from parent pointer):

```
┌────────────────────────────────────────────────────────────────┐
│                      64 bits (8 bytes)                         │
├────────────────────────────────────────────────────────────────┤
│  Interpreted based on flags as one of:                         │
│                                                                │
│  POINTER:  [flags:5][ptr:59]                                   │
│  HOP:      [char:8][char:8][char:8][char:8][char:8][char:8]    │
│            [new_flags:8][length:8]                             │
│  SKIP:     [new_flags:5][length:59]                            │
│  LIST:     [c0:8][c1:8][c2:8][c3:8][c4:8][c5:8][c6:8][count:8] │
│  POP:      [64 bits of 256-bit bitmap]                         │
│  DATA:     [value bits] or [pointer to value]                  │
└────────────────────────────────────────────────────────────────┘
```

### Detailed Node Layouts

#### POINTER Node (Dirty High Pointer)

```
Bit:  63    59 58                                              0
      ├──────┼────────────────────────────────────────────────────┤
      │flags │              pointer address                       │
      ├──────┼────────────────────────────────────────────────────┤
      │5 bits│                   59 bits                          │
      ├──────┼────────────────────────────────────────────────────┤

Flags (5 bits):
  bit 0: EOS   - End of string, next node is value
  bit 1: SKIP  - Long string follows (>6 chars)
  bit 2: HOP   - Short string follows (1-6 chars)
  bit 3: LIST  - Small branch (≤7 children)
  bit 4: POP   - Large branch (8+ children)
```

#### HOP Node (1-6 Characters Inline)

```
Byte: [0]   [1]   [2]   [3]   [4]   [5]   [6]      [7]
      ├─────┼─────┼─────┼─────┼─────┼─────┼─────────┼───────┤
      │char │char │char │char │char │char │new_flags│length │
      │  0  │  1  │  2  │  3  │  4  │  5  │ (5 used)│ (1-6) │
      ├─────┴─────┴─────┴─────┴─────┴─────┴─────────┴───────┤

Example: HOP for "cat" with LIST following
      ['c'] ['a'] ['t'] [0]  [0]  [0]  [LIST] [3]
      0x63  0x61  0x74  0x00 0x00 0x00  0x08   0x03
```

**Why HOP is limited to 6 characters:**
- Bytes 0-5: Character storage (6 bytes)
- Byte 6: new_flags for what follows (needed to know next node type)
- Byte 7: Length (needed to know how many chars are valid)
- Total: 8 bytes = 64 bits = one node

If we used 7 characters, we'd need 9 bytes (7 chars + flags + length), exceeding node size.

#### SKIP Node (Header + Data Nodes)

```
SKIP Header (1 node):
Bit:  63        59 58                                          0
      ├──────────┼───────────────────────────────────────────────┤
      │new_flags │                  length                       │
      ├──────────┼───────────────────────────────────────────────┤
      │  5 bits  │                 59 bits                       │
      ├──────────┼───────────────────────────────────────────────┤

Following Data Nodes (⌈length/8⌉ nodes):
      [char0-7] [char8-15] [char16-23] ...

Example: SKIP for "international" (13 chars)
  Node 0: [EOS|0...0|13]           ← Header: EOS flag, length=13
  Node 1: "internat"               ← Chars 0-7
  Node 2: "ional\0\0\0"            ← Chars 8-12 + padding
```

**Why SKIP stores flags in the header:**
- The header has 59 unused bits after storing the 5-bit flags
- Length can be up to 2^59 bytes (way more than needed)
- Flags indicate what comes AFTER the skip data (EOS, LIST, etc.)
- Without flags here, we'd need an extra node just for flags

#### LIST Node (≤7 Children)

```
Byte: [0]   [1]   [2]   [3]   [4]   [5]   [6]   [7]
      ├─────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┤
      │char │char │char │char │char │char │char │count│
      │  0  │  1  │  2  │  3  │  4  │  5  │  6  │     │
      └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
      ↑                                           ↑
      Sorted characters (ascending)          Number of children

Following: [count] POINTER nodes, one per character

Example: LIST with children 'a', 'e', 'o'
  Node 0: ['a']['e']['o'][0][0][0][0][3]  ← LIST header
  Node 1: [pointer to 'a' subtrie]
  Node 2: [pointer to 'e' subtrie]
  Node 3: [pointer to 'o' subtrie]
```

#### POP Node (8+ Children, Bitmap)

```
256-bit bitmap stored in 4 consecutive nodes:

  Node 0: bits 0-63    (chars 0x00-0x3F)
  Node 1: bits 64-127  (chars 0x40-0x7F, includes uppercase + some symbols)
  Node 2: bits 128-191 (chars 0x80-0xBF)
  Node 3: bits 192-255 (chars 0xC0-0xFF)

Bit N is set if character N exists as a child.

Following: [popcount(bitmap)] POINTER nodes in sorted char order

Example: POP with children 'A'(65), 'B'(66), 'a'(97), 'b'(98), 'c'(99)
  Node 0: 0x0000000000000000  ← No chars 0-63
  Node 1: 0x0000000000000006  ← Bits 65,66 set (A,B)
  Node 2: 0x000000000000000E  ← Bits 97,98,99 set (a,b,c)
  Node 3: 0x0000000000000000  ← No chars 192-255
  Node 4: [pointer to 'A' subtrie]
  Node 5: [pointer to 'B' subtrie]
  Node 6: [pointer to 'a' subtrie]
  Node 7: [pointer to 'b' subtrie]
  Node 8: [pointer to 'c' subtrie]
```

#### EOS Node (Value Storage)

```
For small values (sizeof(T) ≤ 8):
  [value bits stored directly in 64-bit node]

For large values (sizeof(T) > 8):
  [pointer to heap-allocated value]

Example: EOS with int value 42
  Node: 0x000000000000002A  ← 42 stored directly

Example: EOS with std::string value "hello"
  Node: 0x00007FFF12345678  ← Pointer to heap string
  sizeof(std::string) is typically 32 which is greater than 8

Example: EOS with std::string * value
  Node: 0x00007FFF12345678  ← Pointer to heap string
  Warning: This is efficient for pointers but ownership is not passed
          If the string is modified externally, it is modified in the KTRIE
          If the string is deleted - it is available but corrupt in KTRIE
          Make sure this behaviour is understood and desired
          Otherwise use ktrie<std::string> and dereference before insertion

For any pointer type -- see warning above
```

---

## Flag System and State Machine

### Flag Bit Definitions

```cpp
enum flags : uint8_t {
    eos_bit  = 0b00001,  // bit 0: End-of-string (value follows)
    skip_bit = 0b00010,  // bit 1: SKIP node (long string)
    hop_bit  = 0b00100,  // bit 2: HOP node (short string)
    list_bit = 0b01000,  // bit 3: LIST branch (≤7 children)
    pop_bit  = 0b10000,  // bit 4: POP branch (8+ children)
};
```

### Flag Interaction Rules


Valid flag combinations and their meanings:

| Flags | Meaning  |
|-------|----------|
| EOS | Just a value, no continuation |
│ EOS then HOP[1-6] | Value here, then more chars (HOP), then more  |
│ EOS then SKIP[7+] | Value here, then more chars (SKIP), then more |
│ EOS then LIST[2-7] | Value here, then branch point |
│ EOS then POP[8-64] | Value here, then branch point |
│ HOP[1-6] | Chars inline, then interpret new_flags |
│ SKIP[7+] | Long chars, then interpret new_flags |
│ LIST[2-7] | Branch point |
│ POP[8-64] | Branch point |

Invalid combinations or lengths:

| Flags | Reason |
|-------|--------|
| Both HOP and SKIP | Mutually exclusive: use one or the other |
| Both LIST and POP  | Mutually exclusive: one branch type |
| HOP[7+] | Would be a SKIP |
| SKIP[<7] | Would be a HOP |
| LIST[1]  | Character would be a HOP or at end of prior HOP or LIST |
| LIST[8+] | Would be a POP |
| POP[<8] | Would be a LIST |
| >1 HOP/SKIP in Numerics | Numerics are Fixed Length |

### State Machine for Node Interpretation

```
                    ┌─────────────────────────────────────┐
                    │         Read flags from             │
                    │         parent pointer              │
                    └─────────────────┬───────────────────┘
                                      │
                                      ▼
                    ┌─────────────────────────────────────┐
        ┌───────────│           Has EOS bit?              │───────────┐
        │ Yes       └─────────────────────────────────────┘    No     │
        ▼                                                             │
┌───────────────┐                                                     │
│ Read value    │                                                     │
│ from node     │                                                     │
│ Advance ptr   │                                                     │
│ Clear EOS bit │                                                     │
└───────┬───────┘                                                     │
        │                                                             │
        └──────────────────────────┬──────────────────────────────────┘
                                   ▼
                    ┌─────────────────────────────────────┐
        ┌───────────│        Has HOP or SKIP bit?         │───────────┐
        │ HOP       └─────────────────────────────────────┘    SKIP   │
        ▼                             │ Neither                       ▼
┌───────────────┐                     │                    ┌───────────────┐
│ Read HOP node │                     │                    │ Read SKIP hdr │
│ Extract chars │                     │                    │ Read data nds │
│ Get new_flags │                     │                    │ Get new_flags │
│ Advance ptr   │                     │                    │ Advance ptr   │
│ Use new_flags │                     │                    │ Use new_flags │
└───────┬───────┘                     │                    └───────┬───────┘
        │                             │                            │
        └─────────────────────────────┼────────────────────────────┘
                                      ▼
                    ┌─────────────────────────────────────┐
        ┌───────────│       Has LIST or POP bit?          │───────────┐
        │ LIST      └─────────────────────────────────────┘    POP    │
        ▼                             │ Neither                       ▼
┌───────────────┐                     │                    ┌───────────────┐
│ Read LIST hdr │                     │                    │ Read 4 POP    │
│ Find char idx │                     │                    │ bitmap nodes  │
│ Follow ptr[i] │                     ▼                    │ Find char bit │
└───────────────┘              ┌─────────────┐             │ Follow ptr[i] │
                               │ End of path │             └───────────────┘
                               │ (leaf node) │
                               └─────────────┘
```

### Processing Order

When processing a node array, flags are interpreted in this order:

```
1. EOS (if present)  → Read value, advance, clear EOS from working flags
2. HOP or SKIP       → Read chars, get new_flags, replace working flags
3. LIST or POP       → Branch point, select child to follow

This can repeat via new_flags from HOP/SKIP.
```

---

## Node Types Deep Dive

### EOS (End of String)

**Purpose:** Marks that a complete key ends here; the next node contains the value.

**Structure:**
```
[EOS flag in parent] → [Value Node]
                            │
                            ├─ Small value: bits stored directly
                            └─ Large value: pointer to heap
```

**When created:**
- When a key is inserted and no more characters remain
- Can coexist with other flags (key is prefix of other keys)

### HOP (Short String, 1-6 chars)

**Purpose:** Inline storage for short character sequences, avoiding separate nodes.

**Structure:**
```
[HOP flag in parent] → [HOP Node: chars + new_flags + length]
                              │
                              └─ new_flags determine what comes next
```

**Design decisions:**
- **Why 6 chars max:** Need 1 byte for new_flags, 1 byte for length = 2 bytes overhead
- **Why store flags here:** After reading HOP chars, we need to know what's next
- **Why store length:** HOP might have fewer than 6 chars (e.g., "cat" = 3)

**Comparison operations:**
```cpp
// Check if HOP matches key
bool matches(const char* key, size_t remaining) {
    if (length > remaining) return false;
    // Compare char bytes directly (first 6 bytes of uint64)
    return (data & char_mask) == pack_chars(key, length);
}
```

### SKIP (Long String, >6 chars)

**Purpose:** Store strings longer than 6 characters.

**Structure:**
```
[SKIP flag] → [Header: new_flags + length] → [Data nodes...]
                     │                              │
                     │                              └─ ⌈length/8⌉ nodes
                     └─ 5-bit flags, 59-bit length
```

**Design decisions:**
- **Why separate header:** Can't fit flags + length + >6 chars in one node
- **Why flags in header:** Need to know what follows the character data
- **Data packing:** 8 chars per node, last node may have padding

### LIST (Small Branch, ≤7 children)

**Purpose:** Compact representation of branch points with few children.

**Structure:**
```
[LIST flag] → [LIST Node: sorted chars + count] → [Ptr0][Ptr1]...[PtrN]
                     │                                  │
                     └─ Up to 7 chars in sorted order   └─ N+1 pointers
```

**Why sorted:**
- Enables binary search (though linear is faster for ≤7)
- Enables SWAR parallel lookup
- Required for correct iteration order

**SWAR lookup:**
```cpp
int offset(char c) {
    // Replicate c to all byte positions
    uint64_t search = REPLICATE(c);
    // XOR: matching positions become 0x00
    uint64_t diff = list_data ^ search;
    // Find zero byte position
    int pos = find_zero_byte(diff);
    return (pos < count) ? pos + 1 : 0;
}
```

### POP (Large Branch, 8+ children)

**Purpose:** Efficient representation when many children exist.

**Structure:**
```
[POP flag] → [Bitmap0][Bitmap1][Bitmap2][Bitmap3] → [Ptr0][Ptr1]...
                │                                   │
                └─ 256 bits total (4 × 64)          └─ popcount(bitmap) ptrs
```

**Why 8+ threshold:**
- LIST: 1 + N nodes (header + pointers)
- POP: 4 + N nodes (bitmap + pointers)
- Break-even at N=7: LIST=8 nodes, POP=11 nodes
- At N=8: LIST=9 nodes, POP=12 nodes (LIST still smaller)
- But POP lookup is O(1) via bitmap, LIST is O(n) scan
- Trade-off chosen: switch at 8 for better large-branch performance

**Lookup:**
```cpp
bool find_in_pop(char c, int* offset) {
    uint8_t v = c;
    int word = v / 64;        // Which bitmap word (0-3)
    int bit = v % 64;         // Which bit in word
    
    if (!(bitmap[word] & (1ULL << bit)))
        return false;  // Character not present
    
    // Count bits before this one to find pointer offset
    *offset = 4;  // Skip bitmap nodes
    for (int i = 0; i < word; i++)
        *offset += popcount(bitmap[i]);
    *offset += popcount(bitmap[word] & ((1ULL << bit) - 1));
    
    return true;
}
```

---

## Path Compression

### What is Path Compression?

Path compression collapses chains of single-child nodes into a single node:

```
Without compression:          With compression:
      [h]                          [HOP:"hello"][EOS: value]
       │ 
      [e]
       │
      [l]
       │
      [l]
       │
      [o]
       │
     [EOS: value]

6 nodes                       1 node
```

### Compression Rules


1. Single-child chains become HOP (≤6 chars) or SKIP (>6 chars)

2. HOP/SKIP store "new_flags" to indicate what follows:
   - EOS: Value follows
   - LIST/POP: Branch point follows
   - Another HOP/SKIP: More characters (only for variable-length keys)

3. For fixed-length (numeric) keys:
   - At most ONE HOP or SKIP per node array
   - No chaining of HOP or SKIP→HOP or SKIP
   - EOS only at end (FIXED LENGTH)
   - Simplifies processing

### Compression During Insert

```
Before inserting "help" into trie with "hello":

  [HOP:"hello"] → [EOS:value1]

After:

  [HOP:"hel"] → [LIST:'l','p']
                       │   │
                       │   └→ [HOP:"p"] → [EOS:"help"]
                       │
                       └→ [HOP:"lo"] → [EOS:"hello"]
                     
The shared prefix "hel" is compressed, branch at 'l' vs 'p'.
```

### Decompression Never Happens

KTRIE never "decompresses" - once a branch exists, it stays. This is a design choice:
- Simpler delete algorithm
- Avoids complex rebalancing
- Memory is still efficient due to original compression

---

## Example: Storing "hello"

### Step-by-Step Insert into Empty Trie

```
Initial state:
  head: [ptr:null, flags:0]

Insert "hello" with value 42:

Step 1: Empty trie, create tail
  - Key length 5 ≤ 6, use HOP
  - Create: [HOP:"hello", new_flags:EOS, len:5] → [EOS:42]

Final state:
  head: [ptr:→node0, flags:HOP]
  node0: [HOP:"hello"|EOS|5]
  node1: [value:42]
```

### Memory Layout

```
Address   Content                    Interpretation
────────────────────────────────────────────────────────────────
head:     0x0400_0000_0000_1000     flags=HOP(0x04), ptr=0x1000

0x1000:   0x6865_6C6C_6F01_05       'h','e','l','l','o', EOS, 5
0x1008:   0x0000_0000_0000_002A     value = 42
```

### Adding "help" (Creating a Branch)

```
Current: "hello" → 42

Insert "help" → 99:

Step 1: Navigate, find mismatch at position 3
  - "hel" matches
  - 'l' (existing) vs 'p' (new) mismatch

Step 2: Break HOP at position 3
  - Create: [HOP:"hel"|LIST|3] → [LIST:'l','p'] → [ptr][ptr]
  
Step 3: Create children
  - 'l' child: [HOP:"lo"|EOS|2] → [42]
  - 'p' child: [EOS] → [99]

Final state:
  head: [ptr:→array, flags:HOP]
  
  array[0]: [HOP:"hel"|LIST|3]
  array[1]: [LIST:'l','p'|2]
  array[2]: [ptr:→child_l, flags:HOP]
  array[3]: [ptr:→child_p, flags:EOS]
  
  child_l[0]: [HOP:"lo"|EOS|2]
  child_l[1]: [value:42]
  
  child_p[0]: [value:99]
```

### Visual Representation

```
          head
            │
            ▼
    ┌─────────────────┐
    │ HOP: "hel"      │
    │ new_flags: LIST │
    │ length: 3       │
    └────────┬────────┘
             │
             ▼
    ┌─────────────────┐
    │ LIST: 'l', 'p'  │
    │ count: 2        │
    └──┬─────────┬────┘
       │         │
     ['l']     ['p']
       │         │
       ▼         ▼
    ┌──────┐  ┌──────┐
    │"lo"  │  │EOS:99│
    │EOS:42│  └──┬───┘
    └──┬───┘   
```

---

## Insert Algorithm

### High-Level Pseudocode

```
function INSERT(key, value):
    if trie is empty:
        CREATE_TAIL(head, key, value)
        return
    
    (ptr, flags) = head.get_both()
    current_key_pos = 0
    
    while true:
        // Process EOS
        if flags.has(EOS):
            if current_key_pos == key.length:
                // Key exists, optionally update
                return EXISTING
            advance past EOS node
            flags.clear(EOS)
        
        // Process HOP or SKIP
        if flags.has(HOP):
            hop = read_hop(ptr)
            mismatch = find_mismatch(hop, key, current_key_pos)
            if mismatch < hop.length:
                BREAK_HOP_AT(mismatch)
                return
            current_key_pos += hop.length
            flags = hop.new_flags
            advance ptr
        else if flags.has(SKIP):
            // Similar to HOP but for long strings
            ...
        
        // Check if key exhausted
        if current_key_pos == key.length:
            ADD_EOS_AT_CURRENT_POSITION()
            return
        
        // Process branch (LIST or POP)
        if flags.has(LIST):
            char c = key[current_key_pos]
            offset = list.find(c)
            if offset == 0:
                if list.count >= 7:
                    LIST_TO_POP()
                else:
                    ADD_TO_LIST()
                return
            follow pointer at offset
        else if flags.has(POP):
            // Similar to LIST
            ...
        else:
            // No branch, add remaining key
            ADD_BRANCH_AT_END()
            return
        
        current_key_pos++
        (ptr, flags) = current_ptr.get_both()
```

### State Transitions on Insert

```
┌──────────────────────────────────────────────────────────────────────┐
│                     INSERT STATE TRANSITIONS                         │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  EMPTY TRIE                                                          │
│      │                                                               │
│      ├── key.len == 0 ──────────────→ [EOS:value]                    │
│      ├── key.len ≤ 6 ───────────────→ [HOP:key|EOS] → [value]        │
│      └── key.len > 6 ───────────────→ [SKIP:key|EOS] → [value]       │
│                                                                      │
│  EXISTING HOP "abcdef"                                               │
│      │                                                               │
│      ├── insert "abc" (prefix)                                       │
│      │       → [HOP:"abc"|EOS|LIST] → [value] → [LIST:'d'] → [ptr]   │
│      │                                              └→ [HOP:"ef"...] │
│      │                                                               │
│      ├── insert "abcxyz" (mismatch at 'd' vs 'x')                    │
│      │       → [HOP:"abc"|LIST] → [LIST:'d','x']                     │
│      │              ├─ 'd' → [HOP:"ef"...] (old suffix)              │
│      │              └─ 'x' → [HOP:"yz"|EOS] → [value] (new)          │
│      │                                                               │
│      └── insert "abcdefghi" (extends)                                │
│              → [HOP:"abcdef"|HOP] → [HOP:"ghi"|EOS] → [value]        │
│              (for variable-length keys only)                         │
│                                                                      │
│  EXISTING LIST ['a','m','z']                                         │
│      │                                                               │
│      ├── insert new char 'f' (fits)                                  │
│      │       → [LIST:'a','f','m','z'] (sorted insert)                │
│      │                                                               │
│      └── insert when count=7 (full)                                  │
│              → LIST_TO_POP conversion                                │
│              → [POP:bitmap] → [pointers...]                          │
│                                                                      │
│  EXISTING POP (8+ children)                                          │
│      │                                                               │
│      └── insert new char                                             │
│              → Set bit in bitmap                                     │
│              → Insert pointer in sorted position                     │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### Breaking a HOP

When inserting creates a mismatch within a HOP:

```
HOP "hello" + insert "help":

BEFORE:
  [HOP:"hello"|EOS|5] → [value1]

STEP 1: Find mismatch at position 3 ('l' vs 'p')

STEP 2: Split into prefix + branch + suffixes

  Prefix: "hel" (positions 0-2)
  Branch chars: 'l' (from "hello"), 'p' (from "help")
  Old suffix: "lo" (positions 4-5 of "hello")
  New suffix: "" (nothing after 'p' in "help")

STEP 3: Build new structure

  [HOP:"hel"|LIST|3] → [LIST:'l','p'|2] → [ptr_l][ptr_p]
        │                                    │      │
        └─ prefix                            │      │
                                             │      │
                  ┌──────────────────────────┘      │
                  ▼                                 │
           [HOP:"lo"|EOS|2] → [value1]              │
                  │                                 │
                  └─ old suffix                     │
                                                    │
                  ┌─────────────────────────────────┘
                  ▼
           [EOS] → [value2]
                  │
                  └─ new suffix (empty, just EOS)

AFTER:
  array[0]: HOP "hel" with LIST flag
  array[1]: LIST 'l','p' with count 2
  array[2]: pointer to 'l' child (HOP|flags)
  array[3]: pointer to 'p' child (EOS|flags)
```

### LIST to POP Conversion

When a LIST reaches 7 children and we need to add an 8th:

```
BEFORE (LIST with 7 children):
  [LIST:'a','b','c','d','e','f','g'|7] → [ptr0]...[ptr6]
  Total: 8 nodes

Insert 'h':

AFTER (POP with 8 children):
  [bitmap0][bitmap1][bitmap2][bitmap3] → [ptr0]...[ptr7]
  
  bitmap1 = 0x00000000000000FE  (bits 97-103 set for 'a'-'g')
             0x0000000000000100  (bit 104 set for 'h')
           = 0x00000000000001FE
  
  Total: 12 nodes (4 bitmap + 8 pointers)

Algorithm:
  1. Allocate new array (old_size + 4 - 1 + 1 = old_size + 4)
  2. Copy prefix nodes
  3. Build bitmap from LIST chars + new char
  4. Copy child pointers in sorted order, inserting new one
  5. Update parent pointer flags (LIST→POP)
  6. Deallocate old array
```

### Detailed Insert Pseudocode

```
function BREAK_HOP_AT(modify_data m, insert_ret t, hop, break_pos):
    hop_sz = hop.length
    remaining_key = t.key_remaining
    old_flags = hop.new_flags
    
    // Determine what we're creating
    if break_pos >= remaining_key:
        // Key ends mid-HOP: add EOS in middle
        prefix_len = break_pos
        suffix_len = hop_sz - break_pos
        
        // Create: [prefix?] [EOS] [suffix?] [old_continuation]
        new_array = allocate(...)
        
        if prefix_len > 0:
            new_array[0] = HOP(prefix, EOS | (suffix? HOP : old_flags))
        
        eos_node = new_array[prefix_offset]
        eos_node.set_data(t.value)
        
        if suffix_len > 0:
            new_array[suffix_offset] = HOP(suffix, old_flags)
    else:
        // Mismatch: create branch
        prefix_len = break_pos
        suffix_len = hop_sz - break_pos - 1
        
        add_char = t.key[break_pos]
        hop_char = hop.chars[break_pos]
        
        // Create: [prefix?] [LIST:add_char,hop_char] [ptr][ptr]
        new_array = allocate(...)
        
        if prefix_len > 0:
            new_array[0] = HOP(prefix, LIST)
        
        // Sort characters for LIST
        if add_char < hop_char:
            new_array[list_pos] = LIST(add_char, hop_char)
            new_ptr_idx = 0
            old_ptr_idx = 1
        else:
            new_array[list_pos] = LIST(hop_char, add_char)
            new_ptr_idx = 1
            old_ptr_idx = 0
        
        // Create child for old suffix
        if suffix_len > 0:
            old_child = allocate(suffix_nodes + old_continuation)
            old_child[0] = HOP(suffix, old_flags)
            // copy old continuation
        else:
            old_child = copy(old_continuation)
        
        new_array[ptr_pos + old_ptr_idx].set_ptr(old_child, old_flags)
        
        // New child will be created by make_tail
        t.tail_ptr = &new_array[ptr_pos + new_ptr_idx]
        t.key_pos = break_pos + 1
    
    // Replace old array with new
    m.ref.set_ptr(new_array, new_flags)
    deallocate(m.old_array)
```

---

## Delete Algorithm

### High-Level Pseudocode

```
function DELETE(key):
    if trie is empty:
        return NOT_FOUND
    
    path = []  // Stack of (ref, node_start, flags, child_index, branch_node)
    
    (ptr, flags) = head.get_both()
    ref = head
    current_key_pos = 0
    
    while true:
        // Record path for potential cleanup
        node_start = ptr
        initial_flags = flags
        
        // Process EOS
        if flags.has(EOS):
            if current_key_pos == key.length:
                // Found it! Remove this EOS
                REBUILD_WITHOUT_EOS(ref, node_start, initial_flags, eos_position)
                return SUCCESS
            advance past EOS
            flags.clear(EOS)
        
        // Process HOP/SKIP (must match exactly)
        if flags.has(HOP):
            hop = read_hop(ptr)
            if not hop.matches(key, current_key_pos):
                return NOT_FOUND
            current_key_pos += hop.length
            flags = hop.new_flags
            advance ptr
        // Similar for SKIP
        
        // Process branch
        if not flags.has(LIST | POP):
            return NOT_FOUND
        
        if current_key_pos >= key.length:
            return NOT_FOUND
        
        char c = key[current_key_pos]
        
        if flags.has(LIST):
            offset = list.find(c)
            if offset == 0:
                return NOT_FOUND
            child_index = offset - 1
            branch_node = ptr
            ptr += offset
        else:  // POP
            if not pop.contains(c):
                return NOT_FOUND
            child_index = pop.index_of(c)
            branch_node = ptr
            ptr = pop.pointer_for(c)
        
        // Save path entry for cleanup
        path.push(ref, node_start, initial_flags, child_index, branch_node)
        
        current_key_pos++
        ref = ptr
        (ptr, flags) = ptr.get_both()
        
        if ptr == null:
            return NOT_FOUND
```

### State Transitions on Delete

```
┌─────────────────────────────────────────────────────────────────────┐
│                     DELETE STATE TRANSITIONS                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  DELETE EOS (value node)                                            │
│      │                                                              │
│      ├── Only EOS in array, no other content                        │
│      │       → Delete entire array                                  │
│      │       → Parent's child pointer becomes null                  │
│      │       → May trigger parent cleanup                           │
│      │                                                              │
│      ├── EOS followed by HOP/SKIP (key is prefix)                   │
│      │       → Remove EOS node, shift remaining left                │
│      │       → Update flags to remove EOS bit                       │
│      │                                                              │
│      └── EOS followed by LIST/POP (key is prefix)                   │
│              → Remove EOS node, shift remaining left                │
│              → Update flags to remove EOS bit                       │
│                                                                     │
│  PARENT CLEANUP (after child deletion)                              │
│      │                                                              │
│      ├── LIST with 2+ children remaining                            │
│      │       → Remove char and pointer from LIST                    │
│      │       → Shift remaining chars/pointers                       │
│      │       → Decrement count                                      │
│      │                                                              │
│      ├── LIST with 1 child remaining                                │
│      │       → Remove LIST entirely                                 │
│      │       → If only HOP/SKIP before: may need to remove those    │
│      │       → Recurse to grandparent if empty                      │
│      │                                                              │
│      ├── POP with 8+ children remaining                             │
│      │       → Clear bit in bitmap                                  │
│      │       → Remove pointer, shift remaining                      │
│      │                                                              │
│      └── POP with 7 children remaining                              │
│              → POP_TO_LIST conversion                               │
│              → Rebuild as LIST structure                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### POP to LIST Conversion (on delete)

```
When POP drops to 7 children:

BEFORE (POP with 8 children, deleting 'h'):
  [bitmap0][bitmap1][bitmap2][bitmap3] → [ptr_a]...[ptr_h]
  12 nodes total

AFTER (LIST with 7 children):
  [LIST:'a','b','c','d','e','f','g'|7] → [ptr_a]...[ptr_g]
  8 nodes total

Algorithm:
  1. Extract remaining chars from bitmap (excluding deleted)
  2. Allocate new array (prefix_len + 1 + 7)
  3. Copy prefix nodes
  4. Build LIST header with 7 sorted chars
  5. Copy child pointers (excluding deleted)
  6. Update parent flags (POP→LIST)
  7. Deallocate old array
```

### Rebuild Without EOS

```
function REBUILD_WITHOUT_EOS(ref, node_start, flags, eos_position):
    orig_len = calculate_array_size(node_start, flags)
    
    // Destroy the value
    destroy_value(node_start[eos_position])
    
    // Special case: only EOS in array
    if orig_len == 1:
        deallocate(node_start)
        ref.set_ptr(null, 0)
        decrement_count()
        
        // Check if parent needs cleanup
        if path not empty:
            parent = path.back()
            if parent.branch_node != null:
                REMOVE_CHILD_FROM_BRANCH(path)
        return
    
    // Calculate new layout
    nodes_before = eos_position
    nodes_after = orig_len - eos_position - 1
    
    // Check for truncation case (HOP/SKIP before EOS with nothing after)
    need_truncate = check_truncation_needed(...)
    
    // Build new array without EOS
    new_len = calculate_new_length(...)
    new_array = allocate(new_len)
    
    // Copy nodes before EOS
    copy(new_array, node_start, nodes_before)
    
    // Copy nodes after EOS
    copy(new_array + nodes_before, node_start + eos_position + 1, nodes_after)
    
    // Update flags to remove EOS bit
    new_flags = update_flags_removing_eos(...)
    
    // Replace array
    ref.set_ptr(new_array, new_flags)
    deallocate(node_start)
    decrement_count()
```

---

## Search Algorithm

### High-Level Pseudocode

```
function FIND(key):
    if trie is empty:
        return null
    
    (ptr, flags) = head.get_both()
    current_key_pos = 0
    
    while true:
        // Process EOS
        if flags.has(EOS):
            if current_key_pos == key.length:
                return ptr.get_value()  // Found!
            advance past EOS
            flags.clear(EOS)
        
        // Process HOP
        if flags.has(HOP):
            hop = read_hop(ptr)
            if not hop.matches(key, current_key_pos):
                return null  // Mismatch
            current_key_pos += hop.length
            flags = hop.new_flags
            advance ptr
        
        // Process SKIP
        else if flags.has(SKIP):
            skip = read_skip_header(ptr)
            skip_data = read_skip_data(ptr + 1, skip.length)
            if not matches(skip_data, key + current_key_pos, skip.length):
                return null  // Mismatch
            current_key_pos += skip.length
            flags = skip.new_flags
            advance ptr past skip data
        
        // Check for branch
        if not flags.has(LIST | POP):
            return null  // No branch, key not found
        
        if current_key_pos >= key.length:
            return null  // Key exhausted before finding EOS
        
        char c = key[current_key_pos]
        
        // Process LIST
        if flags.has(LIST):
            list = read_list(ptr)
            offset = list.find(c)  // SWAR lookup
            if offset == 0:
                return null  // Character not in list
            ptr += offset
        
        // Process POP
        else:
            if not pop_contains(ptr, c):
                return null  // Character not in bitmap
            ptr = pop_get_pointer(ptr, c)
        
        // Prefetch next array (optimization)
        prefetch(ptr.get_ptr())
        
        current_key_pos++
        (ptr, flags) = ptr.get_both()
        
        if ptr == null:
            return null  // Null pointer
```

### Search Flowchart

```
                    ┌─────────────┐
                    │   START     │
                    └──────┬──────┘
                           │
                           ▼
                    ┌─────────────┐
                    │ Empty trie? │──Yes──→ return NULL
                    └──────┬──────┘
                           │ No
                           ▼
            ┌──────────────────────────────┐
            │ ptr, flags = head.get_both() │
            │ key_pos = 0                  │
            └──────────────┬───────────────┘
                           │
         ┌─────────────────┴────────────────────────────────────┐
         │                                                      │
         ▼                                                      │
    ┌─────────┐                                                 │
    │Has EOS? │──Yes──→ key_pos==len? ──Yes────→ return VALUE   │
    └────┬────┘              │                                  │
         │ No                │ No                               │
         │                   ▼                                  │
         │            advance, clear EOS                        │
         │                   │                                  │
         └────────┬──────────┘                                  │
                  │                                             │
                  ▼                                             │
    ┌─────────────────────┐                                     │
    │ Has HOP or SKIP?    │                                     │
    └──────────┬──────────┘                                     │
               │                                                │
         ┌─────┴─────┐                                          │
         │ HOP/SKIP  │                                          │
         ▼           ▼                                          │
          ┌─────────┐                                           │
          │ Match?  │                                           │
          └────┬────┘                                           │
               │                                                │
          No ──┴── Yes                                          │
          │         │                                           │
          ▼         │                                           │
         NULL       │                                           │
                    ▼                                           │
         advance key_pos                                        │
         update flags                                           │
              │                                                 │
              ▼                                                 │
    ┌─────────────────────┐                                     │
    │ Has LIST or POP?    │──No──→ return NULL                  │
    └──────────┬──────────┘                                     │
               │ Yes                                            │
               ▼                                                │
    ┌─────────────────────┐                                     │
    │ key_pos >= key.len? │──Yes──→ return NULL                 │
    └──────────┬──────────┘                                     │
               │ No                                             │
               ▼                                                │
    ┌─────────────────────┐                                     │
    │ Find char in branch │                                     │
    └──────────┬──────────┘                                     │
               │                                                │
         ┌─────┴─────┐                                          │
         │           │                                          │
      Found      Not Found                                      │
         │           │                                          │
         │           ▼                                          │
         │       return NULL                                    │
         │                                                      │
         ▼                                                      │
    Follow pointer                                              │
    key_pos++                                                   │
         │                                                      │
         └──────────────────────────────────────────────────────┘
```

---

## Traversal and Iteration

### In-Order Traversal

KTRIE iteration visits keys in lexicographic order. The algorithm:

```
function FIND_FIRST():
    if empty: return END
    
    (ptr, flags) = head.get_both()
    key = ""
    
    // Follow minimum path
    while true:
        // Process EOS first (this is a valid key!)
        if flags.has(EOS):
            return (key, ptr.get_value())
        
        // Process HOP/SKIP
        if flags.has(HOP):
            key += hop.chars
            flags = hop.new_flags
            advance ptr
        else if flags.has(SKIP):
            key += skip.data
            flags = skip.new_flags
            advance ptr past data
        
        // Take first (minimum) child at branch
        if flags.has(LIST):
            first_char = list.chars[0]  // Minimum
            key += first_char
            ptr = list.pointers[0]
        else if flags.has(POP):
            first_char = pop.first_set_bit()  // Minimum
            key += first_char
            ptr = pop.pointer_for(first_char)
        else:
            break  // No branch, something wrong
        
        (ptr, flags) = ptr.get_both()
```

### Find Next Key

```
function FIND_NEXT(current_key):
    // Strategy: Navigate to current_key, then find next
    
    path = []  // Stack of (node_info, children, current_child_index)
    
    // Navigate to current_key, recording path
    navigate_and_record_path(current_key, path)
    
    // Try to go deeper from current position
    if can_go_deeper():
        return find_minimum_from_here()
    
    // Backtrack: find ancestor with unexplored siblings
    while path not empty:
        frame = path.pop()
        
        // Try next sibling
        for i = frame.child_index + 1 to frame.children.count:
            child = frame.children[i]
            if child.ptr != null:
                return find_minimum_from(frame.prefix + child.char, child)
    
    return END  // No more keys


function FIND_MINIMUM_FROM(prefix, node):
    (ptr, flags) = node
    key = prefix
    
    while true:
        if flags.has(EOS):
            return (key, ptr.get_value())
        
        // Process HOP/SKIP
        if flags.has(HOP):
            key += hop.chars
            flags = hop.new_flags
            advance
        else if flags.has(SKIP):
            key += skip.data
            flags = skip.new_flags
            advance
        
        // Take minimum child
        if flags.has(LIST):
            key += list.chars[0]
            ptr = list.pointers[0]
        else if flags.has(POP):
            key += pop.minimum_char()
            ptr = pop.pointer_for_minimum()
        else:
            break
        
        (ptr, flags) = ptr.get_both()
    
    return END
```

### Find Previous Key

```
function FIND_PREVIOUS(current_key):
    path = []
    last_eos = null  // Track last EOS seen (greatest key < current)
    
    // Navigate toward current_key
    (ptr, flags) = head.get_both()
    key_prefix = ""
    key_pos = 0
    
    while true:
        // Track EOS (potential previous key)
        if flags.has(EOS):
            if key_pos < current_key.length:
                last_eos = (key_prefix, ptr)
            advance past EOS
        
        // Process HOP/SKIP
        // Compare character by character
        for each char c in hop/skip:
            if key_pos >= current_key.length:
                // current_key is shorter, previous is last_eos
                return last_eos
            
            if c > current_key[key_pos]:
                // Trie path is greater, previous is last_eos
                return last_eos
            
            if c < current_key[key_pos]:
                // Trie path is less, take maximum from here
                return find_maximum_from(key_prefix + remaining_chars, ...)
            
            key_prefix += c
            key_pos++
        
        // At branch
        if at LIST/POP:
            if key_pos >= current_key.length:
                return last_eos
            
            target_char = current_key[key_pos]
            
            // Find greatest char less than target
            prev_child = find_greatest_less_than(branch, target_char)
            if prev_child exists:
                return find_maximum_from(key_prefix + prev_child.char, prev_child)
            
            // No smaller sibling, follow target if exists
            if branch.contains(target_char):
                path.push(...)
                follow target_char
            else:
                return last_eos
    
    // Backtrack similar to FIND_NEXT but in reverse
```

### Iteration State Machine

```
Iterator states:

┌──────────────────────────────────────────────────┐
│                                                  │
│   ┌─────────┐                                    │
│   │  BEGIN  │                                    │
│   └────┬────┘                                    │
│        │                                         │
│        ▼ find_first()                            │
│   ┌─────────┐     ┌─────────┐                    │
│   │ AT_KEY  │───> │   END   │  (no more keys)    │
│   └────┬────┘     └─────────┘                    │
│        │                 ▲                       │
│        │ ++              │                       │
│        ▼                 │                       │
│   ┌─────────┐            │                       │
│   │FIND_NEXT│────────────┘  (no successor)       │
│   └────┬────┘                                    │
│        │                                         │
│        │ found successor                         │
│        ▼                                         │
│   ┌─────────┐                                    │
│   │ AT_KEY  │  (loop back for next ++)           │
│   └─────────┘                                    │
│                                                  │
└──────────────────────────────────────────────────┘
```

---

## Numeric Key Optimization

### Why Numeric Keys Are Special

For fixed-length numeric keys (int, int64_t, etc.), KTRIE applies simplifications:

1. **No EOS before end:** EOS only appears after exactly `sizeof(Key)` bytes
2. **At most one HOP/SKIP per array:** No chaining HOP→HOP
3. **Simpler state machine:** Fewer cases to handle

### Numeric Key Node Array Structure

```
For a numeric key (e.g., int32_t = 4 bytes):

SIMPLE CASE (no shared prefix with others):
  [HOP:4_bytes|EOS|4] → [value]
  
WITH BRANCH (shared prefix):
  [HOP:2_bytes|LIST|2] → [LIST:'a','b'] → [ptr][ptr]
                              │             │     │
                              └─ After 2 bytes, branch on byte 3
                              
                              Each child has remaining 1 byte:
                              [HOP:1_byte|EOS|1] → [value]
```

### State Machine for Numeric Keys

```
NUMERIC KEY PROCESSING (simplified):

                    ┌─────────────────┐
                    │ Read flags      │
                    │ from parent     │
                    └────────┬────────┘
                             │
            ┌────────────────┼──────────────┐
            │                │              │
            ▼                ▼              ▼
       ┌─────────┐     ┌─────────┐     ┌─────────┐
       │   HOP   │     │  SKIP   │     │  (none) │
       └────┬────┘     └────┬────┘     └────┬────┘
            │               │               │
            ▼               ▼               │
       Match chars      Match chars         │
       Get new_flags    Get new_flags       │
            │                │              │
            └────────────────┼──────────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │ At end of key?  │
                    └────────┬────────┘
                             │
                   ┌─────────┴─────────┐
                   │                   │
                   ▼ Yes               ▼ No
            ┌─────────────┐     ┌─────────────┐
            │ Has EOS?    │     │ Has branch? │
            └──────┬──────┘     └──────┬──────┘
                   │                   │
              Yes──┴──No          Yes──┴──No
               │      │            │      │
               ▼      ▼            ▼      ▼
            FOUND   ERROR       Follow  ERROR
                              child ptr
                                 │
                                 └──→ (recurse)

Note: No "EOS then continue" case for numeric keys!
      EOS only appears at key_length bytes.
```

### Insert Comparison: String vs Numeric

```
STRING KEY INSERT (variable length):
  - May encounter EOS mid-traversal (prefix exists as key)
  - May chain HOP→HOP for long extensions
  - EOS can appear anywhere
  
  Example: Insert "hello" into trie with "helloworld"
  
  BEFORE: [HOP:"hellow"|HOP] → [HOP:"orld"|EOS] → [value1]
  AFTER:  [HOP:"hello"|EOS|HOP] → [value2] → [HOP:"world"|EOS] → [value1]
                      ↑
                      EOS inserted mid-path!


NUMERIC KEY INSERT (fixed length):
  - No mid-path EOS (all keys same length)
  - At most one HOP or SKIP
  - Simpler split logic
  
  Example: Insert key 0x12345678 into trie with 0x12340000
  
  BEFORE: [HOP:"\x12\x34\x00\x00"|EOS|4] → [value1]
  
  Split at byte 2 (mismatch: 0x00 vs 0x56):
  
  AFTER:  [HOP:"\x12\x34"|LIST|2] → [LIST:'\x00','\x56']
               ├─ '\x00' → [HOP:"\x00\x00"|EOS|2] → [value1]
               └─ '\x56' → [HOP:"\x78"|EOS|2] → [value2]
  
  Note: Child still has HOP (not HOP→HOP chain).
        Each array has at most one HOP.
```

### Optimized Numeric Insert Loop

```
function INSERT_NUMERIC(key_bytes, value):
    // Simplified loop: no EOS checks until end
    
    while key_pos < key_length:
        if flags.has(HOP):
            hop = read_hop(ptr)
            mismatch = find_mismatch(hop, key_bytes, key_pos)
            if mismatch < hop.length:
                BREAK_HOP_AT(mismatch)  // Creates LIST
                return
            key_pos += hop.length
            flags = hop.new_flags
            advance ptr
        
        else if flags.has(SKIP):
            // Similar to HOP
            ...
        
        // Now check for EOS (only valid at key_length)
        if flags.has(EOS):
            if key_pos == key_length:
                // Key exists
                return EXISTING
            // Error: EOS before end of fixed-length key
            ERROR("corrupted trie")
        
        // Must have branch
        if not flags.has(LIST | POP):
            // End of path before key_length: add remaining
            ADD_HOP_OR_SKIP(remaining_bytes)
            return
        
        // Follow branch
        char c = key_bytes[key_pos]
        ... (standard branch handling)
        
        key_pos++
    
    // Key exhausted: should find EOS
    if flags.has(EOS):
        return EXISTING
    else:
        ADD_EOS()
        return INSERTED
```

---

## Summary: Complete Algorithm Reference

### Insert Decision Tree

```
INSERT(key, value):
│
├── Trie empty?
│   └── Yes: CREATE_TAIL(key, value)
│
├── Navigate trie, at each node array:
│   │
│   ├── Has EOS and key exhausted?
│   │   └── Yes: Return EXISTING (optionally update)
│   │
│   ├── Has HOP/SKIP?
│   │   ├── Mismatch found?
│   │   │   └── Yes: BREAK_HOP/SKIP → Creates LIST
│   │   └── No: Advance through chars
│   │
│   ├── Key exhausted (no EOS)?
│   │   └── ADD_EOS_AT_CURRENT_POSITION
│   │
│   ├── Has LIST?
│   │   ├── Char exists?
│   │   │   └── Yes: Follow pointer
│   │   ├── LIST full (7)?
│   │   │   └── Yes: LIST_TO_POP, add char
│   │   └── No: ADD_TO_LIST
│   │
│   ├── Has POP?
│   │   ├── Char exists?
│   │   │   └── Yes: Follow pointer
│   │   └── No: ADD_TO_POP
│   │
│   └── No branch?
│       └── ADD_BRANCH_AT_END (HOP/SKIP + EOS)
│
└── CREATE_TAIL for remaining key
```

### Delete Decision Tree

```
DELETE(key):
│
├── Trie empty?
│   └── Yes: Return NOT_FOUND
│
├── Navigate trie (recording path), at each array:
│   │
│   ├── Has EOS and key exhausted?
│   │   └── Yes: REMOVE_EOS, cleanup path
│   │
│   ├── Has HOP/SKIP?
│   │   └── Must match exactly, else NOT_FOUND
│   │
│   └── Has LIST/POP?
│       └── Char must exist, else NOT_FOUND
│
└── Key not found: Return NOT_FOUND

CLEANUP after removing EOS:
│
├── Array now empty?
│   └── Yes: Delete array, cleanup parent
│
├── Parent LIST with 1 remaining child?
│   └── Yes: Remove LIST, possibly merge with prefix
│
└── Parent POP with 7 remaining children?
    └── Yes: POP_TO_LIST conversion
```

### Search Decision Tree

```
SEARCH(key):
│
├── Trie empty?
│   └── Yes: Return NULL
│
├── Navigate trie:
│   │
│   ├── Has EOS and key exhausted?
│   │   └── Yes: Return VALUE
│   │
│   ├── Has HOP/SKIP?
│   │   └── Must match exactly, else NULL
│   │
│   └── Has LIST/POP?
│       └── Char must exist, else NULL
│
└── Navigation ended without EOS at key end: NULL
```

---

## See Also

- [README.md](./README.md) - Main documentation, API reference, quick start
- [Implementation Concepts](./concepts.md) - Dirty pointers, SWAR, numeric conversion details
- [Comparison Guide](./comparisons.md) - vs std::map/unordered_map, performance, migration
- [Internal API Reference](./internal_api.md) - Full class and function documentation

---

*Document Version: 1.0*  
*Last Updated: December 2025*
