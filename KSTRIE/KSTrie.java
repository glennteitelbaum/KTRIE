// KSTrie.java — ordered string-keyed associative container
// Trie/B-tree hybrid implementing NavigableMap<String, V>
// with keysuffix sharing and prefix operations.

import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.*;

public class KSTrie<V> extends AbstractMap<String, V>
                       implements NavigableMap<String, V> {

    // === Constants ===
    private static final int COMPACT_KEYSUFFIX_LIMIT = 4096;
    private static final int PARENT_ROOT = -1;
    private static final int PARENT_EOS = -2;

    private static final byte[] NO_SKIP = new byte[0];

    // === Node hierarchy ===
    static abstract sealed class Node permits BitmaskNode, CompactNode {
        BitmaskNode parent;
        int parentByte = PARENT_ROOT;
    }

    static final class BitmaskNode extends Node {
        byte[] skip = NO_SKIP;
        long b0, b1, b2, b3;
        Node[] children;        // [0]=sentinel, [1..N]=real, [last]=EOS
        long totalTailBytes;

        int skipLen() { return skip.length; }
        Node eosChild() { return children[children.length - 1]; }
        boolean hasEos() { return eosChild() != CompactNode.SENTINEL; }

        void setEosChild(Node c) {
            children[children.length - 1] = c;
            if (c != CompactNode.SENTINEL) { c.parent = this; c.parentByte = PARENT_EOS; }
        }

        // --- Bitmap ---
        long word(int w) { return switch (w) { case 0 -> b0; case 1 -> b1; case 2 -> b2; default -> b3; }; }
        void setWord(int w, long v) { switch (w) { case 0 -> b0 = v; case 1 -> b1 = v; case 2 -> b2 = v; default -> b3 = v; } }
        boolean hasBit(int idx) { return (word(idx >> 6) & (1L << (idx & 63))) != 0; }
        void setBit(int idx) { int w = idx >> 6; setWord(w, word(w) | (1L << (idx & 63))); }
        void clearBit(int idx) { int w = idx >> 6; setWord(w, word(w) & ~(1L << (idx & 63))); }

        int slotOf(int idx) {
            int w = idx >> 6, b = idx & 63;
            long shifted = word(w) << (63 - b);
            int slot = Long.bitCount(shifted);
            if (w > 0) slot += Long.bitCount(b0);
            if (w > 1) slot += Long.bitCount(b1);
            if (w > 2) slot += Long.bitCount(b2);
            return slot;
        }

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

        int childCount() { return Long.bitCount(b0) + Long.bitCount(b1) + Long.bitCount(b2) + Long.bitCount(b3); }

        int findNextSet(int start) {
            for (int w = start >> 6; w < 4; w++) {
                long masked = word(w) & ((-1L) << (Math.max(start, w << 6) & 63));
                if (masked != 0) return (w << 6) + Long.numberOfTrailingZeros(masked);
                start = (w + 1) << 6;
            }
            return -1;
        }

        int findPrevSet(int start) {
            for (int w = Math.min(start >> 6, 3); w >= 0; w--) {
                int bit = Math.min(start, (w << 6) + 63) & 63;
                long masked = word(w) & ((-1L) >>> (63 - bit));
                if (masked != 0) return (w << 6) + 63 - Long.numberOfLeadingZeros(masked);
                start = (w << 6) - 1;
            }
            return -1;
        }

        void insertChild(int idx, Node child) {
            setBit(idx);
            int slot = slotOf(idx);
            int oldLen = children.length - 1; // exclude EOS
            Node eos = eosChild();
            Node[] nc = new Node[children.length + 1];
            System.arraycopy(children, 0, nc, 0, slot);
            nc[slot] = child;
            System.arraycopy(children, slot, nc, slot + 1, oldLen - slot);
            nc[nc.length - 1] = eos;
            children = nc;
            child.parent = this; child.parentByte = idx;
        }

        void removeChild(int idx) {
            int slot = slotOf(idx);
            clearBit(idx);
            Node eos = eosChild();
            Node[] nc = new Node[children.length - 1];
            System.arraycopy(children, 0, nc, 0, slot);
            System.arraycopy(children, slot + 1, nc, slot, children.length - 1 - slot - 1);
            nc[nc.length - 1] = eos;
            children = nc;
        }

        void replaceChild(int idx, Node nc) {
            children[slotOf(idx)] = nc;
            nc.parent = this; nc.parentByte = idx;
        }
    }

    // === CompactNode ===
    @SuppressWarnings("rawtypes")
    static final class CompactNode extends Node {
        byte[] skip = NO_SKIP;
        byte[] data;
        Object[] values;
        int count;
        int capacity;
        int ksUsed;

        static final CompactNode SENTINEL = new CompactNode();
        private static final int MIN_CAPACITY = 4;
        private static final int GROW_NUMER = 3;
        private static final int GROW_DENOM = 2;
        private static final int SHRINK_FACTOR = 2;
        private static final int INITIAL_KS_HEADROOM = 16;

        private CompactNode() { data = new byte[0]; values = new Object[0]; count = 0; capacity = 0; ksUsed = 0; }
        CompactNode(int dummy) { this(); }

        int skipLen() { return skip.length; }

        // --- Data accessors (use capacity for offsets) ---
        byte F(int i) { return data[i]; }
        int L(int i) { return data[capacity + i] & 0xFF; }
        int O(int i) { int base = capacity * 2 + i * 2; return ((data[base] & 0xFF) << 8) | (data[base + 1] & 0xFF); }
        int ksOff() { return capacity * 4; }

        // Suffix comparison
        int compareSuffix(byte[] key, int consumed, int i) {
            int klen = L(i);
            int keyRem = key.length - consumed;
            if (klen == 0) return keyRem == 0 ? 0 : 1;
            if (keyRem == 0) return -1;
            int fb = F(i) & 0xFF;
            int kb = key[consumed] & 0xFF;
            if (kb != fb) return kb - fb;
            int tail = klen - 1;
            int keyTail = keyRem - 1;
            int off = ksOff() + O(i);
            int cmpLen = Math.min(tail, keyTail);
            for (int j = 0; j < cmpLen; j++) {
                int a = key[consumed + 1 + j] & 0xFF;
                int b = data[off + j] & 0xFF;
                if (a != b) return a - b;
            }
            return keyTail - tail;
        }

        // Single binary search (F + suffix compare in one loop)
        int[] findPos(byte[] key, int consumed) {
            int slen = key.length - consumed;
            boolean hasEos = (count > 0 && L(0) == 0);
            if (slen == 0) return new int[]{ hasEos ? 1 : 0, 0 };
            int fb = key[consumed] & 0xFF;
            int stail = slen - 1;
            int lo = hasEos ? 1 : 0;
            int hi = count;
            while (lo < hi) {
                int m = lo + ((hi - lo) >> 1);
                int lm = L(m);
                int c = fb - (F(m) & 0xFF);
                if (c == 0) {
                    int mtail = lm - 1;
                    int minTail = Math.min(stail, mtail);
                    int off = ksOff() + O(m);
                    for (int j = 0; j < minTail; j++) {
                        int a = key[consumed + 1 + j] & 0xFF;
                        int b = data[off + j] & 0xFF;
                        if (a != b) { c = a - b; break; }
                    }
                    if (c == 0) {
                        c = slen - lm;
                        if (c == 0) return new int[]{1, m};
                    }
                }
                if (c > 0) lo = m + 1; else hi = m;
            }
            return new int[]{0, lo};
        }

        int findEntry(byte[] key, int consumed) { int[] r = findPos(key, consumed); return r[0] == 1 ? r[1] : -1; }
        int findInsertPos(byte[] key, int consumed) { return findPos(key, consumed)[1]; }

        // --- Capacity management ---
        private void ensureCapacity(int needed, int newTailBytes) {
            int ksNeeded = ksUsed + newTailBytes;
            int ksCap = data.length - capacity * 4;
            if (needed <= capacity && ksNeeded <= ksCap) return;

            int newCap = needed > capacity
                ? Math.max(MIN_CAPACITY, needed * GROW_NUMER / GROW_DENOM + 1)
                : capacity;
            int newKsCap = ksNeeded > ksCap
                ? ksNeeded * GROW_NUMER / GROW_DENOM + INITIAL_KS_HEADROOM
                : ksCap;
            rebuild(newCap, newKsCap);
        }

        void maybeShrink() {
            if (capacity > MIN_CAPACITY && capacity > count * SHRINK_FACTOR) {
                int newCap = Math.max(MIN_CAPACITY, count * GROW_NUMER / GROW_DENOM + 1);
                rebuild(newCap, ksUsed + INITIAL_KS_HEADROOM);
            }
        }

        // Rebuild data[] with new capacity, compacting blob
        private void rebuild(int newCap, int newKsCap) {
            int newDataLen = newCap * 4 + newKsCap;
            byte[] nd = new byte[newDataLen];
            Object[] nv = new Object[newCap];
            int oldKsOff = capacity * 4;
            int newKsOff = newCap * 4;

            // Copy F, L, O, keysuffix, values
            System.arraycopy(data, 0, nd, 0, Math.min(count, newCap));                      // F
            for (int i = 0; i < Math.min(count, newCap); i++) nd[newCap + i] = data[capacity + i]; // L
            for (int i = 0; i < Math.min(count, newCap); i++) {                              // O
                nd[newCap * 2 + i * 2] = data[capacity * 2 + i * 2];
                nd[newCap * 2 + i * 2 + 1] = data[capacity * 2 + i * 2 + 1];
            }
            System.arraycopy(data, oldKsOff, nd, newKsOff, Math.min(ksUsed, newKsCap));      // blob
            System.arraycopy(values, 0, nv, 0, Math.min(count, newCap));                     // values

            data = nd;
            values = nv;
            capacity = newCap;
        }

        // --- In-place insert: shift within sections, append tail ---
        void insertAt(int pos, byte fb, int suffixLen, byte[] tailBytes, Object val) {
            int newTail = suffixLen > 1 ? suffixLen - 1 : 0;
            ensureCapacity(count + 1, newTail);

            int n = count;
            int ksBase = capacity * 4;

            // F: shift right within [0..cap)
            System.arraycopy(data, pos, data, pos + 1, n - pos);
            data[pos] = fb;

            // L: shift right within [cap..2*cap)
            int lOff = capacity;
            System.arraycopy(data, lOff + pos, data, lOff + pos + 1, n - pos);
            data[lOff + pos] = (byte) suffixLen;

            // O: shift right within [2*cap..4*cap), 2 bytes per entry
            int oOff = capacity * 2;
            System.arraycopy(data, oOff + pos * 2, data, oOff + (pos + 1) * 2, (n - pos) * 2);
            // New entry's O: point to current ksUsed
            data[oOff + pos * 2] = (byte) (ksUsed >> 8);
            data[oOff + pos * 2 + 1] = (byte) (ksUsed & 0xFF);

            // Append tail bytes to blob
            if (newTail > 0 && tailBytes != null)
                System.arraycopy(tailBytes, 0, data, ksBase + ksUsed, newTail);
            ksUsed += newTail;

            // Values: shift right
            System.arraycopy(values, pos, values, pos + 1, n - pos);
            values[pos] = val;

            count = n + 1;
        }

        // --- In-place remove: shift within sections, dead blob bytes stay ---
        void removeAt(int pos) {
            int n = count;
            if (n <= 1) { data = new byte[0]; values = new Object[0]; count = 0; capacity = 0; ksUsed = 0; return; }

            // F: shift left
            System.arraycopy(data, pos + 1, data, pos, n - pos - 1);

            // L: shift left
            int lOff = capacity;
            System.arraycopy(data, lOff + pos + 1, data, lOff + pos, n - pos - 1);

            // O: shift left, 2 bytes per entry
            int oOff = capacity * 2;
            System.arraycopy(data, oOff + (pos + 1) * 2, data, oOff + pos * 2, (n - pos - 1) * 2);

            // Values: shift left, null out last
            System.arraycopy(values, pos + 1, values, pos, n - pos - 1);
            values[n - 1] = null;

            count = n - 1;
            maybeShrink();
        }
    }

    // === Build entry for compact construction ===
    record BuildEntry(byte firstByte, int suffixLen, byte[] tail, Object value) {
        static BuildEntry fromKey(byte[] key, int consumed, Object value) {
            int rem = key.length - consumed;
            if (rem == 0) return new BuildEntry((byte) 0, 0, null, value);
            byte fb = key[consumed];
            byte[] tail = rem > 1 ? Arrays.copyOfRange(key, consumed + 1, key.length) : NO_SKIP;
            return new BuildEntry(fb, rem, tail, value);
        }
    }

    // === Fields ===
    private Node root = CompactNode.SENTINEL;
    private int size = 0;
    private int modCount = 0;

    // === Skip helpers ===
    static boolean matchSkip(byte[] skip, byte[] key, int consumed) {
        if (consumed + skip.length > key.length) return false;
        for (int i = 0; i < skip.length; i++)
            if (skip[i] != key[consumed + i]) return false;
        return true;
    }

    static int skipMismatch(byte[] skip, byte[] key, int consumed) {
        int maxCmp = Math.min(skip.length, key.length - consumed);
        for (int i = 0; i < maxCmp; i++)
            if (skip[i] != key[consumed + i]) return i;
        if (maxCmp < skip.length) return maxCmp; // key shorter than skip
        return -1;
    }

    // === nodeTailTotal ===
    static long nodeTailTotal(Node node) {
        if (node == CompactNode.SENTINEL) return 0;
        if (node instanceof CompactNode c) {
            long ksUsed = 0;
            for (int i = 0; i < c.count; i++) ksUsed += c.L(i);
            return ksUsed + (long) c.count * (1 + c.skipLen());
        }
        return ((BitmaskNode) node).totalTailBytes;
    }

    // === countEntries ===
    static int countEntries(Node node) {
        if (node == CompactNode.SENTINEL) return 0;
        if (node instanceof CompactNode c) return c.count;
        BitmaskNode bm = (BitmaskNode) node;
        int n = countEntries(bm.eosChild());
        int bit = bm.findNextSet(0);
        while (bit >= 0) { n += countEntries(bm.children[bm.slotOf(bit)]); bit = bm.findNextSet(bit + 1); }
        return n;
    }

    // === buildCompact with keysuffix sharing ===
    static CompactNode buildCompact(byte[] skip, BuildEntry[] entries, int n) {
        if (n == 0) return CompactNode.SENTINEL;

        // Pass 1: compute blob size with sharing
        int blobSize = 0;
        for (int i = 0; i < n; i++) {
            int tail = entries[i].suffixLen() > 0 ? entries[i].suffixLen() - 1 : 0;
            if (tail == 0) continue;
            boolean shares = i + 1 < n
                && entries[i + 1].suffixLen() > entries[i].suffixLen()
                && entries[i + 1].firstByte() == entries[i].firstByte()
                && entries[i].tail() != null && entries[i + 1].tail() != null
                && prefixMatch(entries[i].tail(), entries[i + 1].tail(), tail);
            if (!shares) blobSize += tail;
        }

        int dataLen = n + n + 2 * n + blobSize;
        byte[] data = new byte[dataLen];
        Object[] values = new Object[n];
        int ksOff = 4 * n;
        int cursor = 0;
        int chainStart = -1;

        for (int i = 0; i < n; i++) {
            int klen = entries[i].suffixLen();
            int tail = klen > 0 ? klen - 1 : 0;
            data[i] = entries[i].firstByte();                  // F[i]
            data[n + i] = (byte) klen;                         // L[i]
            values[i] = entries[i].value();

            if (tail == 0) {
                data[2 * n + 2 * i] = (byte) (cursor >> 8);
                data[2 * n + 2 * i + 1] = (byte) (cursor & 0xFF);
                continue;
            }

            boolean shares = i + 1 < n
                && entries[i + 1].suffixLen() > klen
                && entries[i + 1].firstByte() == entries[i].firstByte()
                && entries[i].tail() != null && entries[i + 1].tail() != null
                && prefixMatch(entries[i].tail(), entries[i + 1].tail(), tail);

            if (shares) {
                if (chainStart < 0) chainStart = i;
                continue;
            }

            // Longest in chain or standalone
            data[2 * n + 2 * i] = (byte) (cursor >> 8);
            data[2 * n + 2 * i + 1] = (byte) (cursor & 0xFF);
            System.arraycopy(entries[i].tail(), 0, data, ksOff + cursor, tail);

            if (chainStart >= 0) {
                for (int j = chainStart; j < i; j++) {
                    data[2 * n + 2 * j] = (byte) (cursor >> 8);
                    data[2 * n + 2 * j + 1] = (byte) (cursor & 0xFF);
                }
                chainStart = -1;
            }
            cursor += tail;
        }

        CompactNode c = new CompactNode();
        c.skip = skip;
        c.data = data;
        c.values = values;
        c.count = n;
        c.capacity = n;
        c.ksUsed = cursor;
        return c;
    }

    static boolean prefixMatch(byte[] a, byte[] b, int len) {
        for (int i = 0; i < len; i++) if (a[i] != b[i]) return false;
        return true;
    }

    // === Extract entries from compact node ===
    static BuildEntry[] extractEntries(CompactNode c) {
        BuildEntry[] entries = new BuildEntry[c.count];
        int ksOff = c.ksOff();
        for (int i = 0; i < c.count; i++) {
            int klen = c.L(i);
            byte fb = klen > 0 ? c.F(i) : 0;
            byte[] tail = null;
            if (klen > 1) {
                int off = c.O(i);
                tail = Arrays.copyOfRange(c.data, ksOff + off, ksOff + off + klen - 1);
            }
            entries[i] = new BuildEntry(fb, klen, tail, c.values[i]);
        }
        return entries;
    }

    // === find ===
    @SuppressWarnings("unchecked")
    public V getBytes(byte[] key) {
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm.skip, key, consumed)) return null;
            consumed += bm.skipLen();
            if (consumed >= key.length) {
                node = bm.eosChild();
                break;
            }
            node = bm.dispatch(key[consumed++] & 0xFF);
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        if (!matchSkip(c.skip, key, consumed)) return null;
        consumed += c.skipLen();
        int pos = c.findEntry(key, consumed);
        return pos >= 0 ? (V) c.values[pos] : null;
    }

    // === put ===
    @SuppressWarnings("unchecked")
    public V putBytes(byte[] key, V value) {
        return putImpl(key, value, false);
    }

    public boolean insertBytes(byte[] key, V value) {
        int oldSize = size;
        putImpl(key, value, true);
        return size > oldSize;
    }

    @SuppressWarnings("unchecked")
    private V putImpl(byte[] key, V value, boolean onlyIfAbsent) {
        if (root == CompactNode.SENTINEL) {
            BuildEntry[] e = { BuildEntry.fromKey(key, 0, value) };
            root = buildCompact(NO_SKIP, e, 1);
            size++; modCount++;
            return null;
        }

        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm.skip, key, consumed);
            if (mm >= 0) {
                splitBitmaskSkip(bm, key, consumed, value, mm);
                size++; modCount++;
                return null;
            }
            consumed += bm.skipLen();
            if (consumed >= key.length) {
                return insertIntoEos(bm, key, consumed, value, onlyIfAbsent);
            }
            int b = key[consumed] & 0xFF;
            consumed++;
            if (!bm.hasBit(b)) {
                long oldTail = bm.totalTailBytes;
                BuildEntry[] e = { BuildEntry.fromKey(key, consumed, value) };
                CompactNode child = buildCompact(NO_SKIP, e, 1);
                bm.insertChild(b, child);
                bm.totalTailBytes += nodeTailTotal(child);
                propagateTail(bm.parent, nodeTailTotal(child));
                size++; modCount++;
                return null;
            }
            node = bm.children[bm.slotOf(b)];
        }

        // Compact leaf
        CompactNode c = (CompactNode) node;
        int mm = skipMismatch(c.skip, key, consumed);
        if (mm >= 0) {
            splitCompactSkip(c, key, consumed, value, mm);
            size++; modCount++;
            return null;
        }
        consumed += c.skipLen();

        int pos = c.findEntry(key, consumed);
        if (pos >= 0) {
            if (onlyIfAbsent) return (V) c.values[pos];
            V old = (V) c.values[pos];
            c.values[pos] = value;
            modCount++;
            return old;
        }

        // Insert in-place
        long oldTail = nodeTailTotal(c);
        int ins = c.findInsertPos(key, consumed);
        int rem = key.length - consumed;
        byte fb = rem > 0 ? key[consumed] : 0;
        byte[] tailBytes = rem > 1 ? Arrays.copyOfRange(key, consumed + 1, key.length) : NO_SKIP;
        c.insertAt(ins, fb, rem, tailBytes, value);

        long delta = nodeTailTotal(c) - oldTail;
        propagateTail(c.parent, delta);

        if (nodeTailTotal(c) > COMPACT_KEYSUFFIX_LIMIT && c.count > 1)
            split(c);

        size++; modCount++;
        return null;
    }

    @SuppressWarnings("unchecked")
    private V insertIntoEos(BitmaskNode bm, byte[] key, int consumed, V value, boolean onlyIfAbsent) {
        Node eos = bm.eosChild();
        if (eos == CompactNode.SENTINEL) {
            BuildEntry[] e = { new BuildEntry((byte) 0, 0, null, value) };
            CompactNode nc = buildCompact(NO_SKIP, e, 1);
            long oldTail = nodeTailTotal(eos);
            bm.setEosChild(nc);
            bm.totalTailBytes += nodeTailTotal(nc) - oldTail;
            propagateTail(bm.parent, nodeTailTotal(nc) - oldTail);
            size++; modCount++;
            return null;
        }
        // EOS child exists — insert into it
        CompactNode c = (CompactNode) eos;
        int pos = c.findEntry(key, consumed);
        if (pos >= 0) {
            if (onlyIfAbsent) return (V) c.values[pos];
            V old = (V) c.values[pos];
            c.values[pos] = value;
            modCount++;
            return old;
        }
        // Add EOS entry in-place
        long oldTail = nodeTailTotal(c);
        c.insertAt(0, (byte) 0, 0, null, value); // EOS sorts first
        long newTail = nodeTailTotal(c);
        bm.totalTailBytes += newTail - oldTail;
        propagateTail(bm.parent, newTail - oldTail);
        size++; modCount++;
        return null;
    }

    // === remove ===
    @SuppressWarnings("unchecked")
    public V removeBytes(byte[] key) {
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm.skip, key, consumed)) return null;
            consumed += bm.skipLen();
            if (consumed >= key.length) {
                return removeFromEos(bm, key, consumed);
            }
            node = bm.dispatch(key[consumed++] & 0xFF);
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        if (!matchSkip(c.skip, key, consumed)) return null;
        consumed += c.skipLen();

        int pos = c.findEntry(key, consumed);
        if (pos < 0) return null;
        V old = (V) c.values[pos];

        long oldTail = nodeTailTotal(c);
        c.removeAt(pos);

        if (c.count == 0) {
            replaceNode(c, CompactNode.SENTINEL);
            propagateTail(c.parent, -oldTail);
            removeEmpty(c.parent);
        } else {
            long delta = nodeTailTotal(c) - oldTail;
            propagateTail(c.parent, delta);
            checkCoalesce(c.parent);
        }
        size--; modCount++;
        return old;
    }

    @SuppressWarnings("unchecked")
    private V removeFromEos(BitmaskNode bm, byte[] key, int consumed) {
        Node eos = bm.eosChild();
        if (eos == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) eos;
        int pos = c.findEntry(key, consumed);
        if (pos < 0) return null;
        V old = (V) c.values[pos];

        long oldTail = nodeTailTotal(c);
        c.removeAt(pos);

        if (c.count == 0) {
            bm.setEosChild(CompactNode.SENTINEL);
            bm.totalTailBytes -= oldTail;
            propagateTail(bm.parent, -oldTail);
        } else {
            long delta = nodeTailTotal(c) - oldTail;
            bm.totalTailBytes += delta;
            propagateTail(bm.parent, delta);
        }
        checkCoalesce(bm);
        size--; modCount++;
        return old;
    }

    // === split: compact → bitmask ===
    private void split(CompactNode leaf) {
        BuildEntry[] entries = extractEntries(leaf);
        int consumed = 0; // relative to this node's position

        // Find common skip among all entries
        byte[] newSkip = computeCommonSkip(entries);
        int skipLen = newSkip.length;

        // Group by first byte after skip (entry suffix position skipLen)
        Map<Integer, List<BuildEntry>> groups = new LinkedHashMap<>();
        List<BuildEntry> eosGroup = new ArrayList<>();

        for (BuildEntry e : entries) {
            if (e.suffixLen() <= skipLen) {
                // EOS or entry fully consumed by skip
                eosGroup.add(new BuildEntry((byte) 0, 0, null, e.value()));
            } else {
                // Trim skip from entry
                int newLen = e.suffixLen() - skipLen;
                byte newF;
                byte[] newTail;
                if (skipLen == 0) {
                    newF = e.firstByte();
                    newTail = e.tail();
                } else {
                    // Reconstruct suffix bytes after skip
                    byte[] full = new byte[e.suffixLen()];
                    full[0] = e.firstByte();
                    if (e.tail() != null) System.arraycopy(e.tail(), 0, full, 1, e.tail().length);
                    newF = full[skipLen];
                    newTail = newLen > 1 ? Arrays.copyOfRange(full, skipLen + 1, full.length) : NO_SKIP;
                }
                int db = newF & 0xFF;
                // Entry for child: remove dispatch byte
                int childLen = newLen - 1;
                byte childF = childLen > 0 && newTail != null && newTail.length > 0 ? newTail[0] : 0;
                byte[] childTail = childLen > 1 && newTail != null ? Arrays.copyOfRange(newTail, 1, newTail.length) : NO_SKIP;
                BuildEntry childEntry = new BuildEntry(childF, childLen, childTail, e.value());
                groups.computeIfAbsent(db, k -> new ArrayList<>()).add(childEntry);
            }
        }

        // Build bitmask
        BitmaskNode bm = new BitmaskNode();
        bm.skip = newSkip;
        int cc = groups.size();
        bm.children = new Node[cc + 2]; // sentinel + children + eos
        bm.children[0] = CompactNode.SENTINEL;

        // EOS child
        if (eosGroup.isEmpty()) {
            bm.children[cc + 1] = CompactNode.SENTINEL;
        } else {
            CompactNode eosNode = buildCompact(NO_SKIP, eosGroup.toArray(new BuildEntry[0]), eosGroup.size());
            bm.children[cc + 1] = eosNode;
            eosNode.parent = bm; eosNode.parentByte = PARENT_EOS;
        }

        int slot = 1;
        for (var entry : groups.entrySet()) {
            int b = entry.getKey();
            bm.setBit(b);
            List<BuildEntry> group = entry.getValue();
            byte[] childSkip = computeCommonSkip(group.toArray(new BuildEntry[0]));
            // Trim childSkip from entries
            BuildEntry[] childEntries;
            if (childSkip.length > 0) {
                childEntries = trimSkip(group.toArray(new BuildEntry[0]), childSkip.length);
            } else {
                childEntries = group.toArray(new BuildEntry[0]);
            }
            CompactNode child = buildCompact(childSkip, childEntries, childEntries.length);
            child.parent = bm; child.parentByte = b;
            bm.children[slot++] = child;
        }

        // Compute totalTailBytes
        bm.totalTailBytes = 0;
        for (int i = 1; i < bm.children.length; i++)
            bm.totalTailBytes += nodeTailTotal(bm.children[i]);

        replaceNode(leaf, bm);

        // Recurse for oversize children
        for (int i = 1; i < bm.children.length - 1; i++)
            if (bm.children[i] instanceof CompactNode c2 && nodeTailTotal(c2) > COMPACT_KEYSUFFIX_LIMIT && c2.count > 1)
                split(c2);
    }

    // === splitBitmaskSkip ===
    private void splitBitmaskSkip(BitmaskNode bmNode, byte[] key, int consumed, V value, int mm) {
        BitmaskNode oldParent = bmNode.parent;
        int oldParentByte = bmNode.parentByte;

        BitmaskNode bm = new BitmaskNode();
        bm.skip = mm > 0 ? Arrays.copyOfRange(bmNode.skip, 0, mm) : NO_SKIP;

        int existByte = bmNode.skip[mm] & 0xFF;
        boolean keyExhausted = (consumed + mm >= key.length);

        int remaining = bmNode.skip.length - mm - 1;
        bmNode.skip = remaining > 0 ? Arrays.copyOfRange(bmNode.skip, mm + 1, bmNode.skip.length) : NO_SKIP;

        if (keyExhausted) {
            BuildEntry[] eosEntries = { new BuildEntry((byte) 0, 0, null, value) };
            CompactNode eosNode = buildCompact(NO_SKIP, eosEntries, 1);

            bm.children = new Node[3]; // sentinel + 1 child + eos
            bm.children[0] = CompactNode.SENTINEL;
            bm.setBit(existByte);
            bm.children[1] = bmNode;
            bm.children[2] = eosNode;

            bmNode.parent = bm; bmNode.parentByte = existByte;
            eosNode.parent = bm; eosNode.parentByte = PARENT_EOS;
            bm.totalTailBytes = nodeTailTotal(bmNode) + nodeTailTotal(eosNode);
        } else {
            int newByte = key[consumed + mm] & 0xFF;

            BuildEntry[] e = { BuildEntry.fromKey(key, consumed + mm + 1, value) };
            
            CompactNode nl = buildCompact(NO_SKIP, e, 1);

            int cc = 2;
            bm.children = new Node[cc + 2];
            bm.children[0] = CompactNode.SENTINEL;
            bm.children[cc + 1] = CompactNode.SENTINEL;
            bm.setBit(existByte);
            bm.setBit(newByte);

            if (existByte < newByte) { bm.children[1] = bmNode; bm.children[2] = nl; }
            else { bm.children[1] = nl; bm.children[2] = bmNode; }

            bmNode.parent = bm; bmNode.parentByte = existByte;
            nl.parent = bm; nl.parentByte = newByte;
            bm.totalTailBytes = nodeTailTotal(bmNode) + nodeTailTotal(nl);
        }

        bm.parent = oldParent; bm.parentByte = oldParentByte;
        if (oldParent == null) root = bm;
        else if (oldParentByte == PARENT_EOS) oldParent.setEosChild(bm);
        else oldParent.replaceChild(oldParentByte, bm);
    }

    // === splitCompactSkip ===
    private void splitCompactSkip(CompactNode cNode, byte[] key, int consumed, V value, int mm) {
        BitmaskNode oldParent = cNode.parent;
        int oldParentByte = cNode.parentByte;

        BitmaskNode bm = new BitmaskNode();
        bm.skip = mm > 0 ? Arrays.copyOfRange(cNode.skip, 0, mm) : NO_SKIP;

        int existByte = cNode.skip[mm] & 0xFF;
        boolean keyExhausted = (consumed + mm >= key.length);

        // Trim existing node's skip
        int remaining = cNode.skip.length - mm - 1;
        cNode.skip = remaining > 0 ? Arrays.copyOfRange(cNode.skip, mm + 1, cNode.skip.length) : NO_SKIP;

        if (keyExhausted) {
            // New entry is EOS child, existing subtree is byte-dispatched
            BuildEntry[] eosEntries = { new BuildEntry((byte) 0, 0, null, value) };
            CompactNode eosNode = buildCompact(NO_SKIP, eosEntries, 1);

            bm.children = new Node[3]; // sentinel + 1 child + eos
            bm.children[0] = CompactNode.SENTINEL;
            bm.setBit(existByte);
            bm.children[1] = cNode;
            bm.children[2] = eosNode; // EOS

            cNode.parent = bm; cNode.parentByte = existByte;
            eosNode.parent = bm; eosNode.parentByte = PARENT_EOS;
            bm.totalTailBytes = nodeTailTotal(cNode) + nodeTailTotal(eosNode);
        } else {
            int newByte = key[consumed + mm] & 0xFF;

            BuildEntry[] e = { BuildEntry.fromKey(key, consumed + mm + 1, value) };
            
            CompactNode nl = buildCompact(NO_SKIP, e, 1);

            int cc = 2;
            bm.children = new Node[cc + 2];
            bm.children[0] = CompactNode.SENTINEL;
            bm.children[cc + 1] = CompactNode.SENTINEL;
            bm.setBit(existByte);
            bm.setBit(newByte);

            if (existByte < newByte) { bm.children[1] = cNode; bm.children[2] = nl; }
            else { bm.children[1] = nl; bm.children[2] = cNode; }

            cNode.parent = bm; cNode.parentByte = existByte;
            nl.parent = bm; nl.parentByte = newByte;
            bm.totalTailBytes = nodeTailTotal(cNode) + nodeTailTotal(nl);
        }

        bm.parent = oldParent; bm.parentByte = oldParentByte;
        if (oldParent == null) root = bm;
        else if (oldParentByte == PARENT_EOS) oldParent.setEosChild(bm);
        else oldParent.replaceChild(oldParentByte, bm);
    }

    // === coalesce / collapse ===
    private void checkCoalesce(BitmaskNode bm) {
        if (bm == null) return;
        int total = bm.childCount() + (bm.hasEos() ? 1 : 0);
        if (bm.totalTailBytes <= COMPACT_KEYSUFFIX_LIMIT) { coalesce(bm); return; }
        if (total == 0) { replaceNode(bm, CompactNode.SENTINEL); removeEmpty(bm.parent); return; }
        if (total == 1) collapse(bm);
    }

    private void coalesce(BitmaskNode bm) {
        List<BuildEntry> all = new ArrayList<>();
        collectAllEntries(bm, all, new byte[0]);
        if (all.isEmpty()) { replaceNode(bm, CompactNode.SENTINEL); return; }
        byte[] skip = computeCommonSkip(all.toArray(new BuildEntry[0]));
        BuildEntry[] entries = skip.length > 0
            ? trimSkip(all.toArray(new BuildEntry[0]), skip.length)
            : all.toArray(new BuildEntry[0]);
        CompactNode c = buildCompact(mergeSkip(bm.skip, skip), entries, entries.length);
        replaceNode(bm, c);
    }

    private void collectAllEntries(Node node, List<BuildEntry> out, byte[] prefix) {
        if (node == CompactNode.SENTINEL) return;
        if (node instanceof CompactNode c) {
            byte[] fullPrefix = prefix;
            if (c.skip.length > 0) fullPrefix = concat(prefix, c.skip);
            for (int i = 0; i < c.count; i++) {
                int klen = c.L(i);
                if (klen == 0) {
                    out.add(new BuildEntry((byte) 0, fullPrefix.length, fullPrefix.length > 0 ? fullPrefix : null, c.values[i]));
                } else {
                    byte[] suffix = new byte[klen];
                    suffix[0] = c.F(i);
                    if (klen > 1) System.arraycopy(c.data, c.ksOff() + c.O(i), suffix, 1, klen - 1);
                    byte[] full = concat(fullPrefix, suffix);
                    out.add(new BuildEntry(full[0], full.length, full.length > 1 ? Arrays.copyOfRange(full, 1, full.length) : null, c.values[i]));
                }
            }
            return;
        }
        BitmaskNode bm = (BitmaskNode) node;
        byte[] bp = bm.skip.length > 0 ? concat(prefix, bm.skip) : prefix;
        collectAllEntries(bm.eosChild(), out, bp);
        int bit = bm.findNextSet(0);
        while (bit >= 0) {
            byte[] cp = concat(bp, new byte[]{(byte) bit});
            collectAllEntries(bm.children[bm.slotOf(bit)], out, cp);
            bit = bm.findNextSet(bit + 1);
        }
    }

    private void collapse(BitmaskNode bm) {
        Node child; int db;
        if (bm.hasEos() && bm.childCount() == 0) { child = bm.eosChild(); db = PARENT_EOS; }
        else if (!bm.hasEos() && bm.childCount() == 1) { int bit = bm.findNextSet(0); child = bm.children[bm.slotOf(bit)]; db = bit; }
        else return;

        if (child instanceof CompactNode cc) {
            cc.skip = mergeSkip(bm.skip, db >= 0 ? new byte[]{(byte) db} : NO_SKIP, cc.skip);
        } else if (child instanceof BitmaskNode bc) {
            bc.skip = mergeSkip(bm.skip, db >= 0 ? new byte[]{(byte) db} : NO_SKIP, bc.skip);
        }
        replaceNode(bm, child);
    }

    private void removeEmpty(BitmaskNode bm) {
        if (bm == null) return;
        int total = bm.childCount() + (bm.hasEos() ? 1 : 0);
        if (total == 0) {
            replaceNode(bm, CompactNode.SENTINEL);
            removeEmpty(bm.parent);
        } else {
            checkCoalesce(bm);
        }
    }

    // === Helpers ===
    private void replaceNode(Node old, Node replacement) {
        if (old.parent == null && old.parentByte == PARENT_ROOT) {
            root = replacement;
            replacement.parent = null;
            replacement.parentByte = PARENT_ROOT;
        } else if (old.parent != null) {
            if (old.parentByte == PARENT_EOS) old.parent.setEosChild(replacement);
            else old.parent.replaceChild(old.parentByte, replacement);
        }
    }

    private void propagateTail(BitmaskNode bm, long delta) {
        while (bm != null) { bm.totalTailBytes += delta; bm = bm.parent; }
    }

    static byte[] commonSkip(byte[] key, int off, int len) {
        return len > 0 ? Arrays.copyOfRange(key, off, off + len) : NO_SKIP;
    }

    static byte[] computeCommonSkip(BuildEntry[] entries) {
        if (entries.length <= 1) return NO_SKIP; // single entry doesn't need skip
        // Find common prefix of all suffixes
        byte[][] suffixes = new byte[entries.length][];
        for (int i = 0; i < entries.length; i++) {
            int klen = entries[i].suffixLen();
            if (klen == 0) return NO_SKIP; // EOS entry — no common skip
            suffixes[i] = new byte[klen];
            suffixes[i][0] = entries[i].firstByte();
            if (entries[i].tail() != null)
                System.arraycopy(entries[i].tail(), 0, suffixes[i], 1, entries[i].tail().length);
        }
        int common = suffixes[0].length;
        for (int j = 1; j < suffixes.length; j++) {
            common = Math.min(common, suffixes[j].length);
            for (int k = 0; k < common; k++)
                if (suffixes[0][k] != suffixes[j][k]) { common = k; break; }
        }
        return common > 0 ? Arrays.copyOf(suffixes[0], common) : NO_SKIP;
    }

    static BuildEntry[] trimSkip(BuildEntry[] entries, int skipLen) {
        BuildEntry[] result = new BuildEntry[entries.length];
        for (int i = 0; i < entries.length; i++) {
            int klen = entries[i].suffixLen();
            int newLen = klen - skipLen;
            if (newLen <= 0) {
                result[i] = new BuildEntry((byte) 0, 0, null, entries[i].value());
            } else {
                byte[] full = new byte[klen];
                full[0] = entries[i].firstByte();
                if (entries[i].tail() != null) System.arraycopy(entries[i].tail(), 0, full, 1, entries[i].tail().length);
                byte newF = full[skipLen];
                byte[] newTail = newLen > 1 ? Arrays.copyOfRange(full, skipLen + 1, full.length) : NO_SKIP;
                result[i] = new BuildEntry(newF, newLen, newTail, entries[i].value());
            }
        }
        return result;
    }

    static byte[] mergeSkip(byte[]... parts) {
        int total = 0;
        for (byte[] p : parts) total += p.length;
        if (total == 0) return NO_SKIP;
        byte[] result = new byte[total];
        int off = 0;
        for (byte[] p : parts) if (p.length > 0) { System.arraycopy(p, 0, result, off, p.length); off += p.length; }
        return result;
    }

    static byte[] concat(byte[] a, byte[] b) {
        byte[] r = new byte[a.length + b.length];
        System.arraycopy(a, 0, r, 0, a.length);
        System.arraycopy(b, 0, r, a.length, b.length);
        return r;
    }

    // === Iterator ===
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

    // Reconstruct key for a compact node entry
    String reconstructKey(CompactNode leaf, int index) {
        // Collect path bytes walking up
        List<byte[]> parts = new ArrayList<>();
        // Entry suffix
        int klen = leaf.L(index);
        if (klen > 0) {
            byte[] suffix = new byte[klen];
            suffix[0] = leaf.F(index);
            if (klen > 1) System.arraycopy(leaf.data, leaf.ksOff() + leaf.O(index), suffix, 1, klen - 1);
            parts.add(suffix);
        }
        if (leaf.skip.length > 0) parts.add(leaf.skip);

        Node n = leaf;
        while (n.parent != null) {
            BitmaskNode bm = n.parent;
            if (n.parentByte >= 0) parts.add(new byte[]{(byte) n.parentByte});
            if (bm.skip.length > 0) parts.add(bm.skip);
            n = bm;
        }

        // Reverse and concatenate
        int total = 0;
        for (byte[] p : parts) total += p.length;
        byte[] key = new byte[total];
        int off = total;
        for (byte[] p : parts) { off -= p.length; System.arraycopy(p, 0, key, off, p.length); }
        return new String(key, StandardCharsets.UTF_8);
    }

    // === Map interface ===
    @Override public int size() { return size; }
    @Override public boolean isEmpty() { return size == 0; }

    @Override public boolean containsKey(Object key) {
        if (!(key instanceof String s)) return false;
        return getBytes(s.getBytes(StandardCharsets.UTF_8)) != null;
    }

    @Override public V get(Object key) {
        if (!(key instanceof String s)) return null;
        return getBytes(s.getBytes(StandardCharsets.UTF_8));
    }

    @Override public V put(String key, V value) {
        return putBytes(key.getBytes(StandardCharsets.UTF_8), value);
    }

    @Override public V remove(Object key) {
        if (!(key instanceof String s)) return null;
        return removeBytes(s.getBytes(StandardCharsets.UTF_8));
    }

    @Override public void clear() { root = CompactNode.SENTINEL; size = 0; modCount++; }

    // === NavigableMap ===
    @Override public Comparator<? super String> comparator() { return null; }
    @Override public String firstKey() { CompactNode c = descendFirst(root); if (c == null) throw new NoSuchElementException(); return reconstructKey(c, 0); }
    @Override public String lastKey() { CompactNode c = descendLast(root); if (c == null) throw new NoSuchElementException(); return reconstructKey(c, c.count - 1); }
    @Override public Map.Entry<String, V> firstEntry() { CompactNode c = descendFirst(root); return c == null ? null : makeEntry(c, 0); }
    @Override @SuppressWarnings("unchecked") public Map.Entry<String, V> lastEntry() { CompactNode c = descendLast(root); return c == null ? null : makeEntry(c, c.count - 1); }
    @Override public Map.Entry<String, V> pollFirstEntry() { var e = firstEntry(); if (e != null) remove(e.getKey()); return e; }
    @Override public Map.Entry<String, V> pollLastEntry() { var e = lastEntry(); if (e != null) remove(e.getKey()); return e; }

    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<String, V> makeEntry(CompactNode c, int i) {
        return new SimpleImmutableEntry<>(reconstructKey(c, i), (V) c.values[i]);
    }

    // Simple O(N) implementations — proper descent follow-up
    @Override public Map.Entry<String, V> lowerEntry(String key) { Map.Entry<String, V> r = null; for (var e : entrySet()) { if (e.getKey().compareTo(key) < 0) r = e; else break; } return r; }
    @Override public Map.Entry<String, V> higherEntry(String key) { for (var e : entrySet()) if (e.getKey().compareTo(key) > 0) return e; return null; }
    @Override public Map.Entry<String, V> floorEntry(String key) { var v = get(key); if (v != null) return new SimpleImmutableEntry<>(key, v); return lowerEntry(key); }
    @Override public Map.Entry<String, V> ceilingEntry(String key) { var v = get(key); if (v != null) return new SimpleImmutableEntry<>(key, v); return higherEntry(key); }
    @Override public String floorKey(String k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
    @Override public String ceilingKey(String k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
    @Override public String lowerKey(String k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }
    @Override public String higherKey(String k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }
    @Override public NavigableMap<String, V> descendingMap() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableSet<String> navigableKeySet() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableSet<String> descendingKeySet() { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<String, V> subMap(String a, boolean ai, String b, boolean bi) { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<String, V> headMap(String t, boolean i) { throw new UnsupportedOperationException("TODO"); }
    @Override public NavigableMap<String, V> tailMap(String f, boolean i) { throw new UnsupportedOperationException("TODO"); }
    @Override public SortedMap<String, V> subMap(String a, String b) { throw new UnsupportedOperationException("TODO"); }
    @Override public SortedMap<String, V> headMap(String t) { throw new UnsupportedOperationException("TODO"); }
    @Override public SortedMap<String, V> tailMap(String f) { throw new UnsupportedOperationException("TODO"); }

    // === entrySet + Iterator ===
    @Override public Set<Map.Entry<String, V>> entrySet() {
        return new AbstractSet<>() {
            @Override public int size() { return KSTrie.this.size; }
            @Override public Iterator<Map.Entry<String, V>> iterator() { return new Iter(); }
        };
    }

    private class Iter implements Iterator<Map.Entry<String, V>> {
        CompactNode leaf;
        int index;
        int expectedModCount;
        String lastKey;
        boolean canRemove;

        Iter() { expectedModCount = modCount; leaf = descendFirst(root); index = 0; }

        @Override public boolean hasNext() { return leaf != null && index < leaf.count; }

        @Override @SuppressWarnings("unchecked")
        public Map.Entry<String, V> next() {
            if (modCount != expectedModCount) throw new ConcurrentModificationException();
            if (!hasNext()) throw new NoSuchElementException();
            String key = reconstructKey(leaf, index);
            V val = (V) leaf.values[index];
            lastKey = key;
            canRemove = true;
            advance();
            return new SimpleImmutableEntry<>(key, val);
        }

        private void advance() {
            index++;
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
            KSTrie.this.remove(lastKey);
            expectedModCount = modCount;
            // Refind position after lastKey
            refind();
        }

        private void refind() {
            leaf = descendFirst(root); index = 0;
            if (leaf == null) return;
            while (leaf != null) {
                for (; index < leaf.count; index++) {
                    String k = reconstructKey(leaf, index);
                    if (k.compareTo(lastKey) > 0) return;
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

    // === Prefix operations ===
    public int prefixCount(String prefix) {
        return prefixCountBytes(prefix.getBytes(StandardCharsets.UTF_8));
    }

    public int prefixCountBytes(byte[] prefix) {
        Node node = root;
        int consumed = 0;
        while (node != CompactNode.SENTINEL) {
            if (node instanceof CompactNode c) {
                if (!matchSkipPartial(c.skip, prefix, consumed, c.skipLen()))
                    return matchSkipContains(c.skip, prefix, consumed) ? countEntries(c) : 0;
                consumed += c.skipLen();
                if (consumed >= prefix.length) return c.count;
                // Count entries whose suffix matches remaining prefix
                int rem = prefix.length - consumed;
                int n = 0;
                for (int i = 0; i < c.count; i++) {
                    int cmp = comparePrefixToEntry(c, i, prefix, consumed);
                    if (cmp == 0) n++;
                    else if (cmp < 0) break; // sorted, no more matches
                }
                return n;
            }
            BitmaskNode bm = (BitmaskNode) node;
            if (!matchSkipPartial(bm.skip, prefix, consumed, bm.skipLen()))
                return matchSkipContains(bm.skip, prefix, consumed) ? countEntries(bm) : 0;
            consumed += bm.skipLen();
            if (consumed >= prefix.length) return countEntries(node);
            node = bm.dispatch(prefix[consumed++] & 0xFF);
        }
        return 0;
    }

    // Does prefix end within skip? (prefix is shorter than skip but matches so far)
    static boolean matchSkipContains(byte[] skip, byte[] prefix, int consumed) {
        int rem = prefix.length - consumed;
        if (rem >= skip.length) return false; // not a partial match
        for (int i = 0; i < rem; i++) if (skip[i] != prefix[consumed + i]) return false;
        return true;
    }

    static boolean matchSkipPartial(byte[] skip, byte[] prefix, int consumed, int skipLen) {
        int rem = prefix.length - consumed;
        if (rem < skipLen) return false; // can't fully match skip — handle separately
        for (int i = 0; i < skipLen; i++) if (skip[i] != prefix[consumed + i]) return false;
        return true;
    }

    // Does entry i's suffix start with prefix[consumed..]? Returns 0 match, >0 past, <0 before
    int comparePrefixToEntry(CompactNode c, int i, byte[] prefix, int consumed) {
        int rem = prefix.length - consumed;
        int klen = c.L(i);
        if (rem == 0) return 0; // empty remaining prefix matches everything
        if (klen == 0) return 1; // EOS entry, but we still have prefix bytes
        int fb = c.F(i) & 0xFF;
        int pb = prefix[consumed] & 0xFF;
        if (fb != pb) return fb > pb ? -1 : 1; // entry before/after prefix
        int tail = klen - 1;
        int prefTail = rem - 1;
        int cmpLen = Math.min(tail, prefTail);
        int off = c.ksOff() + c.O(i);
        for (int j = 0; j < cmpLen; j++) {
            int a = c.data[off + j] & 0xFF;
            int b = prefix[consumed + 1 + j] & 0xFF;
            if (a != b) return a > b ? -1 : 1;
        }
        return 0; // prefix matches this far
    }

    // === Main — smoke test ===
    public static void main(String[] args) {
        KSTrie<Integer> t = new KSTrie<>();

        // Basic CRUD
        t.put("apple", 1);
        t.put("app", 2);
        t.put("application", 3);
        t.put("banana", 4);
        t.put("band", 5);
        t.put("ban", 6);
        assert t.size() == 6 : "size=" + t.size();
        assert t.get("apple") == 1;
        assert t.get("app") == 2;
        assert t.get("banana") == 4;
        assert t.get("missing") == null;
        assert t.containsKey("band");
        assert !t.containsKey("xyz");
        System.out.println("Basic CRUD: PASS (size=" + t.size() + ")"); System.out.flush();

        // Overwrite
        assert t.put("apple", 10) == 1;
        assert t.get("apple") == 10;
        System.out.println("Overwrite: PASS"); System.out.flush();

        // Remove
        assert t.remove("banana") == 4;
        assert t.size() == 5;
        assert t.get("banana") == null;
        System.out.println("Remove: PASS"); System.out.flush();

        // Ordered iteration
        TreeMap<String, Integer> ref = new TreeMap<>();
        ref.put("app", 2); ref.put("apple", 10); ref.put("application", 3);
        ref.put("ban", 6); ref.put("band", 5);
        var ti = t.entrySet().iterator();
        var ri = ref.entrySet().iterator();
        int checked = 0;
        while (ti.hasNext() && ri.hasNext()) {
            var e = ti.next(); var r = ri.next();
            assert e.getKey().equals(r.getKey()) : e.getKey() + " != " + r.getKey();
            assert e.getValue().equals(r.getValue()) : e.getValue() + " != " + r.getValue();
            checked++;
        }
        assert !ti.hasNext() && !ri.hasNext() && checked == 5;
        System.out.println("Iteration order: PASS (" + checked + " entries)"); System.out.flush();

        // NavigableMap
        assert t.firstKey().equals("app");
        assert t.lastKey().equals("band");
        System.out.println("NavigableMap: PASS"); System.out.flush();

        // Empty key (EOS)
        t.put("", 99);
        assert t.get("") == 99;
        assert t.size() == 6;
        t.remove("");
        assert t.size() == 5;
        System.out.println("Empty key: PASS"); System.out.flush();

        // Prefix count
        t.put("banana", 4); // re-add
        assert t.size() == 6;
        assert t.prefixCount("app") == 3 : "prefixCount(app)=" + t.prefixCount("app");
        assert t.prefixCount("ban") == 3;
        assert t.prefixCount("xyz") == 0;
        assert t.prefixCount("") == 6;
        System.out.println("Prefix count: PASS"); System.out.flush();

        // Bulk test
        t.clear();
        TreeMap<String, Integer> ref2 = new TreeMap<>();
        var rng = new java.util.Random(42);
        for (int i = 0; i < 10000; i++) {
            int len = rng.nextInt(20) + 1;
            StringBuilder sb = new StringBuilder();
            for (int j = 0; j < len; j++) sb.append((char)('a' + rng.nextInt(26)));
            String k = sb.toString();
            t.put(k, i);
            ref2.put(k, i);
        }
        assert t.size() == ref2.size() : "size " + t.size() + " != " + ref2.size();
        var tit = t.entrySet().iterator();
        var rit = ref2.entrySet().iterator();
        checked = 0;
        while (tit.hasNext() && rit.hasNext()) {
            var te = tit.next(); var re = rit.next();
            assert te.getKey().equals(re.getKey()) : "key " + te.getKey() + " != " + re.getKey() + " at " + checked;
            assert te.getValue().equals(re.getValue());
            checked++;
        }
        assert !tit.hasNext() && !rit.hasNext();
        System.out.println("Bulk 10K: PASS (" + checked + " entries)"); System.out.flush();

        System.out.println("\nALL PASS"); System.out.flush();
    }
}
