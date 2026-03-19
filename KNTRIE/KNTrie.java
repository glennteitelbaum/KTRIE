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
        int depth;
        byte[] skip;
        int skipLen() { return skip == null ? 0 : skip.length; }
        int effectiveDepth() { return depth + skipLen(); }
    }

    static final class BitmaskNode extends Node {
        long b0, b1, b2, b3;
        Node[] children;
        int descendantCount;

        long word(int w) {
            return switch (w) { case 0 -> b0; case 1 -> b1; case 2 -> b2; default -> b3; };
        }
        void setWord(int w, long val) {
            switch (w) { case 0 -> b0 = val; case 1 -> b1 = val; case 2 -> b2 = val; default -> b3 = val; }
        }
        boolean hasBit(int idx) { return (word(idx >> 6) & (1L << (idx & 63))) != 0; }
        void setBit(int idx) { int w = idx >> 6; setWord(w, word(w) | (1L << (idx & 63))); }
        void clearBit(int idx) { int w = idx >> 6; setWord(w, word(w) & ~(1L << (idx & 63))); }

        int countBelow(int idx) {
            int w = idx >> 6, b = idx & 63;
            long mask = (b == 0) ? 0 : ((-1L) >>> (64 - b));
            int n = Long.bitCount(word(w) & mask);
            if (w > 0) n += Long.bitCount(b0);
            if (w > 1) n += Long.bitCount(b1);
            if (w > 2) n += Long.bitCount(b2);
            return n;
        }

        int slotOf(int idx) { return countBelow(idx) + 1; }

        Node dispatch(int idx) {
            int w = idx >> 6, b = idx & 63;
            long shifted = word(w) << (63 - b);
            boolean hit = (shifted & Long.MIN_VALUE) != 0;
            if (!hit) return children[0];
            int slot = Long.bitCount(shifted);
            if (w > 0) slot += Long.bitCount(b0);
            if (w > 1) slot += Long.bitCount(b1);
            if (w > 2) slot += Long.bitCount(b2);
            return children[slot];
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
        long[] keys;
        Object[] values;
        int count;

        static final CompactNode SENTINEL = new CompactNode();
        private CompactNode() { keys = new long[0]; values = new Object[0]; count = 0; }

        CompactNode(int capacity) {
            capacity = Math.max(MIN_CAPACITY, Integer.highestOneBit(Math.max(capacity - 1, 1)) << 1);
            keys = new long[capacity];
            values = new Object[capacity];
            count = 0;
        }

        int bsearch(long target) {
            int lo = 0, hi = count;
            while (lo < hi) {
                int mid = (lo + hi) >>> 1;
                int cmp = Long.compareUnsigned(keys[mid], target);
                if (cmp < 0) lo = mid + 1;
                else if (cmp > 0) hi = mid;
                else return mid;
            }
            return -(lo + 1);
        }

        void fillDups() {
            if (count > 0 && count < keys.length) {
                Arrays.fill(keys, count, keys.length, keys[count - 1]);
                Arrays.fill(values, count, keys.length, values[count - 1]);
            }
        }

        void grow() {
            int nc = Math.max(MIN_CAPACITY, keys.length * 2);
            keys = Arrays.copyOf(keys, nc);
            values = Arrays.copyOf(values, nc);
        }

        void maybeShrink() {
            if (count > 0 && keys.length > MIN_CAPACITY && count <= keys.length / 4) {
                int nc = Math.max(MIN_CAPACITY, Integer.highestOneBit(count) << 1);
                if (nc < keys.length) {
                    keys = Arrays.copyOf(keys, nc);
                    values = Arrays.copyOf(values, nc);
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

    static boolean matchSkip(Node node, long ikey) {
        if (node.skip == null) return true;
        for (int i = 0; i < node.skip.length; i++)
            if (byteAt(ikey, node.depth + i) != (node.skip[i] & 0xFF)) return false;
        return true;
    }

    // Returns mismatch index, or -1 if fully matched
    static int skipMismatch(Node node, long ikey) {
        if (node.skip == null) return -1;
        for (int i = 0; i < node.skip.length; i++)
            if (byteAt(ikey, node.depth + i) != (node.skip[i] & 0xFF)) return i;
        return -1;
    }

    // === find ===
    @SuppressWarnings("unchecked")
    private V findImpl(long ikey) {
        Node node = root;
        while (node != CompactNode.SENTINEL) {
            if (!matchSkip(node, ikey)) return null;
            if (node instanceof CompactNode c) {
                int pos = c.bsearch(ikey);
                return pos >= 0 ? (V) c.values[pos] : null;
            }
            BitmaskNode bm = (BitmaskNode) node;
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        return null;
    }

    // === put / insert ===
    @SuppressWarnings("unchecked")
    private V putImpl(long ikey, V value, boolean onlyIfAbsent) {
        if (root == CompactNode.SENTINEL) {
            CompactNode c = new CompactNode(MIN_CAPACITY);
            c.depth = 0;
            c.skip = extractSkip(ikey, 0, KEY_BYTES);
            c.keys[0] = ikey; c.values[0] = value; c.count = 1;
            c.fillDups();
            root = c;
            size++; modCount++;
            return null;
        }
        Node node = root;
        for (;;) {
            int mm = skipMismatch(node, ikey);
            if (mm >= 0) {
                splitSkip(node, ikey, value, mm);
                size++; modCount++;
                return null;
            }
            if (node instanceof CompactNode c) {
                int pos = c.bsearch(ikey);
                if (pos >= 0) {
                    if (onlyIfAbsent) return (V) c.values[pos];
                    V old = (V) c.values[pos];
                    updateDups(c, pos, value);
                    modCount++;
                    return old;
                }
                int ins = -(pos + 1);
                if (c.count >= c.keys.length) c.grow();
                System.arraycopy(c.keys, ins, c.keys, ins + 1, c.count - ins);
                System.arraycopy(c.values, ins, c.values, ins + 1, c.count - ins);
                c.keys[ins] = ikey; c.values[ins] = value; c.count++;
                c.fillDups();
                for (BitmaskNode p = c.parent; p != null; p = p.parent) p.descendantCount++;
                if (c.count > COMPACT_MAX) split(c);
                size++; modCount++;
                return null;
            }
            BitmaskNode bm = (BitmaskNode) node;
            int b = byteAt(ikey, bm.effectiveDepth());
            if (!bm.hasBit(b)) {
                CompactNode child = new CompactNode(MIN_CAPACITY);
                child.depth = bm.effectiveDepth() + 1;
                child.skip = extractSkip(ikey, child.depth, KEY_BYTES - child.depth);
                child.keys[0] = ikey; child.values[0] = value; child.count = 1;
                child.fillDups();
                bm.insertChild(b, child);
                bm.descendantCount++;
                for (BitmaskNode p = bm.parent; p != null; p = p.parent) p.descendantCount++;
                size++; modCount++;
                return null;
            }
            node = bm.children[bm.slotOf(b)];
        }
    }

    private void updateDups(CompactNode c, int pos, Object value) {
        long k = c.keys[pos];
        c.values[pos] = value;
        for (int j = pos - 1; j >= 0 && c.keys[j] == k; j--) c.values[j] = value;
        for (int j = pos + 1; j < c.keys.length && c.keys[j] == k; j++) c.values[j] = value;
    }

    // === remove ===
    @SuppressWarnings("unchecked")
    private V removeImpl(long ikey) {
        Node node = root;
        while (node != CompactNode.SENTINEL) {
            if (!matchSkip(node, ikey)) return null;
            if (node instanceof CompactNode c) {
                int pos = c.bsearch(ikey);
                if (pos < 0) return null;
                V old = (V) c.values[pos];
                System.arraycopy(c.keys, pos + 1, c.keys, pos, c.count - pos - 1);
                System.arraycopy(c.values, pos + 1, c.values, pos, c.count - pos - 1);
                c.count--;
                c.values[c.count] = null;
                c.maybeShrink();
                if (c.count > 0) c.fillDups();
                for (BitmaskNode p = c.parent; p != null; p = p.parent) p.descendantCount--;
                if (c.count == 0) removeEmptyLeaf(c);
                else checkCoalesce(c.parent);
                size--; modCount++;
                return old;
            }
            BitmaskNode bm = (BitmaskNode) node;
            node = bm.dispatch(byteAt(ikey, bm.effectiveDepth()));
        }
        return null;
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
        int d = leaf.effectiveDepth();
        if (d >= KEY_BYTES) return;

        // Group by byte at depth d
        int[] groupStart = new int[256];
        int[] groupCount = new int[256];
        Arrays.fill(groupStart, -1);
        for (int i = 0; i < leaf.count; i++) {
            int b = byteAt(leaf.keys[i], d);
            if (groupStart[b] < 0) groupStart[b] = i;
            groupCount[b]++;
        }

        BitmaskNode bm = new BitmaskNode();
        bm.depth = leaf.depth;
        bm.skip = leaf.skip;
        bm.descendantCount = leaf.count;

        int cc = 0;
        for (int b = 0; b < 256; b++) if (groupCount[b] > 0) cc++;
        bm.children = new Node[cc + 2];
        bm.children[0] = CompactNode.SENTINEL;
        bm.children[cc + 1] = CompactNode.SENTINEL;

        int slot = 1;
        for (int b = 0; b < 256; b++) {
            if (groupCount[b] == 0) continue;
            bm.setBit(b);
            int gc = groupCount[b];
            CompactNode child = new CompactNode(gc);
            child.depth = d + 1;
            child.parent = bm;
            child.parentByte = b;
            int gs = groupStart[b];
            System.arraycopy(leaf.keys, gs, child.keys, 0, gc);
            System.arraycopy(leaf.values, gs, child.values, 0, gc);
            child.count = gc;
            computeChildSkip(child);
            child.fillDups();
            bm.children[slot++] = child;
        }
        replaceNode(leaf, bm);
        // Recurse for oversize children
        for (int i = 1; i < bm.children.length - 1; i++)
            if (bm.children[i] instanceof CompactNode c && c.count > COMPACT_MAX) split(c);
    }

    private void computeChildSkip(CompactNode c) {
        if (c.count <= 1) {
            int maxSkip = KEY_BYTES - c.depth;
            if (maxSkip > 0 && c.count == 1) c.skip = extractSkip(c.keys[0], c.depth, maxSkip);
            return;
        }
        int maxSkip = KEY_BYTES - c.depth;
        int common = maxSkip;
        for (int j = 1; j < c.count && common > 0; j++)
            for (int k = 0; k < common; k++)
                if (byteAt(c.keys[0], c.depth + k) != byteAt(c.keys[j], c.depth + k)) { common = k; break; }
        if (common > 0) c.skip = extractSkip(c.keys[0], c.depth, common);
    }

    // === splitSkip ===
    private void splitSkip(Node node, long ikey, V value, int mm) {
        int splitDepth = node.depth + mm;
        BitmaskNode bm = new BitmaskNode();
        bm.depth = node.depth;
        bm.skip = mm > 0 ? extractSkip(ikey, node.depth, mm) : null;
        bm.descendantCount = countEntries(node) + 1;

        int existByte = node.skip[mm] & 0xFF;
        int newByte = byteAt(ikey, splitDepth);

        // Fix existing node's skip
        int remaining = node.skip.length - mm - 1;
        node.skip = remaining > 0 ? Arrays.copyOfRange(node.skip, mm + 1, node.skip.length) : null;
        node.depth = splitDepth + 1;

        // New leaf
        CompactNode nl = new CompactNode(MIN_CAPACITY);
        nl.depth = splitDepth + 1;
        nl.skip = extractSkip(ikey, nl.depth, KEY_BYTES - nl.depth);
        nl.keys[0] = ikey; nl.values[0] = value; nl.count = 1;
        nl.fillDups();

        int cc = 2;
        bm.children = new Node[cc + 2];
        bm.children[0] = CompactNode.SENTINEL;
        bm.children[cc + 1] = CompactNode.SENTINEL;
        bm.setBit(existByte);
        bm.setBit(newByte);
        if (existByte < newByte) { bm.children[1] = node; bm.children[2] = nl; }
        else { bm.children[1] = nl; bm.children[2] = node; }
        node.parent = bm; node.parentByte = existByte;
        nl.parent = bm; nl.parentByte = newByte;
        replaceNode(node, bm);
    }

    // === coalesce: bitmask → compact ===
    private void coalesce(BitmaskNode bm) {
        List<long[]> ak = new ArrayList<>();
        List<Object[]> av = new ArrayList<>();
        collectEntries(bm, ak, av);
        int total = ak.size();
        if (total == 0) { replaceNode(bm, CompactNode.SENTINEL); return; }
        CompactNode c = new CompactNode(total);
        c.depth = bm.depth;
        c.skip = bm.skip;
        for (int i = 0; i < total; i++) { c.keys[i] = ak.get(i)[0]; c.values[i] = av.get(i)[0]; }
        c.count = total;
        c.fillDups();
        replaceNode(bm, c);
    }

    private void collectEntries(Node node, List<long[]> keys, List<Object[]> vals) {
        if (node == CompactNode.SENTINEL) return;
        if (node instanceof CompactNode c) {
            for (int i = 0; i < c.count; i++) {
                keys.add(new long[]{c.keys[i]});
                vals.add(new Object[]{c.values[i]});
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

        int bmSk = bm.skipLen(), chSk = child.skipLen(), dispB = db >= 0 ? 1 : 0;
        int newLen = bmSk + dispB + chSk;
        byte[] ns = null;
        if (newLen > 0) {
            ns = new byte[newLen]; int off = 0;
            if (bm.skip != null) { System.arraycopy(bm.skip, 0, ns, 0, bmSk); off = bmSk; }
            if (db >= 0) ns[off++] = (byte) db;
            if (child.skip != null) System.arraycopy(child.skip, 0, ns, off, chSk);
        }
        child.depth = bm.depth;
        child.skip = ns;
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
        if (c == null || i < 0 || i >= c.count) return null;
        return new SimpleImmutableEntry<>(fromInternal(c.keys[i]), (V) c.values[i]);
    }

    // === NavigableMap ===
    @Override public Comparator<? super Long> comparator() { return null; }
    @Override public Long firstKey() { CompactNode c = descendFirst(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[0]); }
    @Override public Long lastKey() { CompactNode c = descendLast(root); if (c == null) throw new NoSuchElementException(); return fromInternal(c.keys[c.count - 1]); }
    @Override public Entry<Long, V> firstEntry() { return entryAt(descendFirst(root), 0); }
    @Override public Entry<Long, V> lastEntry() { CompactNode c = descendLast(root); return c == null ? null : entryAt(c, c.count - 1); }
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

        @Override public boolean hasNext() { return leaf != null && index < leaf.count; }

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
            long curKey = leaf.keys[index];
            index++;
            while (index < leaf.count && leaf.keys[index] == curKey) index++;
            if (index < leaf.count) return;

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
            leaf = descendFirst(root);
            index = 0;
            if (leaf == null) return;
            while (leaf != null) {
                for (; index < leaf.count; index++)
                    if (Long.compareUnsigned(leaf.keys[index], lastKey) > 0) return;
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
        assert "ten".equals(t.get(10L));
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
