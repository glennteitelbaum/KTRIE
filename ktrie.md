# KTRIE: A Trie/B-Tree Hybrid for Ordered Associative Containers

## Paper Outline

### Abstract
*(from ktrie_abstract.md — ~280 words, already drafted)*

### 1 Introduction
- The problem: ordered associative containers in C++ (std::map, std::unordered_map) — tradeoff between ordering and performance
- Key observation: real-world keys have exploitable structure (shared prefixes, bounded fan-out)
- **Figure**: standard trie (HELLO/HELP/HELPER) — 9 nodes × 27 slots, 95% empty. Illustrates the waste that motivates compression.
- **Figure**: digital trie (0x00000000, 0x00000004, 0x000401BC, 0xFEEDFACE) — 9 nodes × 256 slots, 99.5% empty. Same problem at byte granularity.
- **Figure**: B-tree node — wide sorted array with fan-out to children. Illustrates the cache-friendly density that compact nodes inherit.
- The KTRIE idea in one paragraph: prefix/branch/suffix decomposition — trie-style routing with B-tree-style storage
- **Figure**: KTRIE for same key sets — branch nodes replace the sparse trie nodes, compact nodes replace the deep leaf chains. Visual contrast with the trie and B-tree figures above.
- Contributions:
  1. The prefix/branch/suffix decomposition as a unified hybrid design
  2. Bitmap-compressed 256-way branch nodes with branchless dispatch
  3. Compact sorted leaves with suffix absorption
  4. Keysuffix sharing (KSTRIE-specific novel contribution)
  5. Type-specialized value storage (bool packing, inline embedding, heap indirection)
- Paper organization roadmap

### 2 Background and Related Work
- Tries: Fredkin 1960, Knuth 1973/1998
- Patricia trie: Morrison 1968 (prefix compression)
- B-trees: Bayer & McCreight 1972 (wide sorted nodes, cache locality)
- Judy arrays: Baskins 2004 (bitmap-indexed child compression)
- ART: Leis et al. 2013 (adaptive node sizes, SIMD dispatch)
- HAT-trie: Heinz/Zobel/Williams 2002, Askitis/Sinha 2007 (burst trie + hash leaves)
- Branchless binary search: Khuong & Morin 2017
- Boolean bit-packing: Stepanov & Lee 1994, Austern 1998
- Comparison axes for each related structure:
  - **Point lookup**: hash (O(1)) vs trie (O(L)) vs tree (O(L log N)). KTRIE is O(L + log C).
  - **Ordered iteration**: trees and KTRIE provide it; hash tables and HAT-trie leaves do not without post-sorting.
  - **Prefix queries**: KTRIE and ART support structural prefix descent; HAT-trie requires collecting from unordered leaf containers; hash tables cannot.
  - **Memory**: KTRIE compresses shared prefixes/suffixes; ART creates one node per divergent byte; hash tables store full keys per entry; std::map adds 64 bytes per-node overhead.
  - **Write performance**: hash tables O(1) amortized; KTRIE O(L + C) worst case from memmove in compact nodes; HAT-trie O(1) amortized in hash leaf containers.
  - **Concurrency**: ART has well-studied concurrent variants (ART-OLC, ART-ROWEX); KTRIE currently requires external synchronization.
- Position KTRIE relative to each: what we borrow, what we do differently

### 3 Design
*(the KTRIE as a design pattern, not a single implementation)*

Introduce the KTRIE as a general framework for ordered associative containers.
Every key is decomposed into three logical regions — PREFIX, BRANCH, SUFFIX —
with dynamic boundaries determined by the data, not fixed at compile time.

**KTRIE qualities:**
- Ordered associative container: maintains key ordering, supports range queries
- Key decomposition: PREFIX / BRANCH / SUFFIX — dynamic boundaries per subtree
- Four core components:
  1. Branch nodes: sparse fan-out via bitmap-compressed dispatch
  2. Compact nodes: B-tree-style sorted arrays collecting entries
  3. Skip prefix: collapsing shared bytes into a single comparison
  4. Dynamic promotion/demotion: compact ↔ branch based on density
- In the general pattern, branch nodes CAN hold entries (mixed nodes),
  compact nodes CAN have children (internal nodes). KNTRIE/KSTRIE simplify:
  branch nodes are pure routing (except KNTRIE bitmap256 at suffix width=8,
  KSTRIE EOS at bitmask nodes), compact nodes are pure leaves. Branch nodes
  handle routing, compact nodes handle storage — the hybrid eliminates
  the need for mixed nodes.

Introduce the two concrete instantiations:
- **KNTRIE**: fixed-width integer keys (16-, 32-, 64-bit). Bounded depth, suffix type narrows with depth.
- **KSTRIE**: variable-length string keys. Unbounded depth, byte-level branching, keysuffix sharing.

For each core component below, present the KTRIE concept first,
then show how KNTRIE and KSTRIE each implement it.

#### 3.1 Key Decomposition
- **KTRIE concept**: three logical regions — PREFIX (shared bytes absorbed into a single node), BRANCH (trie-style fan-out, one byte per level), SUFFIX (remaining key material collected into a flat sorted node). Boundaries are dynamic per-subtree: a dense region branches deeply, a sparse region absorbs everything into one node.
- **KNTRIE**: sign-flip XOR for signed types, left-alignment into 64 bits for uniform byte extraction, depth-based suffix narrowing (u64 → u32 → u16 → u8). Key width is a compile-time constant — maximum depth is bounded.
- **KSTRIE**: raw byte sequences, character maps (identity, case-insensitive, custom) applied at entry. Variable-length keys mean unbounded depth. EOS (end-of-string) is a structural concern — keys can be prefixes of other keys.

#### 3.2 Branch Nodes
- **KTRIE concept**: 256-way fan-out at each level, compressed via bitmap. Only children that exist consume space.

**3.2.1 Bitmap Operations**

The bitmap is the core mechanism that compresses 256 possible children down to
only the children that exist. All operations below are branchless on modern x86
(compile to shift + popcount + mask instructions, no data-dependent branches).

- **has_bit(idx)**: test whether child `idx` exists.
  Shift word right by `idx`, test bit 0. Single instruction.
  **Figure**: 64-bit word with bit positions marked, one highlighted.

- **count_below(idx) — rank**: count how many children exist before position `idx`.
  Build a mask of all bits below `idx`: `(1 << idx) - 1`. AND with bitmap word,
  popcount the result. For multi-word bitmaps (128-bit, 256-bit), sum popcounts
  of all complete words below the target word, branchlessly via `& -int(w > N)`.
  This gives the slot index into the dense child array.
  **Figure**: bitmap word with mask highlighted, popcount arrows showing rank computation.

- **find_slot(idx) — dispatch**: combine has_bit + count_below. If bit is set,
  returns 1-based index into child array. If not set, returns 0 (sentinel slot).
  The branchless variant: shift left to put target bit at sign position,
  popcount gives rank, mask result to 0 on miss via `& -int(found)`.
  **Figure**: step-by-step: shift → popcount → mask → slot index (or 0).

- **slot_for_insert(idx) — unfiltered rank**: count_below without checking has_bit.
  Returns the position where a new child would be inserted. Used by insert path.

- **find_next_set(start)**: find the next set bit at or after position `start`.
  Mask off bits below `start`: `words[w] & (~0ULL << (start & 63))`. If nonzero,
  `countr_zero` gives the bit position. Otherwise scan forward through words.
  Used by iterator advancement (next sibling) and lower_bound.
  **Figure**: bitmap with start position marked, mask applied, first set bit found.

- **find_prev_set(start)**: find the previous set bit at or before position `start`.
  Mask off bits above `start`: `words[w] & (~0ULL >> (63 - (start & 63)))`. If nonzero,
  `63 - countl_zero` gives the bit position. Otherwise scan backward through words.
  Used by reverse iteration (previous sibling).
  **Figure**: bitmap with start position marked, mask applied, last set bit found.

- **set_bit(idx) / clear_bit(idx)**: OR to set, AND-NOT to clear. Used by insert/erase
  to maintain the bitmap as children are added or removed.

- **Multi-word extension**: for 256-way dispatch, the bitmap is 4 × 64-bit words.
  The target word is selected by `idx >> 6`, the bit within it by `idx & 63`.
  count_below sums popcounts of complete prior words plus the partial popcount
  of the target word. All word selection is branchless via the `& -int(w > N)` trick.
  **Figure**: 4-word bitmap, showing word selection and cross-word popcount accumulation.

**3.2.2 Dispatch Modes**

The bitmap operations above combine into three named dispatch modes, selected
at compile time based on the calling operation:

- **BRANCHLESS** (find path): does NOT check has_bit with a branch. Shift left to
  put target bit at sign position, popcount gives 1-based rank, mask to 0 on miss.
  A miss produces slot 0, which holds the sentinel. The find loop follows the sentinel
  and detects the miss downstream — no branch at the bitmap level. This is the hot path.
- **FAST_EXIT** (insert, erase, iteration): check has_bit first. If absent, return -1
  immediately. If present, popcount minus 1 gives the 0-based index. The branch is
  well-predicted (insert/erase expect the child to exist or not based on the operation)
  and avoids the sentinel overhead.
- **UNFILTERED** (insertion position): count_below without checking has_bit. Returns
  the position where a new child would be inserted in the dense array. Used only by
  the insert path when adding a new child.

**3.2.3 Tagged Pointers (KNTRIE)**

KNTRIE uses tagged `uint64_t` pointers to encode both address and node type:
- **Leaf tag**: bit 63 (LEAF_BIT) set. Pointer recovered by XOR with LEAF_BIT.
- **Internal tag**: no tag bits. The pointer targets `node[2]` — two u64s past the
  start, past header + parent pointer, landing directly on the bitmap. This saves
  one addition per descent level: the find loop uses the pointer as-is for bitmap access.
- **Not-found tag**: bits 63 + 62 set (LEAF_BIT | NOT_FOUND_BIT). Pure bit pattern,
  no backing allocation. User-space pointers never have bits 62–63 set (canonical
  addresses are < 2^47 on 4-level paging, < 2^56 on 5-level), so tags are unambiguous.
- The find loop: `while (!(ptr & LEAF_BIT))` tests the sign bit — single instruction.
  On exit: `ptr & NOT_FOUND_BIT` distinguishes miss from hit — single bit test,
  no pointer dereference.

**3.2.4 Node Type Dispatch (KSTRIE)**

KSTRIE does NOT use tagged pointers. Node type is stored in the node header via a
flags field: bit 0 = FLAG_BITMASK (bitmask node), bit 2 = FLAG_HAS_SKIP (node has
skip prefix). All-zeros = compact node, no skip. Type dispatch reads the header
after following the pointer — one extra memory access vs KNTRIE's tag-in-pointer
scheme, but avoids the address space assumptions that tagged pointers require.

- **Figure**: bitmask node layout — bitmap, sentinel, child slots, EOS, skip bytes. Same key sets from §1, shown after the compact node has overflowed and split — the large-N case. Contrast with §3.3: same data, different structural representation depending on density.

- **KNTRIE branch nodes**:
  - 2-u64 header: `node[0]` = node_header_t, `node[1]` = parent pointer
  - Embed chains for skip (see §3.4)
  - Parent pointers stored in node header (parent_byte_v) for bidirectional iteration
  - Descendant count stored at end of allocation for coalesce decisions

- **KSTRIE branch nodes**:
  - Layout: header + parent ptr + total_tail + bitmap + sentinel + children + EOS child + skip bytes
  - EOS child slot handles keys that terminate at a branch point (e.g., "HELP" exists alongside "HELPER")
  - **total_tail**: estimated byte cost of collapsing the entire subtree back into a single compact node. Three components per entry: (1) keysuffix bytes from compact leaf children, (2) one dispatch byte per entry for the byte consumed at this bitmask level, (3) skip bytes — for each nested bitmask child with skip=S, every entry passing through gains S bytes in the collapsed representation. Maintained incrementally on insert/erase. Coalesce check: `total_tail <= COMPACT_KEYSUFFIX_LIMIT`. Conservative estimate avoids trial collapse — walking an entire subtree only to discover it won't fit.

#### 3.3 Compact Nodes
- **KTRIE concept**: B-tree-style flat sorted arrays absorbing remaining suffixes. In the general pattern, compact nodes could be internal (holding entries AND child pointers, like a B-tree node). KNTRIE/KSTRIE restrict compact nodes to leaf position: branch nodes' byte-level dispatch already provides the fan-out that B-tree internal nodes provide, so compact nodes don't need children. Search within a compact node uses binary search. Threshold-based split/coalesce governs the transition between compact nodes and branch nodes.
- **Figure**: KNTRIE compact node layout (sorted keys + values); KSTRIE compact node layout (L[]/F[]/O[]/keysuffix/values with reconstruction walkthrough). Same key sets from §1 (HELLO/HELP/HELPER; the four hex keys) shown as they would be stored when the entire dataset fits in a single compact node — the small-N case.

- **KNTRIE compact nodes**:
  - Sorted array of narrowed suffix keys (u8/u16/u32/u64 depending on depth) paired with values
  - Branchless adaptive binary search: `bit_width((count-1) | 1u)` splits into power-of-two reduction + main loop. Diff step comparison mirrors loop body (`<=` for find_base, `<` for find_base_first). One saved loop iteration for power-of-2 counts ≥ 4.
  - **Figure — adaptive search walkthrough**: 20 sorted entries, count=20.
    `bit_width(19) = 5`, count2=16, diff=4. diff_val = base+4.
    Case A: key < keys[4] → base stays at 0, search [0..15] with 16-element power-of-2 loop.
    Case B: key ≥ keys[4] → base advances to 4, search [4..19] with 16-element loop.
    Show both cases side by side with the array, diff_val highlighted, and the
    resulting search range shaded. Then show 4 loop iterations (16→8→4→2→1)
    narrowing to final position.
  - Entry-count capacity: COMPACT_MAX = 4096. Split fan-out argument — 4096 / 256 ≈ 16 entries per child on average, ensuring every post-split child is viable. Descendant-count-driven coalesce on erase.
  - Bitmap256 nodes: when remaining suffix is exactly u8 (256 possible values), the key array is replaced by a 256-bit presence bitmap. For VALUE=bool, a second bitmap stores values — 14 u64s for up to 256 boolean key/value pairs.

- **KSTRIE compact nodes**:
  - Node metadata (ck_prefix at node[1]): cap (max entries before realloc), keysuffix_used (bytes occupied in keysuffix region), skip_data_off (byte offset to skip bytes), parent_byte (dispatch byte in parent, ROOT_PARENT_BYTE=256, EOS_PARENT_BYTE=257). Parent pointer at node[2].
  - Parallel arrays: L[] (suffix length, u8), F[] (first byte, u8), O[] (offset into keysuffix region, u16). Keys sorted lexicographically.
  - Binary search on F[], then memcmp on keysuffix tails for disambiguation.
  - **Figure — KSTRIE search walkthrough**: compact node with ~10 entries, showing:
    Step 1: binary search on F[] — narrows to entries sharing the same first byte.
    Entries with a different F[] are eliminated without touching keysuffix at all.
    Step 2: for matching F[] entries, compare L[] (length) — shorter keys sort before
    longer keys, so length mismatches resolve without memcmp.
    Step 3: only for same-F, same-L entries, follow O[] into keysuffix region and
    memcmp the tail bytes. Show keysuffix sharing: entries with O[i] == O[i+1]
    share the same tail storage — the comparison for one implicitly resolves the other.
    Highlight how many full suffix comparisons are skipped: of 10 entries, F[] eliminates
    ~8, L[] eliminates ~1, memcmp touches only ~1. The search cost is dominated by
    the binary search on F[], not by string comparisons.
  - Keysuffix sharing: when consecutive entries share suffix tails, they reference the same offset in the keysuffix region. Insert of a longer key whose prefix matches an existing entry can chain from the existing suffix storage. Reduces effective byte cost of the node.
  - Byte-budget capacity: COMPACT_KEYSUFFIX_LIMIT governs node size. Entry count is a poor proxy for variable-length keys — 10 entries with 200-byte suffixes cost more than 1000 entries with 1-byte suffixes. Split triggers when keysuffix_used exceeds the limit OR any individual suffix exceeds 255 bytes (L[] is u8).
  - Coalesce: bitmask nodes collapse back to compact when the subtree's total_tail fits within budget.

#### 3.4 Skip Prefix
- **KTRIE concept**: when all keys in a subtree share N bytes at a given depth, store those bytes in the node rather than creating N single-child branch levels. A single memcmp replaces N levels of dispatch. Applies to both branch nodes and compact nodes.

- **KNTRIE** — three distinct skip mechanisms:
  1. **Root skip**: `root_prefix_v` + `root_skip_bits_v`, up to 6 bytes for u64. Checked branchlessly via XOR + mask before any descent. Shrinks as divergent keys arrive.
  2. **Compact node skip**: prefix field in the 6-u64 leaf header. Checked via `skip_eq()` (XOR within skip mask region) on entry. Set at node creation based on shared prefix of all entries.
  3. **Branch node embed chains**: single-child levels inlined into the parent allocation as 6-u64 blocks (bitmap + miss ptr + child ptr). The find loop traverses embeds transparently — `bm_child()` works on any bitmap pointer, whether embed or final. Adds 6 u64s per skip level to the node allocation but avoids separate heap allocations.

- **KSTRIE** — two skip mechanisms:
  1. **Bitmask node skip**: skip bytes appended to end of allocation, after EOS child. Skip length stored in node header. `match_skip_fast` (read path, memcmp) vs `match_prefix` (write path, byte-by-byte with match_len for divergence detection).
  2. **Compact node skip**: skip bytes at byte offset `skip_data_off` (recorded in ck_prefix). Checked before suffix search. Set when node is created or when a bitmask subtree coalesces back to compact.

### 4 Shared Implementation
*(engineering choices common to both instantiations, but not required by the KTRIE pattern)*

#### 4.1 Value Storage
- Three categories determined at compile time via type traits:
  - A (trivial inline): sizeof ≤ 8, trivially copyable → stored directly in slot array
  - B (packed bool): VALUE=bool → one bit per entry in u64 words
  - C (heap pointer): non-trivial or sizeof > 8 → heap-allocated, pointer in slot
- Compile-time dispatch via `if constexpr`, zero runtime overhead
- Normalization to fixed-width unsigned types to reduce template instantiations

#### 4.2 Sentinel Design
- **Concept**: a distinguished value that terminates search without a conditional branch. Every miss resolves through the same code path as a hit — only the data differs. No pointer dereference needed to detect absence. The sentinel appears in two roles: empty root (no entries) and branch node miss target (absent child byte).
- **KNTRIE**: tagged pointer — `SENTINEL_TAGGED = LEAF_BIT | NOT_FOUND_BIT` (bits 63+62 set), a pure bit pattern with no backing allocation. Part of the tagged pointer scheme described in §3.2.3. Slot 0 in every child array holds this value. Bitmap miss → popcount returns 0 → loads sentinel tag → find loop exits on LEAF_BIT → NOT_FOUND_BIT test detects miss. Zero memory cost.
- **KSTRIE**: static zero-initialized compact node (`sentinel_data_[4]`), a real 4-u64 allocation that is never freed. Part of the flags-based dispatch described in §3.2.4 — no tagged pointers, so the sentinel must be a valid node that reads as empty (count=0, flags=0, skip=0). Bitmask nodes store a pointer to this static object. Small fixed memory cost (32 bytes, shared across all instances of the same type).

#### 4.3 Memory Hysteresis
- Size-class allocation: 26 classes from 4 to 16,384 u64s, ~1.5× growth per step
- Padding provides room for in-place insert/erase without reallocation
- Shrink hysteresis: only shrink when allocated exceeds class for 2× needed size
- Prevents oscillation at size-class boundaries on alternating insert/erase

### 5 Instantiation-Specific Details
*(unique to each instantiation, not part of the KTRIE pattern or the shared implementation)*

#### 5.1 KNTRIE
- **Node header**: all nodes share a packed 8-byte header (node_header_t) at node[0]:
  depth_v (16 bits, encodes key position), entries_v (16 bits, entry/child count),
  alloc_u64_v (16 bits, allocation size in u64s), parent_byte_v (16 bits, dispatch byte
  in parent bitmask, ROOT_BYTE=256 to terminate upward walk).
- **depth_t bitfield**: packed 16-bit encoding of leaf position:
  is_skip (1 bit, redundant with skip≠0 but avoids reading skip count on hot read path),
  skip (3 bits, prefix bytes to compare, 0–6),
  consumed (6 bits, total bytes resolved above this leaf),
  shift (6 bits, = 64 − suffix_width_bits, only {0, 32, 48, 56}).
  The `suffix()` function extracts the leaf's suffix from a root-level internal key:
  `(ik << consumed) >> shift`. On x86-64-v3+ (BMI2), compiles to `shlx` + `shrx` —
  two single-cycle operations with no data-dependent branches.
- **Function pointer dispatch**: leaf header carries 3 function pointers (find_fn, adv_fn, edge_fn) set at construction. Type-erased leaf access — find loop doesn't need to know suffix type. Cost: 24 bytes per leaf, benefit: single indirect call instead of template recursion on hot path.
- **Root prefix capture**: root stores up to 6 prefix bytes (for u64), checked branchlessly before descent. Root skip shrinks as divergent keys arrive. normalize_root absorbs single-child roots, coalesce_bm_to_leaf collapses small subtrees.
- **Depth-based dispatch**: depth_switch converts runtime byte depth to compile-time BITS parameter via switch statement. Generates all template instantiations upfront.

#### 5.2 KSTRIE
- **Node header**: all nodes share an 8-byte header at node[0]:
  alloc_u64 (allocation size), count (entries for compact, children for bitmask),
  flags (bit 0: FLAG_BITMASK, bit 2: FLAG_HAS_SKIP; all-zeros = compact no skip),
  skip (prefix bytes captured, 0–254), slots_off (cached u64 offset to value region,
  avoids recomputing layout on every value access, updated on grow/shrink/shuffle).
  No tagged pointers — type is in the node, dispatched via `is_compact()` / `is_bitmap()`.
- **Prefix operations**: prefix_walk (iterator pair), prefix_copy (clone_tree), prefix_erase (subtree detach + destroy), prefix_split (pointer steal, O(1) at bitmask boundaries). Structural operations that exploit the KTRIE's branch node topology.
- **Lazy key reconstruction**: iterator stores leaf pointer + position only. Key is built on demand by walking leaf → root via parent pointers, prepending skip bytes and dispatch bytes at each level. Cached until iterator advances. Value-only iteration has zero key overhead.
- **EOS handling**: keys that are prefixes of other keys (e.g., "HELP" and "HELPER") are handled via the EOS child slot on bitmask nodes. The EOS child points to a compact leaf that stores entries terminating at that branch point.
- **Character maps**: compile-time 256-byte lookup table transforms key bytes before dispatch. Many-to-one maps enable case-insensitive containers. Map is a template parameter — zero runtime dispatch cost.

### 6 Operations

Let N = number of entries, L = key length in bytes, C = compact node capacity
(COMPACT_MAX for KNTRIE, effective entry count under COMPACT_KEYSUFFIX_LIMIT for KSTRIE).
The log C term represents the binary search within a compact node. C is a structural
constant bounded by the split threshold, so log C is bounded — but stating it explicitly
is more honest than collapsing it into O(1), since a 4096-entry leaf search is ~12 comparisons.

For KNTRIE, L ∈ {2, 4, 8} — a compile-time constant per instantiation. We write O(L + log C)
rather than O(1) because the constant factors are meaningful and collapsing them obscures the
real cost structure.

For KSTRIE, L varies per key and is not bounded at compile time.

#### 6.1 Find — O(L + log C)
- Root prefix check → bitmask descent loop → leaf dispatch
- Branch descent: O(L) — at most L levels, each consuming one byte via branchless bitmap dispatch
- Compact node search: O(log C) — branchless adaptive binary search (KNTRIE), binary search on F[] + memcmp (KSTRIE)
- Total: O(L + log C). In practice, skip prefix compression reduces the effective L, and most subtrees terminate in compact nodes well below C.
- Single indirect call at leaf (function pointer dispatch)

#### 6.2 Insert — O(L + log C) amortized, O(L + C) worst case
- Recursive descent through sentinel/leaf/bitmask: O(L)
- Compact node insertion: O(log C) search + O(C) memmove to open a gap
- Skip prefix divergence → split (new bitmask at divergence point): O(1) structural change
- Compact node overflow → promote to bitmask with up to 256 children: O(C) to redistribute entries. Amortized O(1) per insert since overflow occurs every C inserts.
- Parent pointer maintenance on all structural changes
- **Figures — KNTRIE structural evolution**:
  1. Empty: sentinel root
  2. Insert 1: single compact node, one entry, root skip captures prefix
  3. Insert 2: same compact node, two sorted entries
  4. Insert COMPACT_MAX+1: compact overflows → bitmask with compact children, entries distributed by dispatch byte
- **Figures — KSTRIE structural evolution**:
  1. Empty: sentinel root
  2. Insert 1: single compact node, one entry, skip captures full key minus suffix
  3. Insert 2: same compact node, two entries in L[]/F[]/O[] arrays, keysuffix region grows
  4. Insert past COMPACT_KEYSUFFIX_LIMIT: compact overflows → bitmask with EOS + compact children, keysuffix bytes distributed across child nodes

#### 6.3 Erase — O(L + log C) amortized, O(L + C) worst case
- Descent + remove: O(L + log C) search + O(C) memmove to close gap
- Descendant-count-driven coalesce back to compact node: O(C) to collect entries. Amortized O(1) per erase since coalesce occurs only when subtree total drops to C.
- Single-child collapse (absorb skip into child): O(1)
- Root normalization on structural changes
- **Figures — KNTRIE structural collapse** (reverse of insert evolution):
  1. Bitmask with compact children (starting state, >COMPACT_MAX entries)
  2. Erase below COMPACT_MAX: descendant count triggers coalesce → single compact node, bitmask deallocated
  3. Erase to 1: single compact node, one entry
  4. Erase last: compact deallocated → sentinel root
- **Figures — KSTRIE structural collapse** (reverse of insert evolution):
  1. Bitmask with EOS + compact children (starting state, keysuffix budget exceeded)
  2. Erase below COMPACT_KEYSUFFIX_LIMIT: total_tail check triggers coalesce → single compact node, bitmask + children deallocated
  3. Erase to 1: single compact node, one entry
  4. Erase last: compact deallocated → sentinel root

**Aggressive reclamation.** Unlike `std::unordered_map`, which retains its bucket array until an explicit `rehash()` or `reserve()` call, KTRIE reclaims memory eagerly: erase triggers memmove compaction, shrink hysteresis downsizes nodes when allocated capacity exceeds 2× needed, and coalesce rebuilds entire subtrees into single compact nodes when entry count drops below the threshold. This aggressive reclamation is a deliberate design choice: it maintains the memory density that is KTRIE's primary advantage, at the cost of iterator stability. A "lazy" reclamation strategy (deferring reorganization) would preserve iterators across mutation but sacrifice memory density — the very property that keeps KTRIE's working set in faster cache levels.

#### 6.4 Iteration — O(1) amortized per advance
- Bidirectional, O(1) amortized
- Hot path: within-leaf position increment (no search, no pointer chase)
- Cold path: parent-pointer walk + sibling descent via bitmap scan. Cost proportional to tree depth, amortized across all entries in the exhausted leaf.
- Lazy key reconstruction (KSTRIE): O(L) on first access, cached until advance
- **Iterator invalidation**: more restrictive than both `std::map` (which preserves iterators on insert and erase of other keys) and `std::unordered_map` (which preserves iterators on erase of any other element). KTRIE invalidates all iterators on any mutation — insert, erase, or insert_or_assign. The reason: aggressive reclamation means any mutation can trigger structural reorganization (compact node splits, coalesces, reallocation, memmove compaction) that may move or deallocate nodes an iterator points to, even when the mutated key is in a distant subtree. This is a direct consequence of the dense-packing design that achieves KTRIE's memory advantage.

### 7 Evaluation

#### 7.1 Experimental Setup
- Hardware: CPU model, RAM, cache hierarchy
- Compiler and flags: GCC, -O2, -march=x86-64-v4; MSVC /O2 /arch:AVX512 for validation
- Benchmark methodology: key distributions (random, sequential, clustered), operation mix
- Memory measurement: custom tracking allocator, per-allocation accounting
- Note: MSVC captures umap bucket array allocations (sawtooth visible); libstdc++ does not (bucket array bypasses custom allocator via rebind path)

#### 7.2 KNTRIE Benchmarks
- Lookup: vs std::map, std::unordered_map across u16/u32/u64
- Insert/erase
- Iteration
- Memory footprint
- Threshold sensitivity: COMPACT_MAX sweep

#### 7.3 KSTRIE Benchmarks
- Lookup: vs std::map, std::unordered_map
- Prefix queries
- Memory footprint with key sharing analysis
- Threshold sensitivity: COMPACT_KEYSUFFIX_LIMIT sweep

#### 7.4 Memory Analysis
- Per-entry cost breakdown: node overhead, key storage, value storage
- umap sawtooth: rehash-driven memory spikes, ~30% variation in B/entry
- kntrie sub-raw-data memory: 12.2 B/entry at favorable points vs 12 bytes raw (u64→i32)
- Cross-platform comparison: Linux vs MSVC

### 8 Discussion
- When KTRIE wins: read-heavy, ordered access, shared-prefix keys
- When it doesn't: pure point-lookup (hash tables win), write-heavy with large leaves
- Limitations: no concurrency, iterator invalidation on mutation
- **Aggressive reclamation trade-off**: KTRIE's iterator invalidation is stricter than std::map and std::unordered_map. This is the cost of maintaining memory density through eager compaction. The alternative — deferred reclamation preserving iterator stability — would sacrifice the working-set advantage that keeps KTRIE competitive with hash tables. For workloads that hold iterators across mutations, a snapshot or versioned iterator design could be explored, at the cost of indirection on the hot path.
- Adversarial distributions: random keys with no shared structure

### 9 Conclusion
- Restate the prefix/branch/suffix decomposition contribution
- Keysuffix sharing as novel contribution
- Practical result: ordered container competitive with hash tables on speed, fraction of the memory

### References

### Appendix
- A: Performance graphs — KNTRIE u16/u32/u64 (find, insert, erase, iteration, memory), KSTRIE (find, insert, erase, iteration, memory, prefix operations), threshold sensitivity sweeps
