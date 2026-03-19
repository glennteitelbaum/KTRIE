// KNTrie.java — ordered associative container with long keys
// Trie/B-tree hybrid implementing NavigableMap<Long, V>

import java.util.*;
import java.util.function.*;

public class KNTrie<V> extends AbstractMap<Long, V>
                       implements NavigableMap<Long, V> {

    // === Constants ===
    private static final int COMPACT_MAX = 4096;
    private static final int MIN_CAPACITY = 4;
    private static final int KEY_BYTES = 8;
    private static final int PARENT_ROOT = -1;
    private static final int PARENT_EOS = -2;

    // === Node hierarchy ===
    static abstract sealed class Node permits BitmaskNode, CompactNode {
        BitmaskNode parent;
        int parentByte = PARENT_ROOT;
        int depth;      // key bytes consumed BEFORE this node
    }

    static final class BitmaskNode extends Node {
        byte[] skip;    // prefix bytes shared by all entries in subtree (null if none)
        long b0, b1, b2, b3;
        Node[] children;
        int descendantCount;

        int skipLen() { return skip == null ? 0 : skip.length; }
        int effectiveDepth() { return depth + skipLen(); }

        long word(int w) {
            return switch (w) { case 0 -> b0; case 1 -> b1; case 2 -> b2; default -> b3; };
        }
        void setWord(int w, long val) {
            switch (w) { case 0 -> b0 = val; case 1 -> b1 = val; case 2 -> b2 = val; default -> b3 = val; }
        }
        boolean hasBit(int idx) { return (word(idx >> 6) & (1L << (idx & 63))) != 0; }
        void setBit(int idx) { int w = idx >> 6; setWord(w, word(w) | (1L << (idx & 63))); }
        void clearBit(int idx) { int w = idx >> 6; setWord(w, word(w) & ~(1L << (idx & 63))); }

        // 1-based slot for a known-present bit
        int slotOf(int idx) {
            int w = idx >> 6, b = idx & 63;
            long shifted = word(w) << (63 - b);
            int slot = Long.bitCount(shifted);
            if (w > 0) slot += Long.bitCount(b0);
            if (w > 1) slot += Long.bitCount(b1);
            if (w > 2) slot += Long.bitCount(b2);
            return slot;
        }

        // Branchless dispatch: returns child on hit, sentinel on miss
        Node dispatch(int idx) {
            int w = idx >> 6, b = idx & 63;
            long shifted = word(w) << (63 - b);
            int slot = Long.bitCount(shifted);
            if (w > 0) slot += Long.bitCount(b0);
            if (w > 1) slot += Long.bitCount(b1);
            if (w > 2) slot += Long.bitCount(b2);
            boolean hit = (shifted & Long.MIN_VALUE) != 0;
            return children[hit ? slot : 0];
        }

        int childCount() {
            return Long.bitCount(b0) + Long.bitCount(b1)
                 + Long.bitCount(b2) + Long.bitCount(b3);
        }

        int findNextSet(int start) {
            for (int w = start >> 6; w < 4; w++) {
                long masked = word(w) & ((-1L) << (Math.max(start, w << 6) & 63));
                if (masked != 0)
                    return (w << 6) + Long.numberOfTrailingZeros(masked);
                start = (w + 1) << 6;
            }
            return -1;
        }

        int findPrevSet(int start) {
            for (int w = start >> 6; w >= 0; w--) {
                int bit = Math.min(start, (w << 6) + 63) & 63;
                long masked = word(w) & ((-1L) >>> (63 - bit));
                if (masked != 0)
                    return (w << 6) + 63 - Long.numberOfLeadingZeros(masked);
                start = (w << 6) - 1;
            }
            return -1;
        }

        void insertChild(int idx, Node child) {
            setBit(idx);
            int slot = slotOf(idx);
            int oldLen = children.length - 1; // exclude EOS
            Node[] nc = new Node[children.length + 1];
            System.arraycopy(children, 0, nc, 0, slot);
            nc[slot] = child;
            System.arraycopy(children, slot, nc, slot + 1, oldLen - slot + 1); // +1 for EOS
            children = nc;
            child.parent = this;
            child.parentByte = idx;
        }

        void removeChild(int idx) {
            int slot = slotOf(idx);
            clearBit(idx);
            Node[] nc = new Node[children.length - 1];
            System.arraycopy(children, 0, nc, 0, slot);
            System.arraycopy(children, slot + 1, nc, slot, children.length - slot - 1);
            children = nc;
        }

        void replaceChild(int idx, Node nc) {
            children[slotOf(idx)] = nc;
            nc.parent = this;
            nc.parentByte = idx;
        }

        Node eosChild() { return children[children.length - 1]; }
        void setEosChild(Node c) {
            children[children.length - 1] = c;
            if (c != CompactNode.SENTINEL) { c.parent = this; c.parentByte = PARENT_EOS; }
        }
        boolean hasEos() { return eosChild() != CompactNode.SENTINEL; }
    }

    @SuppressWarnings("rawtypes")
    static final class CompactNode extends Node {
        long[] keys;        // power-of-two, fully populated with spread dups
        Object[] values;    // parallel, dups share same object reference
        int count;          // unique entries

        static final CompactNode SENTINEL = new CompactNode();
        private CompactNode() { keys = new long[0]; values = new Object[0]; count = 0; }

        CompactNode(int slots) {
            slots = Math.max(MIN_CAPACITY, Integer.highestOneBit(Math.max(slots - 1, 1)) << 1);
            keys = new long[slots];
            values = new Object[slots];
            count = 0;
        }

        // --- Branchless binary search on full spread array ---
        int findBase(long target) {
            int base = 0, n = keys.length;
            while (n > 1) {
                n >>>= 1;
                base += Long.compareUnsigned(keys[base + n], target) <= 0 ? n : 0;
            }
            return base;
        }

        // --- Spread: distribute n unique entries across keys.length slots ---
        // Slot i maps to unique entry (i * n / slots).
        // 5 entries in 8 slots → [A A B B C D D E]
        void spread(long[] uk, Object[] uv, int n) {
            count = n;
            int slots = keys.length;
            if (n == 0) return;
            for (int i = slots - 1; i >= 0; i--) {
                int src = (int)((long)i * n / slots);
                keys[i] = uk[src];
                values[i] = uv[src];
            }
        }

        // --- Extract unique entries from spread array ---
        int extractUniques(long[] outK, Object[] outV) {
            int j = 0;
            for (int i = 0; i < keys.length; i++) {
                if (i == 0 || keys[i] != keys[i - 1]) {
                    outK[j] = keys[i];
                    outV[j] = values[i];
                    j++;
                }
            }
            return j;
        }

        // --- Erase O(1): overwrite key's range with neighbor ---
        Object eraseKey(long target) {
            int pos = findBase(target);
            if (keys[pos] != target) return null;
            Object old = values[pos];
            // Find range of this key
            int lo = pos, hi = pos;
            while (lo > 0 && keys[lo - 1] == target) lo--;
            while (hi < keys.length - 1 && keys[hi + 1] == target) hi++;
            // Overwrite with neighbor
            if (lo > 0) {
                long nk = keys[lo - 1]; Object nv = values[lo - 1];
                for (int i = lo; i <= hi; i++) { keys[i] = nk; values[i] = nv; }
            } else if (hi < keys.length - 1) {
                long nk = keys[hi + 1]; Object nv = values[hi + 1];
                for (int i = lo; i <= hi; i++) { keys[i] = nk; values[i] = nv; }
            }
            // else: sole entry, caller handles count==0
            count--;
            return old;
        }

        // --- Insert in-place: compact left → insert → spread backward ---
        // No temp arrays allocated. O(slots) total.
        boolean insertKey(long ikey, Object val) {
            // Step 1: compact uniques to positions 0..count-1
            int n = 0;
            for (int i = 0; i < keys.length; i++) {
                if (i == 0 || keys[i] != keys[i - 1]) {
                    keys[n] = keys[i];
                    values[n] = values[i];
                    n++;
                }
            }
            // Step 2: binary search in compacted uniques for insertion point
            int lo = 0, hi = n;
            while (lo < hi) {
                int mid = (lo + hi) >>> 1;
                if (Long.compareUnsigned(keys[mid], ikey) < 0) lo = mid + 1;
                else hi = mid;
            }
            if (lo < n && keys[lo] == ikey) {
                // Duplicate — re-spread without inserting
                spread(keys, values, n);
                return false;
            }
            // Step 3: grow if full
            if (n >= keys.length) {
                int newSlots = keys.length * 2;
                keys = Arrays.copyOf(keys, newSlots);
                values = Arrays.copyOf(values, newSlots);
            }
            // Step 4: shift right to make room at lo
            System.arraycopy(keys, lo, keys, lo + 1, n - lo);
            System.arraycopy(values, lo, values, lo + 1, n - lo);
            keys[lo] = ikey;
            values[lo] = val;
            n++;
            // Step 5: spread backward in-place (src indices <= dest, safe)
            spread(keys, values, n);
            return true;
        }

        // --- Overwrite all dups of a key with new value ---
        void updateValue(long target, Object newVal) {
            for (int i = 0; i < keys.length; i++)
                if (keys[i] == target) values[i] = newVal;
        }

        // --- Shrink if way under-utilized ---
        void maybeShrink() {
            if (count > 0 && keys.length > MIN_CAPACITY && count <= keys.length / 4) {
                int newSlots = Math.max(MIN_CAPACITY, Integer.highestOneBit(count) << 1);
                if (newSlots < keys.length) {
                    // Compact left, then truncate and spread
                    int n = 0;
                    for (int i = 0; i < keys.length; i++)
                        if (i == 0 || keys[i] != keys[i - 1])
                            { keys[n] = keys[i]; values[n] = values[i]; n++; }
                    keys = Arrays.copyOf(keys, newSlots);
                    values = Arrays.copyOf(values, newSlots);
                    spread(keys, values, n);
                }
            }
        }
    }

    // === Fields ===
    private Node root = CompactNode.SENTINEL;
    private int size = 0;
    private int modCount = 0;

    // === Key conversion ===
    static long toInternal(long key) { return key ^ Long.MIN_VALUE; }
    static long fromInternal(long ik) { return ik ^ Long.MIN_VALUE; }
    static int byteAt(long key, int depth) { return (int)(key >>> (56 - depth * 8)) & 0xFF; }

    static byte[] extractSkip(long ikey, int from, int len) {
        if (len <= 0) return null;
        byte[] s = new byte[len];
        for (int i = 0; i < len; i++) s[i] = (byte) byteAt(ikey, from + i);
        return s;
    }

    static boolean matchSkip(BitmaskNode bm, long ikey) {
        if (bm.skip == null) return true;
        for (int i = 0; i < bm.skip.length; i++)
            if (byteAt(ikey, bm.depth + i) != (bm.skip[i] & 0xFF)) return false;
        return true;
    }

    static int skipMismatch(BitmaskNode bm, long ikey) {
        if (bm.skip == null) return -1;
        for (int i = 0; i < bm.skip.length; i++)
            if (byteAt(ikey, bm.depth + i) != (bm.skip[i] & 0xFF)) return i;
        return -1;
    }

    // === find ===
    @SuppressWarnings("unchecked")
    private V findImpl(long ikey) {
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm, ikey)) return null;
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        if (node != CompactNode.SENTINEL) {
            CompactNode c = (CompactNode) node;
            if (c.count == 0) return null;
            int pos = c.findBase(ikey);
            return c.keys[pos] == ikey ? (V) c.values[pos] : null;
        }
        return null;
    }

    // === put / insert ===
    @SuppressWarnings("unchecked")
    private V putImpl(long ikey, V value, boolean onlyIfAbsent) {
        if (root == CompactNode.SENTINEL) {
            CompactNode c = new CompactNode(MIN_CAPACITY);
            c.depth = 0;
            c.spread(new long[]{ikey}, new Object[]{value}, 1);
            root = c;
            size++; modCount++;
            return null;
        }

        // Descend through bitmask nodes
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm, ikey);
            if (mm >= 0) {
                splitSkip(bm, ikey, value, mm);
                size++; modCount++;
                return null;
            }
            int b = byteAt(ikey, bm.effectiveDepth());
            if (!bm.hasBit(b)) {
                CompactNode child = new CompactNode(MIN_CAPACITY);
                child.depth = bm.effectiveDepth() + 1;
                child.spread(new long[]{ikey}, new Object[]{value}, 1);
                bm.insertChild(b, child);
                bm.descendantCount++;
                for (BitmaskNode p = bm.parent; p != null; p = p.parent) p.descendantCount++;
                size++; modCount++;
                return null;
            }
            node = bm.children[bm.slotOf(b)];
        }

        // Landed at a compact leaf
        CompactNode c = (CompactNode) node;
        int pos = c.findBase(ikey);
        if (c.keys[pos] == ikey) {
            // Found — overwrite or reject
            if (onlyIfAbsent) return (V) c.values[pos];
            V old = (V) c.values[pos];
            c.updateValue(ikey, value);
            modCount++;
            return old;
        }
        // Miss — insert
        c.insertKey(ikey, value);
        for (BitmaskNode p = c.parent; p != null; p = p.parent) p.descendantCount++;
        if (c.count > COMPACT_MAX) split(c);
        size++; modCount++;
        return null;
    }

    // === remove ===
    @SuppressWarnings("unchecked")
    private V removeImpl(long ikey) {
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm, ikey)) return null;
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        if (c.count == 0) return null;
        Object old = c.eraseKey(ikey);
        if (old == null) return null;
        c.maybeShrink();
        for (BitmaskNode p = c.parent; p != null; p = p.parent) p.descendantCount--;
        if (c.count == 0) removeEmptyLeaf(c);
        else checkCoalesce(c.parent);
        size--; modCount++;
        return (V) old;
    }

    private void removeEmptyLeaf(CompactNode c) {
        if (c.parent == null) { root = CompactNode.SENTINEL; return; }
        BitmaskNode p = c.parent;
        if (c.parentByte == PARENT_EOS) p.setEosChild(CompactNode.SENTINEL);
        else p.removeChild(c.parentByte);
        checkCoalesce(p);
    }

    private void checkCoalesce(BitmaskNode bm) {
        if (bm == null) return;
        int total = bm.childCount() + (bm.hasEos() ? 1 : 0);
        if (bm.descendantCount <= COMPACT_MAX) { coalesce(bm); return; }
        if (total == 0) { replaceNode(bm, CompactNode.SENTINEL); checkCoalesce(bm.parent); return; }
        if (total == 1) collapse(bm);
    }

    // === split: compact → bitmask ===
    private void split(CompactNode leaf) {
        int d = leaf.depth;
        if (d >= KEY_BYTES) return;

        // Extract unique entries from spread array
        long[] uk = new long[leaf.count];
        Object[] uv = new Object[leaf.count];
        int n = leaf.extractUniques(uk, uv);

        // Find common prefix among unique entries
        int common = KEY_BYTES - d;
        for (int j = 1; j < n && common > 0; j++)
            for (int k = 0; k < common; k++)
                if (byteAt(uk[0], d + k) != byteAt(uk[j], d + k))
                    { common = k; break; }

        int dispatchDepth = d + common;
        if (dispatchDepth >= KEY_BYTES) return;

        // Group unique entries by byte at dispatchDepth
        int[] groupCount = new int[256];
        for (int i = 0; i < n; i++)
            groupCount[byteAt(uk[i], dispatchDepth)]++;

        BitmaskNode bm = new BitmaskNode();
        bm.depth = d;
        bm.skip = common > 0 ? extractSkip(uk[0], d, common) : null;
        bm.descendantCount = n;

        int cc = 0;
        for (int b = 0; b < 256; b++) if (groupCount[b] > 0) cc++;
        bm.children = new Node[cc + 2];
        bm.children[0] = CompactNode.SENTINEL;
        bm.children[cc + 1] = CompactNode.SENTINEL;

        int slot = 1;
        int upos = 0; // position in unique arrays (sorted, so groups are contiguous)
        for (int b = 0; b < 256; b++) {
            if (groupCount[b] == 0) continue;
            bm.setBit(b);
            int gc = groupCount[b];
            CompactNode child = new CompactNode(gc);
            child.depth = dispatchDepth + 1;
            child.parent = bm;
            child.parentByte = b;
            child.spread(Arrays.copyOfRange(uk, upos, upos + gc),
                         Arrays.copyOfRange(uv, upos, upos + gc), gc);
            bm.children[slot++] = child;
            upos += gc;
        }
        replaceNode(leaf, bm);
        for (int i = 1; i < bm.children.length - 1; i++)
            if (bm.children[i] instanceof CompactNode c && c.count > COMPACT_MAX) split(c);
    }

    // === splitSkip: key diverges within a bitmask node's skip prefix ===
    private void splitSkip(BitmaskNode bmNode, long ikey, V value, int mm) {
        BitmaskNode oldParent = bmNode.parent;
        int oldParentByte = bmNode.parentByte;
        int splitDepth = bmNode.depth + mm;

        // New bitmask at the mismatch point
        BitmaskNode bm = new BitmaskNode();
        bm.depth = bmNode.depth;
        bm.skip = mm > 0 ? extractSkip(ikey, bmNode.depth, mm) : null;
        bm.descendantCount = bmNode.descendantCount + 1;

        int existByte = bmNode.skip[mm] & 0xFF;
        int newByte = byteAt(ikey, splitDepth);

        // Trim existing bitmask's skip
        int remaining = bmNode.skip.length - mm - 1;
        bmNode.skip = remaining > 0
            ? Arrays.copyOfRange(bmNode.skip, mm + 1, bmNode.skip.length) : null;
        bmNode.depth = splitDepth + 1;

        // New leaf for the new key
        CompactNode nl = new CompactNode(MIN_CAPACITY);
        nl.depth = splitDepth + 1;
        nl.spread(new long[]{ikey}, new Object[]{value}, 1);

        // Build children: [sentinel, child_lo, child_hi, eos=sentinel]
        bm.children = new Node[4];
        bm.children[0] = CompactNode.SENTINEL;
        bm.children[3] = CompactNode.SENTINEL;
        bm.setBit(existByte);
        bm.setBit(newByte);

        if (existByte < newByte) {
            bm.children[1] = bmNode;
            bm.children[2] = nl;
        } else {
            bm.children[1] = nl;
            bm.children[2] = bmNode;
        }

        bmNode.parent = bm; bmNode.parentByte = existByte;
        nl.parent = bm; nl.parentByte = newByte;

        // Link into tree at old position
        bm.parent = oldParent;
        bm.parentByte = oldParentByte;
        if (oldParent == null) {
            root = bm;
        } else {
            if (oldParentByte == PARENT_EOS) oldParent.setEosChild(bm);
            else oldParent.replaceChild(oldParentByte, bm);
        }
    }

    // === coalesce: bitmask → compact ===
    private void coalesce(BitmaskNode bm) {
        List<long[]> ak = new ArrayList<>();
        List<Object[]> av = new ArrayList<>();
        collectEntries(bm, ak, av);
        int total = ak.size();
        if (total == 0) { replaceNode(bm, CompactNode.SENTINEL); return; }
        long[] uk = new long[total];
        Object[] uv = new Object[total];
        for (int i = 0; i < total; i++) { uk[i] = ak.get(i)[0]; uv[i] = av.get(i)[0]; }
        CompactNode c = new CompactNode(total);
        c.depth = bm.depth;
        c.spread(uk, uv, total);
        replaceNode(bm, c);
    }

    private void collectEntries(Node node, List<long[]> keys, List<Object[]> vals) {
        if (node == CompactNode.SENTINEL) return;
        if (node instanceof CompactNode c) {
            // Extract uniques from spread array
            for (int i = 0; i < c.keys.length; i++) {
                if (i == 0 || c.keys[i] != c.keys[i - 1]) {
                    keys.add(new long[]{c.keys[i]});
                    vals.add(new Object[]{c.values[i]});
                }
            }
            return;
        }
        BitmaskNode bm = (BitmaskNode) node;
        collectEntries(bm.eosChild(), keys, vals);
        int bit = bm.findNextSet(0);
        while (bit >= 0) { collectEntries(bm.children[bm.slotOf(bit)], keys, vals); bit = bm.findNextSet(bit + 1); }
    }

    // === collapse: single-child bitmask → child ===
    private void collapse(BitmaskNode bm) {
        Node child; int db;
        if (bm.hasEos() && bm.childCount() == 0) { child = bm.eosChild(); db = PARENT_EOS; }
        else if (!bm.hasEos() && bm.childCount() == 1) { int bit = bm.findNextSet(0); child = bm.children[bm.slotOf(bit)]; db = bit; }
        else return;

        if (child instanceof CompactNode) {
            // Compact has no skip — just inherit position
            child.depth = bm.depth;
        } else {
            // Bitmask child: merge bm.skip + dispatch byte + child.skip
            BitmaskNode bmChild = (BitmaskNode) child;
            int bmSk = bm.skipLen(), chSk = bmChild.skipLen();
            int dispB = db >= 0 ? 1 : 0;
            int newLen = bmSk + dispB + chSk;
            byte[] ns = null;
            if (newLen > 0) {
                ns = new byte[newLen]; int off = 0;
                if (bm.skip != null) { System.arraycopy(bm.skip, 0, ns, 0, bmSk); off = bmSk; }
                if (db >= 0) ns[off++] = (byte) db;
                if (bmChild.skip != null) System.arraycopy(bmChild.skip, 0, ns, off, chSk);
            }
            bmChild.depth = bm.depth;
            bmChild.skip = ns;
        }
        replaceNode(bm, child);
    }

    // === utilities ===
    private void replaceNode(Node old, Node replacement) {
        if (old.parent == null && old == root) {
            root = replacement;
            replacement.parent = null;
            replacement.parentByte = PARENT_ROOT;
        } else if (old.parent != null) {
            BitmaskNode p = old.parent;
            if (old.parentByte == PARENT_EOS) p.setEosChild(replacement);
            else p.replaceChild(old.parentByte, replacement);
        }
    }

    private int countEntries(Node node) {
        if (node == CompactNode.SENTINEL) return 0;
        if (node instanceof CompactNode c) return c.count;
        return ((BitmaskNode) node).descendantCount;
    }

    // === Map interface ===
    @Override public int size() { return size; }
    @Override public boolean isEmpty() { return size == 0; }
    @Override public boolean containsKey(Object key) { return key instanceof Long k && findImpl(toInternal(k)) != null; }
    @Override public V get(Object key) { return key instanceof Long k ? findImpl(toInternal(k)) : null; }
    @Override public V put(Long key, V value) { return putImpl(toInternal(key), value, false); }
    @Override public V remove(Object key) { return key instanceof Long k ? removeImpl(toInternal(k)) : null; }
    @Override public void clear() { root = CompactNode.SENTINEL; size = 0; modCount++; }

    public boolean insertIfAbsent(long key, V value) { int old = size; putImpl(toInternal(key), value, true); return size > old; }

    // === Navigation helpers ===
    private CompactNode descendFirst(Node node) {
        while (node != CompactNode.SENTINEL) {
            if (node instanceof CompactNode c) return c.count > 0 ? c : null;
            BitmaskNode bm = (BitmaskNode) node;
            if (bm.hasEos()) { node = bm.eosChild(); continue; }
            int bit = bm.findNextSet(0);
            if (bit < 0) return null;
            node = bm.children[bm.slotOf(bit)];
        }
        return null;
    }

    private CompactNode descendLast(Node node) {
        while (node != CompactNode.SENTINEL) {
            if (node instanceof CompactNode c) return c.count > 0 ? c : null;
            BitmaskNode bm = (BitmaskNode) node;
            int bit = bm.findPrevSet(255);
            if (bit >= 0) { node = bm.children[bm.slotOf(bit)]; continue; }
            if (bm.hasEos()) { node = bm.eosChild(); continue; }
            return null;
        }
        return null;
    }

    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> entryAt(CompactNode c, int i) {
        if (c == null || c.count == 0 || i < 0 || i >= c.keys.length) return null;
        return new SimpleImmutableEntry<>(fromInternal(c.keys[i]), (V) c.values[i]);
    }

    // === NavigableMap ===
    @Override public Comparator<? super Long> comparator() { return null; }
    @Override public Long firstKey() { CompactNode c = descendFirst(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[0]); }
    @Override public Long lastKey() { CompactNode c = descendLast(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[c.keys.length - 1]); }
    @Override public Entry<Long, V> firstEntry() { return entryAt(descendFirst(root), 0); }
    @Override public Entry<Long, V> lastEntry() { CompactNode c = descendLast(root); return c == null ? null : entryAt(c, c.keys.length - 1); }
    @Override public Entry<Long, V> pollFirstEntry() { var e = firstEntry(); if (e != null) remove(e.getKey()); return e; }
    @Override public Entry<Long, V> pollLastEntry() { var e = lastEntry(); if (e != null) remove(e.getKey()); return e; }

    // lowerEntry/higherEntry: O(N) scan for now — proper descent is follow-up
    @Override public Entry<Long, V> lowerEntry(Long key) {
        Entry<Long, V> r = null;
        for (var e : entrySet()) { if (e.getKey() < key) r = e; else break; }
        return r;
    }
    @Override public Entry<Long, V> higherEntry(Long key) {
        for (var e : entrySet()) if (e.getKey() > key) return e;
        return null;
    }
    @Override public Entry<Long, V> floorEntry(Long key) { V v = findImpl(toInternal(key)); return v != null ? new SimpleImmutableEntry<>(key, v) : lowerEntry(key); }
    @Override public Entry<Long, V> ceilingEntry(Long key) { V v = findImpl(toInternal(key)); return v != null ? new SimpleImmutableEntry<>(key, v) : higherEntry(key); }
    @Override public Long floorKey(Long k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long ceilingKey(Long k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long lowerKey(Long k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long higherKey(Long k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }

    @Override public NavigableMap<Long, V> descendingMap() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableSet<Long> navigableKeySet() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableSet<Long> descendingKeySet() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<Long, V> subMap(Long a, boolean ai, Long b, boolean bi) { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<Long, V> headMap(Long t, boolean i) { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<Long, V> tailMap(Long f, boolean i) { throw new UnsupportedOperationException("TODO"); }
    @Override public SortedMap<Long, V> subMap(Long a, Long b) { return subMap(a, true, b, false); }
    @Override public SortedMap<Long, V> headMap(Long t) { return headMap(t, false); }
    @Override public SortedMap<Long, V> tailMap(Long f) { return tailMap(f, true); }

    // === Iterator ===
    @Override public Set<Entry<Long, V>> entrySet() {
        return new AbstractSet<>() {
            @Override public int size() { return KNTrie.this.size; }
            @Override public Iterator<Entry<Long, V>> iterator() { return new Iter(); }
        };
    }

    private class Iter implements Iterator<Entry<Long, V>> {
        CompactNode leaf;
        int index;
        int expectedModCount;
        long lastKey;
        boolean canRemove;

        Iter() {
            expectedModCount = modCount;
            leaf = descendFirst(root);
            index = 0;
        }

        @Override public boolean hasNext() { return leaf != null && index < leaf.keys.length; }

        @Override @SuppressWarnings("unchecked")
        public Entry<Long, V> next() {
            if (modCount != expectedModCount) throw new ConcurrentModificationException();
            if (!hasNext()) throw new NoSuchElementException();
            long ik = leaf.keys[index];
            V val = (V) leaf.values[index];
            lastKey = ik;
            canRemove = true;
            advance();
            return new SimpleImmutableEntry<>(fromInternal(ik), val);
        }

        private void advance() {
            // Skip past all dups of current key
            index++;
            while (index < leaf.keys.length && leaf.keys[index] == leaf.keys[index - 1]) index++;
            if (index < leaf.keys.length) return;

            // Walk up to next leaf
            Node node = leaf;
            while (node.parent != null) {
                BitmaskNode bm = node.parent;
                int nextBit = (node.parentByte == PARENT_EOS) ? bm.findNextSet(0) : bm.findNextSet(node.parentByte + 1);
                if (nextBit >= 0) {
                    CompactNode target = descendFirst(bm.children[bm.slotOf(nextBit)]);
                    if (target != null) { leaf = target; index = 0; return; }
                }
                node = bm;
            }
            leaf = null; index = 0;
        }

        @Override public void remove() {
            if (!canRemove) throw new IllegalStateException();
            canRemove = false;
            removeImpl(lastKey);
            expectedModCount = modCount;
            // Re-find position: first entry > lastKey
            refindAfter(lastKey);
        }

        private void refindAfter(long afterKey) {
            leaf = descendFirst(root);
            index = 0;
            if (leaf == null) return;
            while (leaf != null) {
                for (; index < leaf.keys.length; index++) {
                    // Skip dups
                    if (index > 0 && leaf.keys[index] == leaf.keys[index - 1]) continue;
                    if (Long.compareUnsigned(leaf.keys[index], afterKey) > 0) return;
                }
                Node node = leaf; leaf = null;
                while (node.parent != null) {
                    BitmaskNode bm = node.parent;
                    int nb = (node.parentByte == PARENT_EOS) ? bm.findNextSet(0) : bm.findNextSet(node.parentByte + 1);
                    if (nb >= 0) { CompactNode t = descendFirst(bm.children[bm.slotOf(nb)]); if (t != null) { leaf = t; index = 0; break; } }
                    node = bm;
                }
            }
        }
    }

    // === Main — smoke test ===
    public static void main(String[] args) {
         
        KNTrie<String> t = new KNTrie<>();

        // Basic CRUD
        t.put(10L, "ten");
        t.put(20L, "twenty");
        t.put(5L, "five");
        t.put(-1L, "minus_one");
        assert t.size() == 4 : "size=" + t.size();
        assert "ten".equals(t.get(10L)) : "get(10)=" + t.get(10L);
        assert t.containsKey(5L);
        assert !t.containsKey(99L);

        // Overwrite
        assert "ten".equals(t.put(10L, "TEN"));
        assert "TEN".equals(t.get(10L));

        // Remove
        assert "five".equals(t.remove(5L));
        assert t.size() == 3;

        // Ordered iteration
        TreeMap<Long, String> ref = new TreeMap<>();
        ref.put(10L, "TEN"); ref.put(20L, "twenty"); ref.put(-1L, "minus_one");
        var ti = t.entrySet().iterator();
        var ri = ref.entrySet().iterator();
        int checked = 0;
        while (ti.hasNext() && ri.hasNext()) {
            var e = ti.next(); var r = ri.next();
            assert e.getKey().equals(r.getKey()) : e.getKey() + " != " + r.getKey();
            assert e.getValue().equals(r.getValue()) : e.getValue() + " != " + r.getValue();
            checked++;
        }
        assert !ti.hasNext() && !ri.hasNext() && checked == 3;
        System.out.println("Basic: PASS (" + checked + " entries verified)"); 

        // NavigableMap
        assert t.firstKey() == -1L;
        assert t.lastKey() == 20L;
        System.out.println("NavigableMap basics: PASS"); 

        // Bulk
        t.clear();
        TreeMap<Long, Integer> ref2 = new TreeMap<>();
        var rng = new java.util.Random(42);
        for (int i = 0; i < 10000; i++) { long k = rng.nextLong(); t.put(k, "v" + i); ref2.put(k, i); }
        assert t.size() == ref2.size() : "size " + t.size() + " != " + ref2.size();

        var tit = t.entrySet().iterator();
        var rit2 = ref2.keySet().iterator();
        checked = 0;
        while (tit.hasNext() && rit2.hasNext()) {
            var te = tit.next(); var rk = rit2.next();
            assert te.getKey().equals(rk) : "key " + te.getKey() + " != " + rk + " at " + checked;
            checked++;
        }
        assert !tit.hasNext() && !rit2.hasNext();
        System.out.println("Bulk 10K: PASS (" + checked + " entries verified)"); 

        // insertIfAbsent
        t.clear();
        assert t.insertIfAbsent(42, "first");
        assert !t.insertIfAbsent(42, "second");
        assert "first".equals(t.get(42L));
        System.out.println("insertIfAbsent: PASS"); 

        // Iterator remove
        t.clear();
        for (int i = 0; i < 100; i++) t.put((long) i, "v" + i);
        var it = t.entrySet().iterator();
        while (it.hasNext()) { var e = it.next(); if (e.getKey() % 2 == 0) it.remove(); }
        assert t.size() == 50;
        assert !t.containsKey(0L);
        assert t.containsKey(1L);
        System.out.println("Iterator.remove: PASS (size=" + t.size() + ")"); 

        System.out.println("\nALL PASS"); 
    }
}
