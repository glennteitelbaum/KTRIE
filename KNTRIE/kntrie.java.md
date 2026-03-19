# kntrie.java.md — Java Port Design

## Overview

Port of KNTRIE to Java. Proves out the data structure in a GC'd language
with no pointer arithmetic. Key simplifications vs C++: no narrowing
(all keys are `long`), no bitmap256 leaf specialization, sentinel is an
empty CompactNode. Values are `Object[]` — generic `V` with boxing.

Implements `NavigableMap<Long, V>` for full Java collections integration.

## Class structure

```java
package com.gteitelbaum.ktrie;

public class KNTrie<V> extends AbstractMap<Long, V>
                       implements NavigableMap<Long, V> {
    private Node root = CompactNode.SENTINEL;
    private long size = 0;
    private int modCount = 0;
}

sealed abstract class Node permits BitmaskNode, CompactNode {
    BitmaskNode parent;     // null for root
    int parentByte;         // dispatch byte that led here
                            // -1 = root, -2 = EOS child
    static final int PARENT_ROOT = -1;
    static final int PARENT_EOS  = -2;
}

final class BitmaskNode extends Node {
    byte[] skip;                // prefix bytes (null if none)
    long b0, b1, b2, b3;       // 256-bit bitmap inline
    Node[] children;            // dense array, children[0] is sentinel
    int descendantCount;        // total entries in subtree
}

final class CompactNode extends Node {
    byte[] skip;                // prefix bytes (null if none)
    long[] keys;                // sorted suffix keys (left-aligned 64-bit)
    Object[] values;            // parallel to keys, dup slots share references
    int count;                  // live entry count (<= keys.length)

    static final CompactNode SENTINEL = new CompactNode(0);
}
```

## Key Representation

Same as C++: all key types normalized to `long` (Java's 64-bit signed).
Sign bit XOR for signed ordering, left-aligned for uniform byte dispatch.

```java
static long toInternal(long key) {
    return key ^ Long.MIN_VALUE;  // flip sign bit
}

static long fromInternal(long internal) {
    return internal ^ Long.MIN_VALUE;
}
```

Byte extraction at depth d:
```java
static int byteAt(long key, int depth) {
    return (int)(key >>> (56 - depth * 8)) & 0xFF;
}
```

## Node Layout

### BitmaskNode

```
Fields:
    parent      BitmaskNode — parent node (null for root)
    parentByte  int         — dispatch byte that led here (-1 root, -2 EOS)
    skip        byte[]      — shared prefix bytes (null if none)
    b0..b3      long        — 256-bit bitmap (4 inline longs)
    children    Node[]      — dense child array, length = popcount(bitmap) + 1
                              children[0] = sentinel (miss fallback)
                              children[1..N] = real children in bitmap order
    descendantCount int     — total entries in subtree
```

**Bitmap operations** use `Long.bitCount()` (JIT intrinsifies to hardware
`popcnt`) and the same branchless accumulation as C++.

**Child lookup:**
```java
int slot = Long.bitCount(b0 & mask0)
         + Long.bitCount(b1 & mask1)
         + Long.bitCount(b2 & mask2)
         + Long.bitCount(b3 & mask3);
return children[slot];  // slot 0 = sentinel on miss
```

Children array position 0 always holds `CompactNode.SENTINEL`. On bitmap miss,
the popcount resolves to 0. On hit, the 1-based index addresses the real child.

### CompactNode

```
Fields:
    parent      BitmaskNode — parent node (null for root)
    parentByte  int         — dispatch byte that led here (-1 root, -2 EOS)
    skip        byte[]      — shared prefix bytes (null if none)
    keys        long[]      — sorted suffix keys, left-aligned
    values      Object[]    — parallel to keys, same length
    count       int         — live entries (may be < keys.length due to hysteresis)
```

**Binary search** uses branchless `Long.compareUnsigned`:
```java
static int findBase(long[] keys, int count, long target) {
    int base = 0;
    int n = count;
    while (n > 1) {
        n >>>= 1;
        base += Long.compareUnsigned(keys[base + n], target) <= 0 ? n : 0;
    }
    return base;
}
```

**Sentinel.** `CompactNode.SENTINEL` is a static final instance with
`count = 0`, `keys = new long[0]`, `values = new Object[0]`,
`parent = null`, `parentByte = PARENT_ROOT`.
`node == SENTINEL` is an identity check.

## Value Storage

All values are `Object[]`. The dup strategy applies:

**Dup slots.** When the compact leaf is allocated with hysteresis padding,
empty slots are filled with copies of adjacent entries (same key, same
value reference). All dup slots for a given key reference the same `V`
object instance.

**put replaces.** `put(key, newVal)` finds the entry and updates all
dup slots to the new reference. For mutable values, the caller can also
mutate the object directly through `get()` — all dups share the same
reference.

## Iterator Design — Actual, Parent-Pointer Based

The iterator holds a leaf + index. Navigation uses parent pointers —
no path stack, no re-descent.

### Structure

```java
class KNTrieIterator<V> implements Iterator<Map.Entry<Long, V>> {
    private final KNTrie<V> trie;
    private final int expectedModCount;
    private CompactNode leaf;
    private int index;
}
```

Two fields of state. The parent chain provides the upward path.

### next()

```
1. Check modCount — throw ConcurrentModificationException if stale.

2. Advance within leaf:
   index++
   skip dups (advance while keys[index] == keys[index-1])
   if index < leaf.count:
       return entry                   // O(1) — common case

3. Walk up via parent pointers:
   node = leaf
   while node.parent != null:
       bm = node.parent
       if node.parentByte == PARENT_EOS:
           // EOS exhausted — try first byte-dispatched child
           nextBit = findFirstSet(bm.bitmap)
       else:
           nextBit = findNextSet(bm.bitmap, node.parentByte + 1)

       if nextBit >= 0:
           child = bm.childByByte(nextBit)
           descend to first entry of child's subtree
           return entry

       // This bitmask exhausted — walk up further
       node = bm

4. Exhausted — set leaf = null, return hasNext() = false
```

**Descend to first entry:** follow each node's first child (EOS if present,
else lowest set bit) until a CompactNode is reached. Return index 0.

**O(1) amortized.** Step 2 handles the common case. Step 3 executes once
per leaf boundary. Total walk-up across full iteration is O(N).

### previous()

Mirror of next(): decrement index, skip dups backward, walk up looking
for previous sibling (highest set bit before `parentByte`), descend to
last entry.

### Iterator ordering

EOS children sort before all byte-dispatched children at each bitmask level
(empty suffix < any non-empty suffix). Within a compact leaf, entries are
in unsigned key order. The iterator produces entries in ascending key order
by following: EOS first, then lowest bitmap bit, recursively.

### Invalidation

```java
if (trie.modCount != expectedModCount)
    throw new ConcurrentModificationException();
```

Standard Java pattern. Any structural mutation (put, remove, clear)
increments `modCount`. Standard Java convention — users who need mutation
during iteration collect keys first.

### Live value access

The iterator returns the live value reference. `entry.getValue()` is the
real object in the trie, not a copy. For mutable values, the caller sees
the current state. Standard Java `Map.Entry` semantics.

### iterator.remove()

Supported. Erases the current entry, increments both `trie.modCount` and
`expectedModCount` (so the iterator stays valid). Advances to the next
entry position internally. Same pattern as `HashMap.Iterator.remove()`.

## API — NavigableMap<Long, V>

```java
public class KNTrie<V> extends AbstractMap<Long, V>
                       implements NavigableMap<Long, V> {

    // --- Map ---
    V get(Object key);
    V put(Long key, V value);
    V remove(Object key);
    boolean containsKey(Object key);
    int size();
    void clear();
    Set<Map.Entry<Long, V>> entrySet();

    // --- SortedMap ---
    Comparator<? super Long> comparator();    // null (natural ordering)
    Long firstKey();
    Long lastKey();
    SortedMap<Long, V> subMap(Long from, Long to);
    SortedMap<Long, V> headMap(Long to);
    SortedMap<Long, V> tailMap(Long from);

    // --- NavigableMap ---
    Map.Entry<Long, V> firstEntry();
    Map.Entry<Long, V> lastEntry();
    Map.Entry<Long, V> pollFirstEntry();
    Map.Entry<Long, V> pollLastEntry();
    Map.Entry<Long, V> floorEntry(Long key);
    Map.Entry<Long, V> ceilingEntry(Long key);
    Map.Entry<Long, V> lowerEntry(Long key);
    Map.Entry<Long, V> higherEntry(Long key);
    Long floorKey(Long key);
    Long ceilingKey(Long key);
    Long lowerKey(Long key);
    Long higherKey(Long key);
    NavigableMap<Long, V> descendingMap();
    NavigableSet<Long> navigableKeySet();
    NavigableSet<Long> descendingKeySet();
    NavigableMap<Long, V> subMap(Long from, boolean fromInc,
                                 Long to, boolean toInc);
    NavigableMap<Long, V> headMap(Long to, boolean inclusive);
    NavigableMap<Long, V> tailMap(Long from, boolean inclusive);

    // --- KTRIE-specific ---
    boolean insert(long key, V value);        // insert only, no overwrite
    long memoryEstimate();                    // approximate bytes used
}
```

Most NavigableMap methods are thin wrappers around `findFirst`, `findLast`,
`findNext`, `findPrev` which the trie already provides. The subMap/headMap/
tailMap views are lightweight wrappers that delegate to the underlying trie
with range bounds.

`insert(long key, V value)` is the only non-Map method — insert-only
semantics not expressible through the Map interface. Uses primitive `long`
to avoid boxing on the hot path.

## Parent Pointer Maintenance

Every mutation that changes the tree structure must update parent pointers:

**insert (into compact leaf):** No parent change. The leaf's parent is
unchanged.

**split (compact → bitmask):** New bitmask node gets the old leaf's parent.
Old leaf's parent updates its child to point to the new bitmask. Each new
child compact node gets the bitmask as parent with the appropriate
parentByte.

**coalesce (bitmask → compact):** New compact leaf gets the bitmask's
parent. Parent updates its child pointer. The bitmask and its children
become garbage (GC handles it).

**remove_child / add_child:** Rebuild children array. New child's parent
is set to the bitmask. Removed child's parent can be nulled (optional,
GC handles it regardless).

**single-child collapse:** The remaining child inherits the collapsed
node's parent pointer and parentByte. The collapsed node becomes garbage.

Cost: one reference write per parent update. No cascading — only the
immediate parent relationship changes.

## Memory Hysteresis

Same bin sizes as C++:

```java
static final int[] BIN_SIZES = {4, 6, 8, 10, 14, 18, 26, 34, 48, 69, 98, 128};
```

Compact leaf allocated at next bin size above needed count. Excess slots
filled with dups. `Arrays.copyOf` for realloc. Beyond 128, power-of-two
with midpoints.

## Skip Prefix

Same as C++: common prefix bytes shared by all entries in a subtree.
Stored as `byte[] skip` on both node types (null when empty). Checked
on entry via `Arrays.mismatch` (JIT-optimized to SIMD on x86).

## Split and Coalesce

### Split (compact → bitmask)

When a compact leaf exceeds `COMPACT_MAX` entries:

1. Extract the byte at the current depth from each key
2. Group entries by that byte
3. Create a new BitmaskNode with bitmap set for each group's byte
4. Each group becomes a child CompactNode
5. Set parent pointers: each child's parent = new bitmask, parentByte = group byte
6. New bitmask's parent = old leaf's parent
7. Update old leaf's parent to point to new bitmask

### Coalesce (bitmask → compact)

When a bitmask node's descendant count drops to `COMPACT_MAX` or below:

1. Collect all entries from all children (recursive)
2. Build a single CompactNode with all entries sorted
3. New compact's parent = bitmask's parent
4. Update bitmask's parent to point to new compact
5. Bitmask and children become garbage

## Testing

```
KNTrieTest.java:
  - Empty trie operations
  - Single entry CRUD
  - Negative key ordering (sign flip)
  - Duplicate insert (returns false, keeps original)
  - put overwrites, returns old value
  - remove returns old value
  - containsKey
  - Iterator: ordered traversal vs TreeMap reference
  - Iterator: ConcurrentModificationException on mutation
  - Iterator: live value visibility
  - Iterator: remove() during iteration
  - Iterator: previous() (descending)
  - NavigableMap: firstEntry / lastEntry
  - NavigableMap: floorEntry / ceilingEntry / lowerEntry / higherEntry
  - NavigableMap: subMap / headMap / tailMap
  - NavigableMap: descendingMap iteration
  - Large-scale random insert + ordered iteration vs TreeMap
  - Memory estimate decreases on erase
  - Clear + reuse
```

## Files

```
src/main/java/com/gteitelbaum/ktrie/
    KNTrie.java           — public API, NavigableMap impl, root management
    Node.java             — sealed abstract, parent pointer
    BitmaskNode.java      — bitmap dispatch, child management
    CompactNode.java      — sorted leaf, binary search, dup management
    KNTrieIterator.java   — cursor + parent-pointer navigation
    KNTrieSubMap.java     — range view for subMap/headMap/tailMap
```

## Non-goals

- No concurrent variant (prove correctness first)
- No primitive specialization (prove algorithm first; `KNTrieLong`
  with `long[]` values is a follow-up)
- No serialization (follow-up)
