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
        // Intentional duplication of slotOf() popcount arithmetic for dispatch hot path.
        // dispatch inlines the branchless sentinel-on-miss pattern.
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

        // --- Find nearest dup slot: keys[i] == keys[i+1], scanning outward from ins ---
        // Returns the position of the LEFT element of the dup pair.
        // For small arrays, linear scan. For large, banded outward scan.
        private static final int DUP_SCAN_MAX = 128;

        int findDupPos(int ins) {
            int total = keys.length;
            if (total <= DUP_SCAN_MAX) {
                for (int i = ins; i < total - 1; i++)
                    if (keys[i] == keys[i + 1]) return i;
                for (int i = ins - 1; i >= 1; i--)
                    if (keys[i] == keys[i - 1]) return i;
                return -1;
            }
            // Banded outward scan
            int dups = total - count;
            int band = count / (dups + 1) + 1;
            int rLo = ins, lHi = ins - 1;
            while (true) {
                int rHi = Math.min(rLo + band, total - 1);
                for (int i = rLo; i < rHi; i++)
                    if (keys[i] == keys[i + 1]) return i;
                rLo = rHi;

                int lLo = Math.max(1, lHi - band + 1);
                for (int i = lLo; i <= lHi; i++)
                    if (keys[i] == keys[i - 1]) return i;
                lHi = lLo - 1;
            }
        }

        // --- Insert by consuming one dup slot ---
        // findBase gives position; compute insertion point; find nearest dup;
        // shift small range; write. O(distance_to_nearest_dup).
        boolean insertKey(long ikey, Object val) {
            if (count == 0 && keys.length > 0) {
                // Empty leaf — fill entire array with this entry
                Arrays.fill(keys, ikey);
                Arrays.fill(values, val);
                count = 1;
                return true;
            }

            int pos = findBase(ikey);
            if (keys[pos] == ikey) return false; // duplicate

            // Insertion point: first slot > ikey
            int ins = Long.compareUnsigned(keys[pos], ikey) < 0 ? pos + 1 : pos;

            // Need room? Grow and re-spread to create dups
            if (count >= keys.length) {
                long[] uk = new long[count];
                Object[] uv = new Object[count];
                extractUniques(uk, uv);
                int newSlots = keys.length * 2;
                keys = new long[newSlots];
                values = new Object[newSlots];
                spread(uk, uv, count);
                // Recompute insertion point in new spread
                pos = findBase(ikey);
                ins = Long.compareUnsigned(keys[pos], ikey) < 0 ? pos + 1 : pos;
            }

            // Find nearest expendable dup and shift
            int dp = findDupPos(ins);
            if (dp < ins) {
                // Dup is to the left: shift [dp+1..ins-1] left by 1, write at ins-1
                int sc = ins - 1 - dp;
                if (sc > 0) {
                    System.arraycopy(keys, dp + 1, keys, dp, sc);
                    System.arraycopy(values, dp + 1, values, dp, sc);
                }
                keys[ins - 1] = ikey;
                values[ins - 1] = val;
            } else {
                // Dup is to the right: shift [ins..dp-1] right by 1, write at ins
                int sc = dp - ins;
                if (sc > 0) {
                    System.arraycopy(keys, ins, keys, ins + 1, sc);
                    System.arraycopy(values, ins, values, ins + 1, sc);
                }
                keys[ins] = ikey;
                values[ins] = val;
            }
            count++;
            return true;
        }

        // --- Erase: overwrite key's range with neighbor, creating a dup ---
        // O(range_size) — typically small.
        Object eraseKey(long target) {
            int pos = findBase(target);
            if (keys[pos] != target) return null;
            Object old = values[pos];

            // Find first occurrence of this key
            int first = pos;
            while (first > 0 && keys[first - 1] == target) first--;
            // Find last occurrence
            int last = pos;
            while (last < keys.length - 1 && keys[last + 1] == target) last++;

            // Pick neighbor to fill the gap
            long nk; Object nv;
            if (first > 0) { nk = keys[first - 1]; nv = values[first - 1]; }
            else if (last < keys.length - 1) { nk = keys[last + 1]; nv = values[last + 1]; }
            else { count--; return old; } // sole entry

            for (int i = first; i <= last; i++) { keys[i] = nk; values[i] = nv; }
            count--;
            return old;
        }

        // --- Overwrite all dups of a key with new value ---
        void updateValue(long target, Object newVal) {
            // Dup run is contiguous — scan only the run, not the full array
            int pos = findBase(target);
            if (keys[pos] != target) return;
            values[pos] = newVal;
            for (int i = pos - 1; i >= 0 && keys[i] == target; i--) values[i] = newVal;
            for (int i = pos + 1; i < keys.length && keys[i] == target; i++) values[i] = newVal;
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

    // === Navigation helpers ===
    // First/last entry within a subtree
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> firstEntryIn(Node node) {
        CompactNode c = descendFirst(node);
        return c != null ? entryAt(c, 0) : null;
    }

    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> lastEntryIn(Node node) {
        CompactNode c = descendLast(node);
        return c != null ? entryAt(c, c.keys.length - 1) : null;
    }

    // Next entry after entire subtree rooted at 'node' (walk up, find next sibling)
    private SimpleImmutableEntry<Long, V> nextEntryAfter(Node node) {
        while (node.parent != null) {
            BitmaskNode bm = node.parent;
            int nextBit = (node.parentByte == PARENT_EOS)
                ? bm.findNextSet(0)
                : bm.findNextSet(node.parentByte + 1);
            if (nextBit >= 0) {
                var e = firstEntryIn(bm.children[bm.slotOf(nextBit)]);
                if (e != null) return e;
            }
            node = bm;
        }
        return null;
    }

    // Previous entry before entire subtree rooted at 'node' (walk up, find prev sibling)
    private SimpleImmutableEntry<Long, V> prevEntryBefore(Node node) {
        while (node.parent != null) {
            BitmaskNode bm = node.parent;
            if (node.parentByte == PARENT_EOS) {
                // EOS is first in order — nothing before it at this level
                node = bm;
                continue;
            }
            int prevBit = bm.findPrevSet(node.parentByte - 1);
            if (prevBit >= 0) {
                var e = lastEntryIn(bm.children[bm.slotOf(prevBit)]);
                if (e != null) return e;
            }
            // Check EOS (sorts before all byte children)
            if (bm.hasEos()) {
                var e = lastEntryIn(bm.eosChild());
                if (e != null) return e;
            }
            node = bm;
        }
        return null;
    }

    // === ceilingImpl: first entry >= ikey ===
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> ceilingImpl(long ikey) {
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm, ikey);
            if (mm >= 0) {
                int skipByte = bm.skip[mm] & 0xFF;
                int keyByte = byteAt(ikey, bm.depth + mm);
                if (keyByte < skipByte) return firstEntryIn(bm);
                else return nextEntryAfter(bm);
            }
            int b = byteAt(ikey, bm.effectiveDepth());
            Node child = bm.dispatch(b);
            if (child == CompactNode.SENTINEL) {
                // Bit not set — find next set bit
                int next = bm.findNextSet(b + 1);
                if (next >= 0) return firstEntryIn(bm.children[bm.slotOf(next)]);
                return nextEntryAfter(bm);
            }
            node = child;
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int pos = c.findBase(ikey);
        if (c.keys[pos] == ikey) return entryAt(c, pos);
        // findBase: keys[pos] <= ikey. If keys[pos] < ikey, ceiling is pos+1
        if (Long.compareUnsigned(c.keys[pos], ikey) > 0) return entryAt(c, pos); // all entries > ikey, pos=0
        if (pos + 1 < c.keys.length) return entryAt(c, pos + 1);
        return nextEntryAfter(c);
    }

    // === floorImpl: last entry <= ikey ===
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> floorImpl(long ikey) {
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm, ikey);
            if (mm >= 0) {
                int skipByte = bm.skip[mm] & 0xFF;
                int keyByte = byteAt(ikey, bm.depth + mm);
                if (keyByte > skipByte) return lastEntryIn(bm);
                else return prevEntryBefore(bm);
            }
            int b = byteAt(ikey, bm.effectiveDepth());
            Node child = bm.dispatch(b);
            if (child == CompactNode.SENTINEL) {
                // Bit not set — find prev set bit
                int prev = bm.findPrevSet(b - 1);
                if (prev >= 0) return lastEntryIn(bm.children[bm.slotOf(prev)]);
                if (bm.hasEos()) return lastEntryIn(bm.eosChild());
                return prevEntryBefore(bm);
            }
            node = child;
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int pos = c.findBase(ikey);
        // findBase: keys[pos] is last <= ikey. But if all entries > ikey, pos=0 and keys[0] > ikey
        if (Long.compareUnsigned(c.keys[pos], ikey) > 0) return prevEntryBefore(c);
        return entryAt(c, pos);
    }

    // === higherImpl: first entry > ikey ===
    private SimpleImmutableEntry<Long, V> higherImpl(long ikey) {
        // ceiling of ikey, skip if exact match
        var e = ceilingImpl(ikey);
        if (e != null && toInternal(e.getKey()) == ikey) {
            // Exact match — need next distinct key
            // Find the node, skip past all dups of ikey
            return ceilingAfter(ikey);
        }
        return e;
    }

    // Find first entry with key > ikey (used by higherImpl when ceiling found exact match)
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> ceilingAfter(long ikey) {
        // Descend to the leaf containing ikey, then advance past it
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm, ikey)) return null; // shouldn't happen, key exists
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int pos = c.findBase(ikey);
        // Skip past all dups of ikey
        while (pos < c.keys.length - 1 && c.keys[pos + 1] == ikey) pos++;
        if (pos + 1 < c.keys.length) return entryAt(c, pos + 1);
        return nextEntryAfter(c);
    }

    // === lowerImpl: last entry < ikey ===
    private SimpleImmutableEntry<Long, V> lowerImpl(long ikey) {
        var e = floorImpl(ikey);
        if (e != null && toInternal(e.getKey()) == ikey) {
            return floorBefore(ikey);
        }
        return e;
    }

    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<Long, V> floorBefore(long ikey) {
        Node node = root;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm, ikey)) return null;
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int pos = c.findBase(ikey);
        // Skip backward past all dups of ikey
        while (pos > 0 && c.keys[pos - 1] == ikey) pos--;
        if (pos > 0) return entryAt(c, pos - 1);
        return prevEntryBefore(c);
    }

    // === NavigableMap ===
    @Override public Comparator<? super Long> comparator() { return null; }
    @Override public Long firstKey() { CompactNode c = descendFirst(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[0]); }
    @Override public Long lastKey() { CompactNode c = descendLast(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[c.keys.length - 1]); }
    @Override public Entry<Long, V> firstEntry() { return entryAt(descendFirst(root), 0); }
    @Override public Entry<Long, V> lastEntry() { CompactNode c = descendLast(root); return c == null ? null : entryAt(c, c.keys.length - 1); }
    @Override public Entry<Long, V> pollFirstEntry() { var e = firstEntry(); if (e != null) remove(e.getKey()); return e; }
    @Override public Entry<Long, V> pollLastEntry() { var e = lastEntry(); if (e != null) remove(e.getKey()); return e; }

    @Override public Entry<Long, V> ceilingEntry(Long key) { return ceilingImpl(toInternal(key)); }
    @Override public Entry<Long, V> floorEntry(Long key) { return floorImpl(toInternal(key)); }
    @Override public Entry<Long, V> higherEntry(Long key) { return higherImpl(toInternal(key)); }
    @Override public Entry<Long, V> lowerEntry(Long key) { return lowerImpl(toInternal(key)); }
    @Override public Long floorKey(Long k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long ceilingKey(Long k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long lowerKey(Long k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }
    @Override public Long higherKey(Long k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }

    // === NavigableMap views ===
    @Override public NavigableSet<Long> navigableKeySet() { return new KeySet(this); }
    @Override public NavigableSet<Long> descendingKeySet() { return new KeySet(descendingMap()); }
    @Override public NavigableMap<Long, V> descendingMap() { return new DescendingMap(); }

    // === Descending iterator (spread dups: skip backward) ===
    private class DescIter implements Iterator<Entry<Long, V>> {
        CompactNode leaf;
        int index;
        int expectedModCount;
        long lastKey;
        boolean canRemove;

        DescIter() {
            expectedModCount = modCount;
            leaf = descendLast(root);
            index = (leaf != null) ? leaf.keys.length - 1 : -1;
        }

        @Override public boolean hasNext() { return leaf != null && index >= 0; }

        @Override @SuppressWarnings("unchecked")
        public Entry<Long, V> next() {
            if (modCount != expectedModCount) throw new ConcurrentModificationException();
            if (!hasNext()) throw new NoSuchElementException();
            long ik = leaf.keys[index];
            V val = (V) leaf.values[index];
            lastKey = ik;
            canRemove = true;
            retreat();
            return new SimpleImmutableEntry<>(fromInternal(ik), val);
        }

        private void retreat() {
            index--;
            while (index >= 0 && leaf.keys[index] == leaf.keys[index + 1]) index--;
            if (index >= 0) return;
            // Walk up to previous leaf
            Node node = leaf;
            while (node.parent != null) {
                BitmaskNode bm = node.parent;
                if (node.parentByte == PARENT_EOS) { node = bm; continue; }
                int prevBit = bm.findPrevSet(node.parentByte - 1);
                if (prevBit >= 0) {
                    CompactNode target = descendLast(bm.children[bm.slotOf(prevBit)]);
                    if (target != null) { leaf = target; index = leaf.keys.length - 1; return; }
                }
                if (bm.hasEos()) {
                    CompactNode target = descendLast(bm.eosChild());
                    if (target != null) { leaf = target; index = leaf.keys.length - 1; return; }
                }
                node = bm;
            }
            leaf = null; index = -1;
        }

        @Override public void remove() {
            if (!canRemove) throw new IllegalStateException();
            canRemove = false;
            removeImpl(lastKey);
            expectedModCount = modCount;
            // Refind: position at entry just before lastKey (in descending order = just after in ascending)
            var e = floorImpl(lastKey);
            if (e == null || toInternal(e.getKey()) == lastKey) {
                e = lowerImpl(lastKey);
            }
            if (e == null) { leaf = null; index = -1; return; }
            long ik = toInternal(e.getKey());
            // Descend to that entry
            Node node = root;
            while (node instanceof BitmaskNode bm) {
                if (!matchSkip(bm, ik)) { leaf = null; index = -1; return; }
                node = bm.dispatch(byteAt(ik, bm.effectiveDepth()));
            }
            if (node instanceof CompactNode c && c != CompactNode.SENTINEL) {
                int pos = c.findBase(ik);
                if (c.keys[pos] == ik) { leaf = c; index = pos; }
                else { leaf = null; index = -1; }
            } else { leaf = null; index = -1; }
        }
    }

    // === KeySet: NavigableSet<Long> backed by NavigableMap ===
    private static class KeySet extends AbstractSet<Long> implements NavigableSet<Long> {
        private final NavigableMap<Long, ?> map;
        KeySet(NavigableMap<Long, ?> map) { this.map = map; }
        @Override public int size() { return map.size(); }
        @Override public boolean contains(Object o) { return map.containsKey(o); }
        @Override public Iterator<Long> iterator() {
            var ei = map.entrySet().iterator();
            return new Iterator<>() {
                public boolean hasNext() { return ei.hasNext(); }
                public Long next() { return ei.next().getKey(); }
                public void remove() { ei.remove(); }
            };
        }
        @Override public Long first() { return map.firstKey(); }
        @Override public Long last() { return map.lastKey(); }
        @Override public Long lower(Long e) { return map.lowerKey(e); }
        @Override public Long floor(Long e) { return map.floorKey(e); }
        @Override public Long ceiling(Long e) { return map.ceilingKey(e); }
        @Override public Long higher(Long e) { return map.higherKey(e); }
        @Override public Long pollFirst() { var e = map.pollFirstEntry(); return e == null ? null : e.getKey(); }
        @Override public Long pollLast() { var e = map.pollLastEntry(); return e == null ? null : e.getKey(); }
        @Override public NavigableSet<Long> descendingSet() { return new KeySet(map.descendingMap()); }
        @Override public Iterator<Long> descendingIterator() { return descendingSet().iterator(); }
        @Override public NavigableSet<Long> subSet(Long a, boolean ai, Long b, boolean bi) { return new KeySet(map.subMap(a, ai, b, bi)); }
        @Override public NavigableSet<Long> headSet(Long t, boolean i) { return new KeySet(map.headMap(t, i)); }
        @Override public NavigableSet<Long> tailSet(Long f, boolean i) { return new KeySet(map.tailMap(f, i)); }
        @Override public SortedSet<Long> subSet(Long a, Long b) { return subSet(a, true, b, false); }
        @Override public SortedSet<Long> headSet(Long t) { return headSet(t, false); }
        @Override public SortedSet<Long> tailSet(Long f) { return tailSet(f, true); }
        @Override public Comparator<? super Long> comparator() { return map.comparator(); }
    }

    // === DescendingMap: reversed NavigableMap view ===
    private class DescendingMap extends AbstractMap<Long, V> implements NavigableMap<Long, V> {
        @Override public int size() { return KNTrie.this.size; }
        @Override public boolean containsKey(Object key) { return KNTrie.this.containsKey(key); }
        @Override public V get(Object key) { return KNTrie.this.get(key); }
        @Override public V put(Long key, V value) { return KNTrie.this.put(key, value); }
        @Override public V remove(Object key) { return KNTrie.this.remove(key); }
        @Override public Set<Entry<Long, V>> entrySet() {
            return new AbstractSet<>() {
                @Override public int size() { return KNTrie.this.size; }
                @Override public Iterator<Entry<Long, V>> iterator() { return new DescIter(); }
            };
        }
        @Override public Comparator<? super Long> comparator() { return Comparator.<Long>naturalOrder().reversed(); }
        @Override public Long firstKey() { return KNTrie.this.lastKey(); }
        @Override public Long lastKey() { return KNTrie.this.firstKey(); }
        @Override public Entry<Long, V> firstEntry() { return KNTrie.this.lastEntry(); }
        @Override public Entry<Long, V> lastEntry() { return KNTrie.this.firstEntry(); }
        @Override public Entry<Long, V> pollFirstEntry() { return KNTrie.this.pollLastEntry(); }
        @Override public Entry<Long, V> pollLastEntry() { return KNTrie.this.pollFirstEntry(); }
        @Override public Entry<Long, V> ceilingEntry(Long key) { return KNTrie.this.floorEntry(key); }
        @Override public Entry<Long, V> floorEntry(Long key) { return KNTrie.this.ceilingEntry(key); }
        @Override public Entry<Long, V> higherEntry(Long key) { return KNTrie.this.lowerEntry(key); }
        @Override public Entry<Long, V> lowerEntry(Long key) { return KNTrie.this.higherEntry(key); }
        @Override public Long ceilingKey(Long k) { return KNTrie.this.floorKey(k); }
        @Override public Long floorKey(Long k) { return KNTrie.this.ceilingKey(k); }
        @Override public Long higherKey(Long k) { return KNTrie.this.lowerKey(k); }
        @Override public Long lowerKey(Long k) { return KNTrie.this.higherKey(k); }
        @Override public NavigableMap<Long, V> descendingMap() { return KNTrie.this; }
        @Override public NavigableSet<Long> navigableKeySet() { return new KeySet(this); }
        @Override public NavigableSet<Long> descendingKeySet() { return KNTrie.this.navigableKeySet(); }
        @Override public NavigableMap<Long, V> subMap(Long a, boolean ai, Long b, boolean bi) { return KNTrie.this.subMap(b, bi, a, ai).descendingMap(); }
        @Override public NavigableMap<Long, V> headMap(Long t, boolean i) { return KNTrie.this.tailMap(t, i).descendingMap(); }
        @Override public NavigableMap<Long, V> tailMap(Long f, boolean i) { return KNTrie.this.headMap(f, i).descendingMap(); }
        @Override public SortedMap<Long, V> subMap(Long a, Long b) { return subMap(a, true, b, false); }
        @Override public SortedMap<Long, V> headMap(Long t) { return headMap(t, false); }
        @Override public SortedMap<Long, V> tailMap(Long f) { return tailMap(f, true); }
    }

    @Override public NavigableMap<Long, V> subMap(Long fromKey, boolean fromInclusive, Long toKey, boolean toInclusive) {
        return new SubMap(fromKey, fromInclusive, toKey, toInclusive);
    }
    @Override public NavigableMap<Long, V> headMap(Long toKey, boolean inclusive) {
        return new SubMap(null, true, toKey, inclusive);
    }
    @Override public NavigableMap<Long, V> tailMap(Long fromKey, boolean inclusive) {
        return new SubMap(fromKey, inclusive, null, true);
    }
    @Override public SortedMap<Long, V> subMap(Long a, Long b) { return subMap(a, true, b, false); }
    @Override public SortedMap<Long, V> headMap(Long t) { return headMap(t, false); }
    @Override public SortedMap<Long, V> tailMap(Long f) { return tailMap(f, true); }

    // === SubMap view ===
    private class SubMap extends AbstractMap<Long, V> implements NavigableMap<Long, V> {
        final Long fromKey, toKey;
        final boolean fromInclusive, toInclusive;

        SubMap(Long from, boolean fi, Long to, boolean ti) {
            this.fromKey = from; this.fromInclusive = fi;
            this.toKey = to; this.toInclusive = ti;
        }

        private boolean inRange(Long key) {
            if (fromKey != null) {
                int c = Long.compare(key, fromKey);
                if (c < 0 || (c == 0 && !fromInclusive)) return false;
            }
            if (toKey != null) {
                int c = Long.compare(key, toKey);
                if (c > 0 || (c == 0 && !toInclusive)) return false;
            }
            return true;
        }

        @Override public V get(Object key) {
            if (!(key instanceof Long k) || !inRange(k)) return null;
            return KNTrie.this.get(k);
        }

        @Override public V put(Long key, V value) {
            if (!inRange(key)) throw new IllegalArgumentException("key out of range");
            return KNTrie.this.put(key, value);
        }

        @Override public int size() {
            int n = 0;
            for (var e : entrySet()) n++;
            return n;
        }

        @Override public boolean containsKey(Object key) {
            return key instanceof Long k && inRange(k) && KNTrie.this.containsKey(k);
        }

        // Starting entry for iteration
        private Entry<Long, V> loEntry() {
            if (fromKey == null) return KNTrie.this.firstEntry();
            return fromInclusive ? KNTrie.this.ceilingEntry(fromKey) : KNTrie.this.higherEntry(fromKey);
        }

        @Override public Set<Entry<Long, V>> entrySet() {
            return new AbstractSet<>() {
                @Override public int size() { return SubMap.this.size(); }
                @Override public Iterator<Entry<Long, V>> iterator() {
                    return new Iterator<>() {
                        Entry<Long, V> next = loEntry();
                        { if (next != null && !inRange(next.getKey())) next = null; }

                        @Override public boolean hasNext() { return next != null; }
                        @Override public Entry<Long, V> next() {
                            if (next == null) throw new NoSuchElementException();
                            Entry<Long, V> r = next;
                            next = KNTrie.this.higherEntry(r.getKey());
                            if (next != null && !inRange(next.getKey())) next = null;
                            return r;
                        }
                    };
                }
            };
        }

        // NavigableMap methods delegate to parent with range checks
        @Override public Comparator<? super Long> comparator() { return null; }
        @Override public Long firstKey() { var e = loEntry(); if (e == null || !inRange(e.getKey())) throw new NoSuchElementException(); return e.getKey(); }
        @Override public Long lastKey() {
            Entry<Long, V> e = toKey != null ? (toInclusive ? KNTrie.this.floorEntry(toKey) : KNTrie.this.lowerEntry(toKey)) : KNTrie.this.lastEntry();
            if (e == null || !inRange(e.getKey())) throw new NoSuchElementException();
            return e.getKey();
        }
        @Override public Entry<Long, V> firstEntry() { var e = loEntry(); return e != null && inRange(e.getKey()) ? e : null; }
        @Override public Entry<Long, V> lastEntry() {
            Entry<Long, V> e = toKey != null ? (toInclusive ? KNTrie.this.floorEntry(toKey) : KNTrie.this.lowerEntry(toKey)) : KNTrie.this.lastEntry();
            return e != null && inRange(e.getKey()) ? e : null;
        }
        @Override public Entry<Long, V> pollFirstEntry() { var e = firstEntry(); if (e != null) KNTrie.this.remove(e.getKey()); return e; }
        @Override public Entry<Long, V> pollLastEntry() { var e = lastEntry(); if (e != null) KNTrie.this.remove(e.getKey()); return e; }

        @Override public Entry<Long, V> ceilingEntry(Long key) { var e = KNTrie.this.ceilingEntry(key); return e != null && inRange(e.getKey()) ? e : null; }
        @Override public Entry<Long, V> floorEntry(Long key) { var e = KNTrie.this.floorEntry(key); return e != null && inRange(e.getKey()) ? e : null; }
        @Override public Entry<Long, V> higherEntry(Long key) { var e = KNTrie.this.higherEntry(key); return e != null && inRange(e.getKey()) ? e : null; }
        @Override public Entry<Long, V> lowerEntry(Long key) { var e = KNTrie.this.lowerEntry(key); return e != null && inRange(e.getKey()) ? e : null; }
        @Override public Long ceilingKey(Long k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
        @Override public Long floorKey(Long k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
        @Override public Long higherKey(Long k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }
        @Override public Long lowerKey(Long k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }

        @Override public NavigableMap<Long, V> subMap(Long a, boolean ai, Long b, boolean bi) {
            if (fromKey != null && Long.compare(a, fromKey) < 0) throw new IllegalArgumentException("fromKey out of range");
            if (toKey != null && Long.compare(b, toKey) > 0) throw new IllegalArgumentException("toKey out of range");
            return KNTrie.this.subMap(a, ai, b, bi);
        }
        @Override public NavigableMap<Long, V> headMap(Long t, boolean i) {
            if (toKey != null && Long.compare(t, toKey) > 0) throw new IllegalArgumentException("toKey out of range");
            return KNTrie.this.subMap(fromKey != null ? fromKey : Long.MIN_VALUE, fromInclusive, t, i);
        }
        @Override public NavigableMap<Long, V> tailMap(Long f, boolean i) {
            if (fromKey != null && Long.compare(f, fromKey) < 0) throw new IllegalArgumentException("fromKey out of range");
            return KNTrie.this.subMap(f, i, toKey != null ? toKey : Long.MAX_VALUE, toInclusive);
        }
        @Override public SortedMap<Long, V> subMap(Long a, Long b) { return subMap(a, true, b, false); }
        @Override public SortedMap<Long, V> headMap(Long t) { return headMap(t, false); }
        @Override public SortedMap<Long, V> tailMap(Long f) { return tailMap(f, true); }
        @Override public NavigableSet<Long> navigableKeySet() { return new KeySet(this); }
        @Override public NavigableSet<Long> descendingKeySet() { return new KeySet(descendingMap()); }
        @Override public NavigableMap<Long, V> descendingMap() {
            return KNTrie.this.descendingMap().subMap(
                toKey != null ? toKey : Long.MAX_VALUE, toInclusive,
                fromKey != null ? fromKey : Long.MIN_VALUE, fromInclusive);
        }
    }

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

        // Navigation: floor/ceiling/lower/higher
        t.clear();
        for (int i = 0; i < 100; i += 10) t.put((long) i, "v" + i);
        // Keys: 0, 10, 20, 30, 40, 50, 60, 70, 80, 90

        // ceiling
        assert t.ceilingKey(0L) == 0L;
        assert t.ceilingKey(5L) == 10L;
        assert t.ceilingKey(10L) == 10L;
        assert t.ceilingKey(90L) == 90L;
        assert t.ceilingKey(91L) == null;

        // floor
        assert t.floorKey(0L) == 0L;
        assert t.floorKey(5L) == 0L;
        assert t.floorKey(10L) == 10L;
        assert t.floorKey(90L) == 90L;
        assert t.floorKey(-1L) == null;

        // higher
        assert t.higherKey(0L) == 10L;
        assert t.higherKey(5L) == 10L;
        assert t.higherKey(89L) == 90L;
        assert t.higherKey(90L) == null;

        // lower
        assert t.lowerKey(0L) == null;
        assert t.lowerKey(10L) == 0L;
        assert t.lowerKey(15L) == 10L;
        assert t.lowerKey(91L) == 90L;

        System.out.println("Navigation (floor/ceil/lower/higher): PASS");

        // Verify against TreeMap at scale
        t.clear();
        TreeMap<Long, String> navRef = new TreeMap<>();
        var navRng = new java.util.Random(123);
        for (int i = 0; i < 5000; i++) {
            long k = navRng.nextLong();
            t.put(k, "v" + i); navRef.put(k, "v" + i);
        }
        navRng = new java.util.Random(456);
        int navChecked = 0;
        for (int i = 0; i < 1000; i++) {
            long probe = navRng.nextLong();
            Long tc = t.ceilingKey(probe), rc = navRef.ceilingKey(probe);
            assert Objects.equals(tc, rc) : "ceiling(" + probe + "): " + tc + " != " + rc;
            Long tf = t.floorKey(probe), rf = navRef.floorKey(probe);
            assert Objects.equals(tf, rf) : "floor(" + probe + "): " + tf + " != " + rf;
            Long th = t.higherKey(probe), rh = navRef.higherKey(probe);
            assert Objects.equals(th, rh) : "higher(" + probe + "): " + th + " != " + rh;
            Long tl = t.lowerKey(probe), rl = navRef.lowerKey(probe);
            assert Objects.equals(tl, rl) : "lower(" + probe + "): " + tl + " != " + rl;
            navChecked++;
        }
        System.out.println("Navigation vs TreeMap: PASS (" + navChecked + " probes)");

        // SubMap view
        t.clear();
        for (int i = 0; i < 100; i++) t.put((long) i, "v" + i);
        var sub = t.subMap(20L, true, 50L, false);
        assert sub.size() == 30 : "subMap size=" + sub.size();
        assert sub.firstKey() == 20L;
        assert sub.lastKey() == 49L;
        assert sub.containsKey(20L);
        assert sub.containsKey(49L);
        assert !sub.containsKey(50L);
        assert !sub.containsKey(19L);

        var head = t.headMap(10L, false);
        assert head.size() == 10;
        assert head.firstKey() == 0L;
        assert head.lastKey() == 9L;

        var tail = t.tailMap(90L, true);
        assert tail.size() == 10;
        assert tail.firstKey() == 90L;
        assert tail.lastKey() == 99L;
        System.out.println("SubMap/HeadMap/TailMap: PASS");

        // Descending map
        t.clear();
        t.put(10L, "ten"); t.put(20L, "twenty"); t.put(30L, "thirty");
        var desc = t.descendingMap();
        assert desc.firstKey() == 30L;
        assert desc.lastKey() == 10L;
        var descIt = desc.entrySet().iterator();
        assert descIt.next().getKey() == 30L;
        assert descIt.next().getKey() == 20L;
        assert descIt.next().getKey() == 10L;
        assert !descIt.hasNext();
        assert desc.descendingMap() == t;
        System.out.println("DescendingMap: PASS");

        // NavigableKeySet
        var ks = t.navigableKeySet();
        assert ks.size() == 3;
        assert ks.first() == 10L;
        assert ks.last() == 30L;
        assert ks.contains(20L);
        assert !ks.contains(15L);
        assert ks.ceiling(15L) == 20L;
        assert ks.floor(15L) == 10L;
        System.out.println("NavigableKeySet: PASS");

        // DescendingKeySet
        var dks = t.descendingKeySet();
        var dksIt = dks.iterator();
        assert dksIt.next() == 30L;
        assert dksIt.next() == 20L;
        assert dksIt.next() == 10L;
        assert !dksIt.hasNext();
        System.out.println("DescendingKeySet: PASS");

        // Descending at scale vs TreeMap
        t.clear();
        TreeMap<Long, String> descRef = new TreeMap<>();
        var descRng = new java.util.Random(555);
        for (int i = 0; i < 5000; i++) { long k = descRng.nextLong(); t.put(k, "v" + i); descRef.put(k, "v" + i); }
        var descTrieIt = t.descendingMap().entrySet().iterator();
        var descRefIt = descRef.descendingMap().entrySet().iterator();
        int descChecked = 0;
        while (descTrieIt.hasNext() && descRefIt.hasNext()) {
            var te = descTrieIt.next(); var re = descRefIt.next();
            assert te.getKey().equals(re.getKey()) : "desc key " + te.getKey() + " != " + re.getKey() + " at " + descChecked;
            descChecked++;
        }
        assert !descTrieIt.hasNext() && !descRefIt.hasNext();
        System.out.println("Descending vs TreeMap: PASS (" + descChecked + " entries)");

        System.out.println("\nALL PASS"); 
    }
}
