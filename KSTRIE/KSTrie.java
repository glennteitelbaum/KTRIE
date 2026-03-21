// KSTrie.java — ordered string-keyed associative container
// Trie/B-tree hybrid implementing NavigableMap<String, V>
// with keysuffix sharing and prefix operations.

import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.*;

/**
 * KSTrie — ordered string-keyed associative container.
 * <p>
 * A trie/B-tree hybrid implementing {@link NavigableMap NavigableMap&lt;String, V&gt;}
 * with keysuffix sharing and prefix operations. Keys are stored and sorted in
 * UTF-8 byte order (not Java's UTF-16 code unit order). Compact leaf nodes pack
 * entries in parallel F/L/O/keysuffix arrays with forward-chain sharing that
 * eliminates redundant suffix storage. Bitmask nodes provide 256-way fan-out
 * with EOS (end-of-string) support for variable-length keys.
 * <p>
 * <b>Ordering:</b> UTF-8 unsigned byte-lexicographic order. This matches C
 * {@code strcmp} order for ASCII and produces a well-defined total order for
 * all Unicode strings. Note: this differs from {@code String.compareTo} which
 * uses UTF-16 code unit order. All {@code SubMap} range checks use byte order.
 * <p>
 * <b>Thread safety:</b> Not thread-safe. Concurrent structural modification
 * produces undefined behavior. Iterators fail-fast via {@code modCount}.
 * <p>
 * <b>Iterator semantics:</b> Live iterators with key reconstruction via parent
 * walk. {@code Iterator.remove()} is supported.
 * <p>
 * <b>Prefix operations:</b> {@code prefixCount}, {@code prefixWalk},
 * {@code prefixItems}, {@code prefixErase}, {@code prefixCopy}, and
 * {@code prefixSplit} provide efficient subtree operations not available
 * in standard {@code NavigableMap}.
 * <p>
 * <b>NavigableMap compliance:</b> Full implementation including {@code subMap},
 * {@code headMap}, {@code tailMap}, {@code descendingMap}, and {@code navigableKeySet}.
 *
 * @param <V> the value type
 */
public class KSTrie<V> extends AbstractMap<String, V>
                       implements NavigableMap<String, V> {

    // === Constants ===
    private static final int COMPACT_KEYSUFFIX_LIMIT = 4096;
    private static final int PARENT_ROOT = -1;
    private static final int PARENT_EOS = -2;

    private static final byte[] NO_SKIP = new byte[0];

    // Byte-order string comparison (UTF-8 unsigned byte order, matching trie sort).
    // String.compareTo uses UTF-16 code unit order which diverges for non-ASCII.
    private static int compareByteOrder(String a, String b) {
        return Arrays.compareUnsigned(
            a.getBytes(StandardCharsets.UTF_8),
            b.getBytes(StandardCharsets.UTF_8));
    }

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
            // ksUsed is actual keysuffix tail bytes (excludes F bytes), matching C++ keysuffix_used
            return c.ksUsed + (long) c.count * (1 + c.skipLen());
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
    /** Looks up a key by raw UTF-8 bytes, avoiding String encoding overhead.
     *  @return the value, or {@code null} if not found */
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
    /** Inserts or updates by raw UTF-8 bytes. @return previous value, or {@code null} */
    @SuppressWarnings("unchecked")
    public V putBytes(byte[] key, V value) {
        return putImpl(key, value, false);
    }

    /** Inserts by raw UTF-8 bytes only if absent. @return {@code true} if inserted */
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
    /** Removes an entry by raw UTF-8 bytes. @return previous value, or {@code null} */
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

        // Find common skip among entries (relative to after leaf.skip)
        byte[] entrySkip = computeCommonSkip(entries);
        int skipLen = entrySkip.length;

        // Bitmask inherits leaf.skip + common entry prefix
        byte[] bmSkip = mergeSkip(leaf.skip, entrySkip);

        // Group by dispatch byte after skip. Entries are sorted — same-dispatch
        // entries are contiguous. Walk once to find run boundaries.
        List<BuildEntry> eosGroup = new ArrayList<>();
        int[] runStart = new int[256];    // start index per dispatch byte
        int[] runCount = new int[256];    // count per dispatch byte
        int[] runBytes = new int[256];    // dispatch byte values in order
        int nRuns = 0;
        BuildEntry[] trimmed = new BuildEntry[entries.length];
        int trimmedCount = 0;

        for (BuildEntry e : entries) {
            if (e.suffixLen() <= skipLen) {
                eosGroup.add(new BuildEntry((byte) 0, 0, null, e.value()));
            } else {
                int newLen = e.suffixLen() - skipLen;
                byte newF;
                byte[] newTail;
                if (skipLen == 0) {
                    newF = e.firstByte();
                    newTail = e.tail();
                } else {
                    byte[] full = new byte[e.suffixLen()];
                    full[0] = e.firstByte();
                    if (e.tail() != null) System.arraycopy(e.tail(), 0, full, 1, e.tail().length);
                    newF = full[skipLen];
                    newTail = newLen > 1 ? Arrays.copyOfRange(full, skipLen + 1, full.length) : NO_SKIP;
                }
                int db = newF & 0xFF;
                int childLen = newLen - 1;
                byte childF = childLen > 0 && newTail != null && newTail.length > 0 ? newTail[0] : 0;
                byte[] childTail = childLen > 1 && newTail != null ? Arrays.copyOfRange(newTail, 1, newTail.length) : NO_SKIP;
                BuildEntry childEntry = new BuildEntry(childF, childLen, childTail, e.value());

                if (nRuns == 0 || runBytes[nRuns - 1] != db) {
                    runBytes[nRuns] = db;
                    runStart[nRuns] = trimmedCount;
                    runCount[nRuns] = 1;
                    nRuns++;
                } else {
                    runCount[nRuns - 1]++;
                }
                trimmed[trimmedCount++] = childEntry;
            }
        }

        // Build bitmask
        BitmaskNode bm = new BitmaskNode();
        bm.skip = bmSkip;
        int cc = nRuns;
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
        for (int r = 0; r < nRuns; r++) {
            int b = runBytes[r];
            bm.setBit(b);
            BuildEntry[] group = Arrays.copyOfRange(trimmed, runStart[r], runStart[r] + runCount[r]);
            byte[] childSkip = computeCommonSkip(group);
            // Trim childSkip from entries
            BuildEntry[] childEntries;
            if (childSkip.length > 0) {
                childEntries = trimSkip(group, childSkip.length);
            } else {
                childEntries = group;
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
        // skip already includes bm.skip (collectAllEntries prepended it)
        CompactNode c = buildCompact(skip, entries, entries.length);
        replaceNode(bm, c);
    }

    private void collectAllEntries(Node node, List<BuildEntry> out, byte[] prefix) {
        if (node == CompactNode.SENTINEL) return;
        if (node instanceof CompactNode c) {
            byte[] fullPrefix = prefix;
            if (c.skip.length > 0) fullPrefix = concat(prefix, c.skip);
            for (int i = 0; i < c.count; i++) {
                int klen = c.L(i);
                byte[] full;
                if (klen == 0) {
                    full = fullPrefix; // EOS: key is exactly the path so far
                } else {
                    byte[] suffix = new byte[klen];
                    suffix[0] = c.F(i);
                    if (klen > 1) System.arraycopy(c.data, c.ksOff() + c.O(i), suffix, 1, klen - 1);
                    full = concat(fullPrefix, suffix);
                }
                if (full.length == 0) {
                    out.add(new BuildEntry((byte) 0, 0, null, c.values[i])); // true EOS
                } else {
                    out.add(new BuildEntry(full[0], full.length,
                        full.length > 1 ? Arrays.copyOfRange(full, 1, full.length) : null,
                        c.values[i]));
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
        if (entries.length == 0) return NO_SKIP;
        // Check for EOS entry (suffixLen == 0) — no common skip possible
        for (var e : entries)
            if (e.suffixLen() == 0) return NO_SKIP;
        // Single entry: full suffix becomes skip (entry becomes EOS after trim)
        if (entries.length == 1) {
            int klen = entries[0].suffixLen();
            byte[] full = new byte[klen];
            full[0] = entries[0].firstByte();
            if (entries[0].tail() != null)
                System.arraycopy(entries[0].tail(), 0, full, 1, entries[0].tail().length);
            return full;
        }
        // Multiple entries: find common prefix of all suffixes
        byte[][] suffixes = new byte[entries.length][];
        for (int i = 0; i < entries.length; i++) {
            int klen = entries[i].suffixLen();
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
    /** {@inheritDoc} */
    @Override public int size() { return size; }
    @Override public boolean isEmpty() { return size == 0; }

    /** {@inheritDoc} */
    @Override public boolean containsKey(Object key) {
        if (!(key instanceof String s)) return false;
        return getBytes(s.getBytes(StandardCharsets.UTF_8)) != null;
    }

    /** {@inheritDoc} */
    @Override public V get(Object key) {
        if (!(key instanceof String s)) return null;
        return getBytes(s.getBytes(StandardCharsets.UTF_8));
    }

    /** {@inheritDoc} */
    @Override public V put(String key, V value) {
        return putBytes(key.getBytes(StandardCharsets.UTF_8), value);
    }

    /** {@inheritDoc} */
    @Override public V remove(Object key) {
        if (!(key instanceof String s)) return null;
        return removeBytes(s.getBytes(StandardCharsets.UTF_8));
    }

    /** {@inheritDoc} */
    @Override public void clear() { root = CompactNode.SENTINEL; size = 0; modCount++; }

    // === NavigableMap ===
    /** {@inheritDoc} */
    @Override public Comparator<? super String> comparator() { return KSTrie::compareByteOrder; }
    /** {@inheritDoc} */
    @Override public String firstKey() { CompactNode c = descendFirst(root); if (c == null) throw new NoSuchElementException(); return reconstructKey(c, 0); }
    /** {@inheritDoc} */
    @Override public String lastKey() { CompactNode c = descendLast(root); if (c == null) throw new NoSuchElementException(); return reconstructKey(c, c.count - 1); }
    /** {@inheritDoc} */
    @Override public Map.Entry<String, V> firstEntry() { CompactNode c = descendFirst(root); return c == null ? null : makeEntry(c, 0); }
    /** {@inheritDoc} */
    @Override @SuppressWarnings("unchecked") public Map.Entry<String, V> lastEntry() { CompactNode c = descendLast(root); return c == null ? null : makeEntry(c, c.count - 1); }
    @Override public Map.Entry<String, V> pollFirstEntry() { var e = firstEntry(); if (e != null) remove(e.getKey()); return e; }
    @Override public Map.Entry<String, V> pollLastEntry() { var e = lastEntry(); if (e != null) remove(e.getKey()); return e; }

    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<String, V> makeEntry(CompactNode c, int i) {
        return new SimpleImmutableEntry<>(reconstructKey(c, i), (V) c.values[i]);
    }

    // === Navigation helpers ===
    private SimpleImmutableEntry<String, V> firstEntryIn(Node node) {
        CompactNode c = descendFirst(node);
        return c != null ? makeEntry(c, 0) : null;
    }

    private SimpleImmutableEntry<String, V> lastEntryIn(Node node) {
        CompactNode c = descendLast(node);
        return c != null ? makeEntry(c, c.count - 1) : null;
    }

    private SimpleImmutableEntry<String, V> nextEntryAfter(Node node) {
        while (node.parent != null) {
            BitmaskNode bm = node.parent;
            int nextBit = (node.parentByte == PARENT_EOS)
                ? bm.findNextSet(0) : bm.findNextSet(node.parentByte + 1);
            if (nextBit >= 0) {
                var e = firstEntryIn(bm.children[bm.slotOf(nextBit)]);
                if (e != null) return e;
            }
            node = bm;
        }
        return null;
    }

    private SimpleImmutableEntry<String, V> prevEntryBefore(Node node) {
        while (node.parent != null) {
            BitmaskNode bm = node.parent;
            if (node.parentByte == PARENT_EOS) { node = bm; continue; }
            int prevBit = bm.findPrevSet(node.parentByte - 1);
            if (prevBit >= 0) {
                var e = lastEntryIn(bm.children[bm.slotOf(prevBit)]);
                if (e != null) return e;
            }
            if (bm.hasEos()) {
                var e = lastEntryIn(bm.eosChild());
                if (e != null) return e;
            }
            node = bm;
        }
        return null;
    }

    // === ceilingImpl: first entry >= key ===
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<String, V> ceilingImpl(byte[] key) {
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm.skip, key, consumed);
            if (mm >= 0) {
                int skipByte = bm.skip[mm] & 0xFF;
                int keyByte = consumed + mm < key.length ? (key[consumed + mm] & 0xFF) : -1;
                return keyByte < skipByte ? firstEntryIn(bm) : nextEntryAfter(bm);
            }
            consumed += bm.skipLen();
            if (consumed >= key.length) {
                if (bm.hasEos()) return firstEntryIn(bm.eosChild());
                int bit = bm.findNextSet(0);
                return bit >= 0 ? firstEntryIn(bm.children[bm.slotOf(bit)]) : nextEntryAfter(bm);
            }
            int b = key[consumed++] & 0xFF;
            Node child = bm.dispatch(b);
            if (child == CompactNode.SENTINEL) {
                int next = bm.findNextSet(b + 1);
                return next >= 0 ? firstEntryIn(bm.children[bm.slotOf(next)]) : nextEntryAfter(bm);
            }
            node = child;
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int mm = skipMismatch(c.skip, key, consumed);
        if (mm >= 0) {
            int skipByte = c.skip[mm] & 0xFF;
            int keyByte = consumed + mm < key.length ? (key[consumed + mm] & 0xFF) : -1;
            return keyByte < skipByte ? firstEntryIn(c) : nextEntryAfter(c);
        }
        consumed += c.skipLen();
        int[] fp = c.findPos(key, consumed);
        if (fp[0] == 1) return makeEntry(c, fp[1]);
        int pos = fp[1];
        return pos < c.count ? makeEntry(c, pos) : nextEntryAfter(c);
    }

    // === floorImpl: last entry <= key ===
    @SuppressWarnings("unchecked")
    private SimpleImmutableEntry<String, V> floorImpl(byte[] key) {
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            int mm = skipMismatch(bm.skip, key, consumed);
            if (mm >= 0) {
                int skipByte = bm.skip[mm] & 0xFF;
                int keyByte = consumed + mm < key.length ? (key[consumed + mm] & 0xFF) : -1;
                return keyByte > skipByte ? lastEntryIn(bm) : prevEntryBefore(bm);
            }
            consumed += bm.skipLen();
            if (consumed >= key.length) {
                if (bm.hasEos()) return lastEntryIn(bm.eosChild());
                return prevEntryBefore(bm);
            }
            int b = key[consumed++] & 0xFF;
            Node child = bm.dispatch(b);
            if (child == CompactNode.SENTINEL) {
                int prev = bm.findPrevSet(b - 1);
                if (prev >= 0) return lastEntryIn(bm.children[bm.slotOf(prev)]);
                if (bm.hasEos()) return lastEntryIn(bm.eosChild());
                return prevEntryBefore(bm);
            }
            node = child;
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        int mm = skipMismatch(c.skip, key, consumed);
        if (mm >= 0) {
            int skipByte = c.skip[mm] & 0xFF;
            int keyByte = consumed + mm < key.length ? (key[consumed + mm] & 0xFF) : -1;
            return keyByte > skipByte ? lastEntryIn(c) : prevEntryBefore(c);
        }
        consumed += c.skipLen();
        int[] fp = c.findPos(key, consumed);
        if (fp[0] == 1) return makeEntry(c, fp[1]);
        int pos = fp[1] - 1;
        return pos >= 0 ? makeEntry(c, pos) : prevEntryBefore(c);
    }

    // === higher/lower: find entry, advance via parent ===
    private SimpleImmutableEntry<String, V> higherImpl(byte[] key, String keyStr) {
        var e = ceilingImpl(key);
        if (e == null) return null;
        if (!e.getKey().equals(keyStr)) return e;
        // Exact match — descend to the leaf, advance past it
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm.skip, key, consumed)) return null;
            consumed += bm.skipLen();
            if (consumed >= key.length) { node = bm.eosChild(); break; }
            node = bm.dispatch(key[consumed++] & 0xFF);
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        consumed += c.skipLen();
        int pos = c.findEntry(key, consumed);
        if (pos < 0) return null;
        if (pos + 1 < c.count) return makeEntry(c, pos + 1);
        return nextEntryAfter(c);
    }

    private SimpleImmutableEntry<String, V> lowerImpl(byte[] key, String keyStr) {
        var e = floorImpl(key);
        if (e == null) return null;
        if (!e.getKey().equals(keyStr)) return e;
        // Exact match — descend, retreat
        Node node = root;
        int consumed = 0;
        while (node instanceof BitmaskNode bm) {
            if (!matchSkip(bm.skip, key, consumed)) return null;
            consumed += bm.skipLen();
            if (consumed >= key.length) { node = bm.eosChild(); break; }
            node = bm.dispatch(key[consumed++] & 0xFF);
        }
        if (node == CompactNode.SENTINEL) return null;
        CompactNode c = (CompactNode) node;
        consumed += c.skipLen();
        int pos = c.findEntry(key, consumed);
        if (pos < 0) return null;
        if (pos > 0) return makeEntry(c, pos - 1);
        return prevEntryBefore(c);
    }

    // === NavigableMap ===
    /** {@inheritDoc} */
    @Override public Map.Entry<String, V> ceilingEntry(String key) { return ceilingImpl(key.getBytes(StandardCharsets.UTF_8)); }
    /** {@inheritDoc} */
    @Override public Map.Entry<String, V> floorEntry(String key) { return floorImpl(key.getBytes(StandardCharsets.UTF_8)); }
    /** {@inheritDoc} */
    @Override public Map.Entry<String, V> higherEntry(String key) { return higherImpl(key.getBytes(StandardCharsets.UTF_8), key); }
    /** {@inheritDoc} */
    @Override public Map.Entry<String, V> lowerEntry(String key) { return lowerImpl(key.getBytes(StandardCharsets.UTF_8), key); }
    /** {@inheritDoc} */
    @Override public String floorKey(String k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
    /** {@inheritDoc} */
    @Override public String ceilingKey(String k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
    @Override public String lowerKey(String k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }
    @Override public String higherKey(String k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }

    /** {@inheritDoc} */
    /** {@inheritDoc} */
    /** {@inheritDoc} */
    @Override public NavigableSet<String> navigableKeySet() { return new KeySet(this); }
    @Override public NavigableSet<String> descendingKeySet() { return new KeySet(descendingMap()); }
    @Override public NavigableMap<String, V> descendingMap() { return new DescendingMap(); }

    // === Descending iterator ===
    private class DescIter implements Iterator<Map.Entry<String, V>> {
        CompactNode leaf;
        int index;
        int expectedModCount;
        String lastKey;
        boolean canRemove;

        DescIter() {
            expectedModCount = modCount;
            leaf = descendLast(root);
            index = (leaf != null) ? leaf.count - 1 : -1;
        }

        /** {@inheritDoc} */
        @Override public boolean hasNext() { return leaf != null && index >= 0; }

        /** {@inheritDoc} */
        @Override @SuppressWarnings("unchecked")
        public Map.Entry<String, V> next() {
            if (modCount != expectedModCount) throw new ConcurrentModificationException();
            if (!hasNext()) throw new NoSuchElementException();
            String key = reconstructKey(leaf, index);
            V val = (V) leaf.values[index];
            lastKey = key;
            canRemove = true;
            retreat();
            return new SimpleImmutableEntry<>(key, val);
        }

        private void retreat() {
            index--;
            if (index >= 0) return;
            // Walk up to previous leaf
            Node node = leaf;
            while (node.parent != null) {
                BitmaskNode bm = node.parent;
                if (node.parentByte == PARENT_EOS) {
                    // EOS is first — nothing before it at this level
                    node = bm;
                    continue;
                }
                int prevBit = bm.findPrevSet(node.parentByte - 1);
                if (prevBit >= 0) {
                    CompactNode target = descendLast(bm.children[bm.slotOf(prevBit)]);
                    if (target != null) { leaf = target; index = leaf.count - 1; return; }
                }
                if (bm.hasEos()) {
                    CompactNode target = descendLast(bm.eosChild());
                    if (target != null) { leaf = target; index = leaf.count - 1; return; }
                }
                node = bm;
            }
            leaf = null; index = -1;
        }

        /** {@inheritDoc} */
        @Override public void remove() {
            if (!canRemove) throw new IllegalStateException();
            canRemove = false;
            KSTrie.this.remove(lastKey);
            expectedModCount = modCount;
            // Refind: position at entry just before lastKey
            leaf = null; index = -1;
            var e = KSTrie.this.lowerEntry(lastKey);
            if (e == null) return;
            // Descend to that entry's leaf
            byte[] k = e.getKey().getBytes(StandardCharsets.UTF_8);
            Node node = root;
            int consumed = 0;
            while (node instanceof BitmaskNode bm) {
                if (!matchSkip(bm.skip, k, consumed)) return;
                consumed += bm.skipLen();
                if (consumed >= k.length) { node = bm.eosChild(); break; }
                node = bm.dispatch(k[consumed++] & 0xFF);
            }
            if (node instanceof CompactNode c && c != CompactNode.SENTINEL) {
                consumed += c.skipLen();
                int pos = c.findEntry(k, consumed);
                if (pos >= 0) { leaf = c; index = pos; }
            }
        }
    }

    // === KeySet: NavigableSet<String> backed by a NavigableMap ===
    private static class KeySet extends AbstractSet<String> implements NavigableSet<String> {
        private final NavigableMap<String, ?> map;
        KeySet(NavigableMap<String, ?> map) { this.map = map; }

        /** {@inheritDoc} */
        @Override public int size() { return map.size(); }
        /** {@inheritDoc} */
        @Override public boolean contains(Object o) { return map.containsKey(o); }
        /** {@inheritDoc} */
        @Override public Iterator<String> iterator() {
            var ei = map.entrySet().iterator();
            return new Iterator<>() {
                /** {@inheritDoc} */ public boolean hasNext() { return ei.hasNext(); }
                /** {@inheritDoc} */ public String next() { return ei.next().getKey(); }
                /** {@inheritDoc} */ public void remove() { ei.remove(); }
            };
        }
        /** {@inheritDoc} */
        @Override public String first() { return map.firstKey(); }
        /** {@inheritDoc} */
        @Override public String last() { return map.lastKey(); }
        /** {@inheritDoc} */
        @Override public String lower(String e) { return map.lowerKey(e); }
        /** {@inheritDoc} */
        @Override public String floor(String e) { return map.floorKey(e); }
        /** {@inheritDoc} */
        @Override public String ceiling(String e) { return map.ceilingKey(e); }
        /** {@inheritDoc} */
        @Override public String higher(String e) { return map.higherKey(e); }
        /** {@inheritDoc} */
        @Override public String pollFirst() { var e = map.pollFirstEntry(); return e == null ? null : e.getKey(); }
        /** {@inheritDoc} */
        @Override public String pollLast() { var e = map.pollLastEntry(); return e == null ? null : e.getKey(); }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> descendingSet() { return new KeySet(map.descendingMap()); }
        /** {@inheritDoc} */
        @Override public Iterator<String> descendingIterator() { return descendingSet().iterator(); }
        @Override public NavigableSet<String> subSet(String a, boolean ai, String b, boolean bi) { return new KeySet(map.subMap(a, ai, b, bi)); }
        @Override public NavigableSet<String> headSet(String t, boolean i) { return new KeySet(map.headMap(t, i)); }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> tailSet(String f, boolean i) { return new KeySet(map.tailMap(f, i)); }
        /** {@inheritDoc} */
        @Override public SortedSet<String> subSet(String a, String b) { return subSet(a, true, b, false); }
        @Override public SortedSet<String> headSet(String t) { return headSet(t, false); }
        /** {@inheritDoc} */
        @Override public SortedSet<String> tailSet(String f) { return tailSet(f, true); }
        @Override public Comparator<? super String> comparator() { return map.comparator(); }
    }

    // === DescendingMap: reversed NavigableMap view ===
    private class DescendingMap extends AbstractMap<String, V> implements NavigableMap<String, V> {
        /** {@inheritDoc} */
        @Override public int size() { return KSTrie.this.size; }
        /** {@inheritDoc} */
        @Override public boolean containsKey(Object key) { return KSTrie.this.containsKey(key); }
        /** {@inheritDoc} */
        @Override public V get(Object key) { return KSTrie.this.get(key); }
        @Override public V put(String key, V value) { return KSTrie.this.put(key, value); }
        @Override public V remove(Object key) { return KSTrie.this.remove(key); }

        /** {@inheritDoc} */
        @Override public Set<Entry<String, V>> entrySet() {
            return new AbstractSet<>() {
                /** {@inheritDoc} */
                /** {@inheritDoc} */
                @Override public int size() { return KSTrie.this.size; }
                @Override public Iterator<Entry<String, V>> iterator() { return new DescIter(); }
            };
        }

        /** {@inheritDoc} */
        /** {@inheritDoc} */
        /** {@inheritDoc} */
        @Override public Comparator<? super String> comparator() { return KSTrie.this.comparator().reversed(); }
        @Override public String firstKey() { return KSTrie.this.lastKey(); }
        @Override public String lastKey() { return KSTrie.this.firstKey(); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> firstEntry() { return KSTrie.this.lastEntry(); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> lastEntry() { return KSTrie.this.firstEntry(); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> pollFirstEntry() { return KSTrie.this.pollLastEntry(); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> pollLastEntry() { return KSTrie.this.pollFirstEntry(); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> ceilingEntry(String key) { return KSTrie.this.floorEntry(key); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> floorEntry(String key) { return KSTrie.this.ceilingEntry(key); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> higherEntry(String key) { return KSTrie.this.lowerEntry(key); }
        /** {@inheritDoc} */
        @Override public Entry<String, V> lowerEntry(String key) { return KSTrie.this.higherEntry(key); }
        /** {@inheritDoc} */
        @Override public String ceilingKey(String k) { return KSTrie.this.floorKey(k); }
        @Override public String floorKey(String k) { return KSTrie.this.ceilingKey(k); }
        @Override public String higherKey(String k) { return KSTrie.this.lowerKey(k); }
        /** {@inheritDoc} */
        @Override public String lowerKey(String k) { return KSTrie.this.higherKey(k); }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> descendingMap() { return KSTrie.this; }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> navigableKeySet() { return new KeySet(this); }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> descendingKeySet() { return KSTrie.this.navigableKeySet(); }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> subMap(String a, boolean ai, String b, boolean bi) { return KSTrie.this.subMap(b, bi, a, ai).descendingMap(); }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> headMap(String t, boolean i) { return KSTrie.this.tailMap(t, i).descendingMap(); }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> tailMap(String f, boolean i) { return KSTrie.this.headMap(f, i).descendingMap(); }
        @Override public SortedMap<String, V> subMap(String a, String b) { return subMap(a, true, b, false); }
        @Override public SortedMap<String, V> headMap(String t) { return headMap(t, false); }
        /** {@inheritDoc} */
        @Override public SortedMap<String, V> tailMap(String f) { return tailMap(f, true); }
    }

    /** {@inheritDoc} */
    @Override public NavigableMap<String, V> subMap(String fromKey, boolean fi, String toKey, boolean ti) { return new SubMap(fromKey, fi, toKey, ti); }
    /** {@inheritDoc} */
    @Override public NavigableMap<String, V> headMap(String toKey, boolean i) { return new SubMap(null, true, toKey, i); }
    /** {@inheritDoc} */
    @Override public NavigableMap<String, V> tailMap(String fromKey, boolean i) { return new SubMap(fromKey, i, null, true); }
    /** {@inheritDoc} */
    @Override public SortedMap<String, V> subMap(String a, String b) { return subMap(a, true, b, false); }
    /** {@inheritDoc} */
    @Override public SortedMap<String, V> headMap(String t) { return headMap(t, false); }
    /** {@inheritDoc} */
    @Override public SortedMap<String, V> tailMap(String f) { return tailMap(f, true); }

    // === SubMap view ===
    private class SubMap extends AbstractMap<String, V> implements NavigableMap<String, V> {
        final String fromKey, toKey;
        final boolean fromInclusive, toInclusive;

        SubMap(String from, boolean fi, String to, boolean ti) {
            this.fromKey = from; this.fromInclusive = fi;
            this.toKey = to; this.toInclusive = ti;
        }

        private boolean inRange(String key) {
            if (fromKey != null) {
                int c = compareByteOrder(key, fromKey);
                if (c < 0 || (c == 0 && !fromInclusive)) return false;
            }
            if (toKey != null) {
                int c = compareByteOrder(key, toKey);
                if (c > 0 || (c == 0 && !toInclusive)) return false;
            }
            return true;
        }

        /** {@inheritDoc} */
        @Override public V get(Object key) {
            if (!(key instanceof String k) || !inRange(k)) return null;
            return KSTrie.this.get(k);
        }
        /** {@inheritDoc} */
        @Override public V put(String key, V value) {
            if (!inRange(key)) throw new IllegalArgumentException("key out of range");
            return KSTrie.this.put(key, value);
        }
        /** {@inheritDoc} */
        @Override public int size() { int n = 0; for (var ignored : entrySet()) n++; return n; }
        /** {@inheritDoc} */
        @Override public boolean containsKey(Object key) { return key instanceof String k && inRange(k) && KSTrie.this.containsKey(k); }

        private Entry<String, V> loEntry() {
            if (fromKey == null) return KSTrie.this.firstEntry();
            return fromInclusive ? KSTrie.this.ceilingEntry(fromKey) : KSTrie.this.higherEntry(fromKey);
        }

        /** {@inheritDoc} */
        @Override public Set<Entry<String, V>> entrySet() {
            return new AbstractSet<>() {
                /** {@inheritDoc} */
                /** {@inheritDoc} */
                @Override public int size() { return SubMap.this.size(); }
                @Override public Iterator<Entry<String, V>> iterator() {
                    return new Iterator<>() {
                        Entry<String, V> next = loEntry();
                        { if (next != null && !inRange(next.getKey())) next = null; }
                        /** {@inheritDoc} */
                        /** {@inheritDoc} */
                        @Override public boolean hasNext() { return next != null; }
                        @Override public Entry<String, V> next() {
                            if (next == null) throw new NoSuchElementException();
                            Entry<String, V> r = next;
                            next = KSTrie.this.higherEntry(r.getKey());
                            if (next != null && !inRange(next.getKey())) next = null;
                            return r;
                        }
                    };
                }
            };
        }

        /** {@inheritDoc} */
        @Override public Comparator<? super String> comparator() { return KSTrie.this.comparator(); }
        /** {@inheritDoc} */
        @Override public String firstKey() { var e = loEntry(); if (e == null || !inRange(e.getKey())) throw new NoSuchElementException(); return e.getKey(); }
        /** {@inheritDoc} */
        @Override public String lastKey() {
            Entry<String, V> e = toKey != null ? (toInclusive ? KSTrie.this.floorEntry(toKey) : KSTrie.this.lowerEntry(toKey)) : KSTrie.this.lastEntry();
            if (e == null || !inRange(e.getKey())) throw new NoSuchElementException(); return e.getKey();
        }
        /** {@inheritDoc} */
        @Override public Entry<String, V> firstEntry() { var e = loEntry(); return e != null && inRange(e.getKey()) ? e : null; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> lastEntry() {
            Entry<String, V> e = toKey != null ? (toInclusive ? KSTrie.this.floorEntry(toKey) : KSTrie.this.lowerEntry(toKey)) : KSTrie.this.lastEntry();
            return e != null && inRange(e.getKey()) ? e : null;
        }
        /** {@inheritDoc} */
        @Override public Entry<String, V> pollFirstEntry() { var e = firstEntry(); if (e != null) KSTrie.this.remove(e.getKey()); return e; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> pollLastEntry() { var e = lastEntry(); if (e != null) KSTrie.this.remove(e.getKey()); return e; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> ceilingEntry(String k) { var e = KSTrie.this.ceilingEntry(k); return e != null && inRange(e.getKey()) ? e : null; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> floorEntry(String k) { var e = KSTrie.this.floorEntry(k); return e != null && inRange(e.getKey()) ? e : null; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> higherEntry(String k) { var e = KSTrie.this.higherEntry(k); return e != null && inRange(e.getKey()) ? e : null; }
        /** {@inheritDoc} */
        @Override public Entry<String, V> lowerEntry(String k) { var e = KSTrie.this.lowerEntry(k); return e != null && inRange(e.getKey()) ? e : null; }
        /** {@inheritDoc} */
        @Override public String ceilingKey(String k) { var e = ceilingEntry(k); return e == null ? null : e.getKey(); }
        /** {@inheritDoc} */
        @Override public String floorKey(String k) { var e = floorEntry(k); return e == null ? null : e.getKey(); }
        /** {@inheritDoc} */
        @Override public String higherKey(String k) { var e = higherEntry(k); return e == null ? null : e.getKey(); }
        @Override public String lowerKey(String k) { var e = lowerEntry(k); return e == null ? null : e.getKey(); }
        @Override public NavigableMap<String, V> subMap(String a, boolean ai, String b, boolean bi) {
            if (fromKey != null && compareByteOrder(a, fromKey) < 0) throw new IllegalArgumentException("fromKey out of range");
            if (toKey != null && compareByteOrder(b, toKey) > 0) throw new IllegalArgumentException("toKey out of range");
            return KSTrie.this.subMap(a, ai, b, bi);
        }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> headMap(String t, boolean i) {
            if (toKey != null && compareByteOrder(t, toKey) > 0) throw new IllegalArgumentException("toKey out of range");
            if (fromKey != null) return KSTrie.this.subMap(fromKey, fromInclusive, t, i);
            return KSTrie.this.headMap(t, i);
        }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> tailMap(String f, boolean i) {
            if (fromKey != null && compareByteOrder(f, fromKey) < 0) throw new IllegalArgumentException("fromKey out of range");
            if (toKey != null) return KSTrie.this.subMap(f, i, toKey, toInclusive);
            return KSTrie.this.tailMap(f, i);
        }
        /** {@inheritDoc} */
        @Override public SortedMap<String, V> subMap(String a, String b) { return subMap(a, true, b, false); }
        /** {@inheritDoc} */
        @Override public SortedMap<String, V> headMap(String t) { return headMap(t, false); }
        /** {@inheritDoc} */
        @Override public SortedMap<String, V> tailMap(String f) { return tailMap(f, true); }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> navigableKeySet() { return new KeySet(this); }
        /** {@inheritDoc} */
        @Override public NavigableSet<String> descendingKeySet() { return new KeySet(descendingMap()); }
        /** {@inheritDoc} */
        @Override public NavigableMap<String, V> descendingMap() {
            // Compose: parent's descending map with swapped bounds
            NavigableMap<String, V> desc = KSTrie.this.descendingMap();
            if (fromKey != null && toKey != null) return desc.subMap(toKey, toInclusive, fromKey, fromInclusive);
            if (fromKey != null) return desc.headMap(fromKey, fromInclusive);
            if (toKey != null) return desc.tailMap(toKey, toInclusive);
            return desc;
        }
    }

    // === Prefix operations ===

    /** Walks all entries whose key starts with {@code prefix} in sorted order.
     *  @param prefix the key prefix to match
     *  @param action called with (key, value) for each matching entry */
    public void prefixWalk(String prefix, BiConsumer<String, V> action) {
        prefixWalkBytes(prefix.getBytes(StandardCharsets.UTF_8), (k, v) ->
            action.accept(new String(k, StandardCharsets.UTF_8), v));
    }

    /** Byte-array variant of {@link #prefixWalk}. Avoids String encoding overhead. */
    @SuppressWarnings("unchecked")
    public void prefixWalkBytes(byte[] prefix, BiConsumer<byte[], V> action) {
        // Descend to prefix point
        Node node = root;
        int consumed = 0;
        // Accumulate path bytes for key reconstruction
        byte[] pathSoFar = new byte[0];

        while (node instanceof BitmaskNode bm) {
            if (bm.skip.length > 0) {
                int rem = prefix.length - consumed;
                if (rem < bm.skip.length) {
                    // Prefix ends within skip — check partial match
                    for (int i = 0; i < rem; i++)
                        if (bm.skip[i] != prefix[consumed + i]) return;
                    // Whole subtree matches
                    walkSubtree(node, pathSoFar, action);
                    return;
                }
                if (!matchSkip(bm.skip, prefix, consumed)) return;
                pathSoFar = concat(pathSoFar, bm.skip);
            }
            consumed += bm.skipLen();
            if (consumed >= prefix.length) { walkSubtree(node, pathSoFar, action); return; }
            int b = prefix[consumed++] & 0xFF;
            pathSoFar = concat(pathSoFar, new byte[]{(byte) b});
            node = bm.dispatch(b);
        }
        if (node == CompactNode.SENTINEL) return;
        CompactNode c = (CompactNode) node;
        if (c.skip.length > 0) {
            int rem = prefix.length - consumed;
            if (rem < c.skip.length) {
                for (int i = 0; i < rem; i++)
                    if (c.skip[i] != prefix[consumed + i]) return;
                walkSubtree(node, pathSoFar, action);
                return;
            }
            if (!matchSkip(c.skip, prefix, consumed)) return;
            pathSoFar = concat(pathSoFar, c.skip);
        }
        consumed += c.skipLen();
        if (consumed >= prefix.length) { walkSubtree(node, pathSoFar, action); return; }
        // Partial match within compact entries
        for (int i = 0; i < c.count; i++) {
            if (comparePrefixToEntry(c, i, prefix, consumed) == 0) {
                int klen = c.L(i);
                byte[] suffix = new byte[klen];
                if (klen > 0) {
                    suffix[0] = c.F(i);
                    if (klen > 1) System.arraycopy(c.data, c.ksOff() + c.O(i), suffix, 1, klen - 1);
                }
                action.accept(concat(pathSoFar, suffix), (V) c.values[i]);
            }
        }
    }

    @SuppressWarnings("unchecked")
    private void walkSubtree(Node node, byte[] prefix, BiConsumer<byte[], V> action) {
        if (node == CompactNode.SENTINEL) return;
        if (node instanceof CompactNode c) {
            byte[] fp = c.skip.length > 0 ? concat(prefix, c.skip) : prefix;
            for (int i = 0; i < c.count; i++) {
                int klen = c.L(i);
                if (klen == 0) {
                    action.accept(fp, (V) c.values[i]);
                } else {
                    byte[] suffix = new byte[klen];
                    suffix[0] = c.F(i);
                    if (klen > 1) System.arraycopy(c.data, c.ksOff() + c.O(i), suffix, 1, klen - 1);
                    action.accept(concat(fp, suffix), (V) c.values[i]);
                }
            }
            return;
        }
        BitmaskNode bm = (BitmaskNode) node;
        byte[] bp = bm.skip.length > 0 ? concat(prefix, bm.skip) : prefix;
        walkSubtree(bm.eosChild(), bp, action);
        int bit = bm.findNextSet(0);
        while (bit >= 0) {
            walkSubtree(bm.children[bm.slotOf(bit)], concat(bp, new byte[]{(byte) bit}), action);
            bit = bm.findNextSet(bit + 1);
        }
    }

    /** Returns all entries whose key starts with {@code prefix}, in sorted order.
     *  @return list of (key, value) entries; empty if no matches */
    public List<Map.Entry<String, V>> prefixItems(String prefix) {
        List<Map.Entry<String, V>> result = new ArrayList<>();
        prefixWalk(prefix, (k, v) -> result.add(new SimpleImmutableEntry<>(k, v)));
        return result;
    }

    /** Removes all entries whose key starts with {@code prefix}.
     *  @return the number of entries removed */
    public int prefixErase(String prefix) {
        return prefixEraseBytes(prefix.getBytes(StandardCharsets.UTF_8));
    }

    /** Byte-array variant of {@link #prefixErase}. */
    public int prefixEraseBytes(byte[] prefix) {
        int[] result = prefixEraseNode(root, prefix, 0);
        // result[0] = erased count, root may have changed
        if (result[0] > 0) { size -= result[0]; modCount++; }
        return result[0];
    }

    // Returns [erased_count]. Modifies tree in place, updates root if needed.
    private int[] prefixEraseNode(Node node, byte[] pfx, int consumed) {
        if (node == CompactNode.SENTINEL) return new int[]{0};

        if (node instanceof CompactNode c) {
            // Check compact skip
            if (c.skip.length > 0) {
                int rem = pfx.length - consumed;
                if (rem <= c.skip.length) {
                    // Prefix ends within or at skip
                    for (int i = 0; i < rem; i++)
                        if (c.skip[i] != pfx[consumed + i]) return new int[]{0};
                    // Whole compact matches — erase it
                    int n = c.count;
                    replaceNode(c, CompactNode.SENTINEL);
                    propagateTail(c.parent, -nodeTailTotal(c));
                    removeEmpty(c.parent);
                    return new int[]{n};
                }
                if (!matchSkip(c.skip, pfx, consumed)) return new int[]{0};
                consumed += c.skipLen();
            }
            if (consumed >= pfx.length) {
                int n = c.count;
                replaceNode(c, CompactNode.SENTINEL);
                propagateTail(c.parent, -nodeTailTotal(c));
                removeEmpty(c.parent);
                return new int[]{n};
            }
            // Filter compact entries
            return prefixEraseCompact(c, pfx, consumed);
        }

        BitmaskNode bm = (BitmaskNode) node;
        if (bm.skip.length > 0) {
            int rem = pfx.length - consumed;
            if (rem <= bm.skip.length) {
                for (int i = 0; i < rem; i++)
                    if (bm.skip[i] != pfx[consumed + i]) return new int[]{0};
                int n = countEntries(bm);
                replaceNode(bm, CompactNode.SENTINEL);
                propagateTail(bm.parent, -nodeTailTotal(bm));
                removeEmpty(bm.parent);
                return new int[]{n};
            }
            if (!matchSkip(bm.skip, pfx, consumed)) return new int[]{0};
            consumed += bm.skipLen();
        }

        if (consumed >= pfx.length) {
            int n = countEntries(bm);
            replaceNode(bm, CompactNode.SENTINEL);
            propagateTail(bm.parent, -nodeTailTotal(bm));
            removeEmpty(bm.parent);
            return new int[]{n};
        }

        // Dispatch into child
        int b = pfx[consumed++] & 0xFF;
        Node child = bm.dispatch(b);
        if (child == CompactNode.SENTINEL) return new int[]{0};

        long oldChildTail = nodeTailTotal(child);
        int[] r = prefixEraseNode(child, pfx, consumed);
        if (r[0] == 0) return new int[]{0};

        // Unwind: update totalTailBytes, check collapse
        bm.totalTailBytes -= oldChildTail - nodeTailTotal(
            bm.hasBit(b) ? bm.children[bm.slotOf(b)] : CompactNode.SENTINEL);
        propagateTail(bm.parent, -(oldChildTail - nodeTailTotal(
            bm.hasBit(b) ? bm.children[bm.slotOf(b)] : CompactNode.SENTINEL)));
        checkCoalesce(bm);
        return r;
    }

    private int[] prefixEraseCompact(CompactNode c, byte[] pfx, int consumed) {
        int rlen = pfx.length - consumed;
        // Partition entries: kept vs erased
        List<Integer> kept = new ArrayList<>();
        int erased = 0;
        for (int i = 0; i < c.count; i++) {
            if (entryMatchesPrefix(c, i, pfx, consumed, rlen)) {
                erased++;
            } else {
                kept.add(i);
            }
        }
        if (erased == 0) return new int[]{0};

        long oldTail = nodeTailTotal(c);
        if (kept.isEmpty()) {
            replaceNode(c, CompactNode.SENTINEL);
            propagateTail(c.parent, -oldTail);
            removeEmpty(c.parent);
            return new int[]{erased};
        }

        // Rebuild compact with kept entries
        BuildEntry[] entries = new BuildEntry[kept.size()];
        for (int i = 0; i < kept.size(); i++)
            entries[i] = extractSingleEntry(c, kept.get(i));
        CompactNode nc = buildCompact(c.skip, entries, entries.length);
        nc.parent = c.parent; nc.parentByte = c.parentByte;
        replaceNode(c, nc);
        propagateTail(nc.parent, nodeTailTotal(nc) - oldTail);
        checkCoalesce(nc.parent);
        return new int[]{erased};
    }

    /** Deep-clones all entries whose key starts with {@code prefix} into a new trie.
     *  The source trie is not modified.
     *  @return a new KSTrie containing only the matching entries */
    public KSTrie<V> prefixCopy(String prefix) {
        return prefixCopyBytes(prefix.getBytes(StandardCharsets.UTF_8));
    }

    /** Byte-array variant of {@link #prefixCopy}. */
    @SuppressWarnings("unchecked")
    public KSTrie<V> prefixCopyBytes(byte[] prefix) {
        KSTrie<V> result = new KSTrie<>();
        Node node = root;
        int consumed = 0;
        int pathLen = 0; // bytes consumed by dispatch + skip before entering current node

        while (node instanceof BitmaskNode bm) {
            if (bm.skip.length > 0) {
                int rem = prefix.length - consumed;
                if (rem <= bm.skip.length) {
                    for (int i = 0; i < rem; i++)
                        if (bm.skip[i] != prefix[consumed + i]) return result;
                    result.root = deepClone(bm);
                    reskipRoot(result, prefix, pathLen);
                    result.size = countEntries(result.root);
                    return result;
                }
                if (!matchSkip(bm.skip, prefix, consumed)) return result;
                consumed += bm.skipLen();
            }
            if (consumed >= prefix.length) {
                result.root = deepClone(bm);
                reskipRoot(result, prefix, pathLen);
                result.size = countEntries(result.root);
                return result;
            }
            pathLen = consumed + 1; // dispatch byte included
            int b = prefix[consumed++] & 0xFF;
            node = bm.dispatch(b);
        }
        if (node == CompactNode.SENTINEL) return result;
        CompactNode c = (CompactNode) node;
        if (c.skip.length > 0) {
            int rem = prefix.length - consumed;
            if (rem <= c.skip.length) {
                for (int i = 0; i < rem; i++)
                    if (c.skip[i] != prefix[consumed + i]) return result;
                result.root = deepClone(c);
                reskipRoot(result, prefix, pathLen);
                result.size = countEntries(result.root);
                return result;
            }
            if (!matchSkip(c.skip, prefix, consumed)) return result;
            consumed += c.skipLen();
        }
        if (consumed >= prefix.length) {
            result.root = deepClone(c);
            reskipRoot(result, prefix, pathLen);
            result.size = countEntries(result.root);
            return result;
        }
        // Filter compact entries — entries get prefix prepended
        result.root = prefixCopyCompact(c, prefix, consumed, pathLen);
        result.size = countEntries(result.root);
        return result;
    }

    private Node prefixCopyCompact(CompactNode c, byte[] pfx, int consumed, int pathLen) {
        int rlen = pfx.length - consumed;
        List<BuildEntry> matched = new ArrayList<>();
        for (int i = 0; i < c.count; i++) {
            if (entryMatchesPrefix(c, i, pfx, consumed, rlen))
                matched.add(extractSingleEntry(c, i));
        }
        if (matched.isEmpty()) return CompactNode.SENTINEL;
        // Build with original skip, then reskip
        CompactNode nc = buildCompact(c.skip, matched.toArray(new BuildEntry[0]), matched.size());
        // Prepend path to root's skip
        byte[] pathPrefix = pathLen > 0 ? Arrays.copyOf(pfx, pathLen) : NO_SKIP;
        nc.skip = mergeSkip(pathPrefix, nc.skip);
        return nc;
    }

    // Prepend consumed path bytes to a cloned/stolen root's skip
    private void reskipRoot(KSTrie<V> trie, byte[] prefix, int pathLen) {
        if (pathLen == 0 || trie.root == CompactNode.SENTINEL) return;
        byte[] pathPrefix = Arrays.copyOf(prefix, pathLen);
        if (trie.root instanceof CompactNode c) {
            c.skip = mergeSkip(pathPrefix, c.skip);
        } else if (trie.root instanceof BitmaskNode bm) {
            bm.skip = mergeSkip(pathPrefix, bm.skip);
        }
    }

    /** Removes all entries whose key starts with {@code prefix} from this trie and
     *  returns them in a new trie. Zero-copy where possible (subtree pointer steal).
     *  @return a new KSTrie containing the extracted entries */
    public KSTrie<V> prefixSplit(String prefix) {
        return prefixSplitBytes(prefix.getBytes(StandardCharsets.UTF_8));
    }

    /** Byte-array variant of {@link #prefixSplit}. */
    public KSTrie<V> prefixSplitBytes(byte[] prefix) {
        KSTrie<V> stolen = new KSTrie<>();
        int[] result = prefixSplitNode(root, prefix, 0, 0, stolen);
        // result = [count_stolen, pathLen]
        if (result[0] > 0) {
            size -= result[0]; modCount++;
            reskipRoot(stolen, prefix, result[1]);
        }
        return stolen;
    }

    // Returns [count_stolen, pathLen].
    private int[] prefixSplitNode(Node node, byte[] pfx, int consumed, int pathLen, KSTrie<V> stolen) {
        if (node == CompactNode.SENTINEL) return new int[]{0, 0};

        if (node instanceof CompactNode c) {
            if (c.skip.length > 0) {
                int rem = pfx.length - consumed;
                if (rem <= c.skip.length) {
                    for (int i = 0; i < rem; i++)
                        if (c.skip[i] != pfx[consumed + i]) return new int[]{0, 0};
                    int n = c.count;
                    detachNode(c);
                    stolen.root = c;
                    stolen.size = n;
                    c.parent = null; c.parentByte = PARENT_ROOT;
                    return new int[]{n, pathLen};
                }
                if (!matchSkip(c.skip, pfx, consumed)) return new int[]{0, 0};
                consumed += c.skipLen();
            }
            if (consumed >= pfx.length) {
                int n = c.count;
                detachNode(c);
                stolen.root = c;
                stolen.size = n;
                c.parent = null; c.parentByte = PARENT_ROOT;
                return new int[]{n, pathLen};
            }
            return prefixSplitCompact(c, pfx, consumed, pathLen, stolen);
        }

        BitmaskNode bm = (BitmaskNode) node;
        if (bm.skip.length > 0) {
            int rem = pfx.length - consumed;
            if (rem <= bm.skip.length) {
                for (int i = 0; i < rem; i++)
                    if (bm.skip[i] != pfx[consumed + i]) return new int[]{0, 0};
                int n = countEntries(bm);
                detachNode(bm);
                stolen.root = bm;
                stolen.size = n;
                bm.parent = null; bm.parentByte = PARENT_ROOT;
                return new int[]{n, pathLen};
            }
            if (!matchSkip(bm.skip, pfx, consumed)) return new int[]{0, 0};
            consumed += bm.skipLen();
        }
        if (consumed >= pfx.length) {
            int n = countEntries(bm);
            detachNode(bm);
            stolen.root = bm;
            stolen.size = n;
            bm.parent = null; bm.parentByte = PARENT_ROOT;
            return new int[]{n, pathLen};
        }

        int b = pfx[consumed++] & 0xFF;
        Node child = bm.dispatch(b);
        if (child == CompactNode.SENTINEL) return new int[]{0, 0};

        long oldChildTail = nodeTailTotal(child);
        int[] r = prefixSplitNode(child, pfx, consumed, consumed, stolen);
        if (r[0] == 0) return new int[]{0, 0};

        long newChildTail = bm.hasBit(b) ? nodeTailTotal(bm.children[bm.slotOf(b)]) : 0;
        long delta = -(oldChildTail - newChildTail);
        bm.totalTailBytes += delta;
        propagateTail(bm.parent, delta);
        checkCoalesce(bm);
        return r;
    }

    private int[] prefixSplitCompact(CompactNode c, byte[] pfx, int consumed, int pathLen, KSTrie<V> stolen) {
        int rlen = pfx.length - consumed;
        List<Integer> keptIdx = new ArrayList<>();
        List<BuildEntry> stolenEntries = new ArrayList<>();
        for (int i = 0; i < c.count; i++) {
            if (entryMatchesPrefix(c, i, pfx, consumed, rlen))
                stolenEntries.add(extractSingleEntry(c, i));
            else
                keptIdx.add(i);
        }
        if (stolenEntries.isEmpty()) return new int[]{0, 0};

        int nstolen = stolenEntries.size();
        // Build stolen with skip, reskip happens at top level
        stolen.root = buildCompact(c.skip, stolenEntries.toArray(new BuildEntry[0]), nstolen);
        stolen.size = nstolen;

        long oldTail = nodeTailTotal(c);
        if (keptIdx.isEmpty()) {
            replaceNode(c, CompactNode.SENTINEL);
            propagateTail(c.parent, -oldTail);
            removeEmpty(c.parent);
        } else {
            BuildEntry[] kept = new BuildEntry[keptIdx.size()];
            for (int i = 0; i < keptIdx.size(); i++)
                kept[i] = extractSingleEntry(c, keptIdx.get(i));
            CompactNode nc = buildCompact(c.skip, kept, kept.length);
            nc.parent = c.parent; nc.parentByte = c.parentByte;
            replaceNode(c, nc);
            propagateTail(nc.parent, nodeTailTotal(nc) - oldTail);
            checkCoalesce(nc.parent);
        }
        return new int[]{nstolen, pathLen};
    }

    // === Prefix helpers ===

    private boolean entryMatchesPrefix(CompactNode c, int i, byte[] pfx, int consumed, int rlen) {
        int klen = c.L(i);
        if (rlen == 0) return true; // empty remaining prefix matches all
        if (klen == 0) return false; // EOS entry, but prefix has more bytes
        if (klen < rlen) return false;
        if ((c.F(i) & 0xFF) != (pfx[consumed] & 0xFF)) return false;
        if (rlen <= 1) return true;
        int off = c.ksOff() + c.O(i);
        for (int j = 0; j < rlen - 1; j++)
            if ((c.data[off + j] & 0xFF) != (pfx[consumed + 1 + j] & 0xFF)) return false;
        return true;
    }

    private BuildEntry extractSingleEntry(CompactNode c, int i) {
        int klen = c.L(i);
        byte fb = klen > 0 ? c.F(i) : 0;
        byte[] tail = null;
        if (klen > 1) {
            int off = c.O(i);
            tail = Arrays.copyOfRange(c.data, c.ksOff() + off, c.ksOff() + off + klen - 1);
        }
        return new BuildEntry(fb, klen, tail, c.values[i]);
    }

    @SuppressWarnings("unchecked")
    private Node deepClone(Node node) {
        if (node == CompactNode.SENTINEL) return CompactNode.SENTINEL;
        if (node instanceof CompactNode c) {
            CompactNode nc = new CompactNode();
            nc.skip = c.skip; // NO_SKIP or shared immutable — safe
            nc.data = c.data.clone();
            nc.values = c.values.clone();
            nc.count = c.count;
            nc.capacity = c.capacity;
            nc.ksUsed = c.ksUsed;
            return nc;
        }
        BitmaskNode bm = (BitmaskNode) node;
        BitmaskNode nb = new BitmaskNode();
        nb.skip = bm.skip;
        nb.b0 = bm.b0; nb.b1 = bm.b1; nb.b2 = bm.b2; nb.b3 = bm.b3;
        nb.totalTailBytes = bm.totalTailBytes;
        nb.children = new Node[bm.children.length];
        nb.children[0] = CompactNode.SENTINEL;
        for (int i = 1; i < bm.children.length - 1; i++) {
            nb.children[i] = deepClone(bm.children[i]);
            nb.children[i].parent = nb;
            nb.children[i].parentByte = bm.children[i].parentByte;
        }
        Node eosClone = deepClone(bm.eosChild());
        nb.children[nb.children.length - 1] = eosClone;
        if (eosClone != CompactNode.SENTINEL) {
            eosClone.parent = nb; eosClone.parentByte = PARENT_EOS;
        }
        return nb;
    }

    private void detachNode(Node node) {
        if (node.parent == null) { root = CompactNode.SENTINEL; return; }
        BitmaskNode p = node.parent;
        if (node.parentByte == PARENT_EOS) p.setEosChild(CompactNode.SENTINEL);
        else { p.removeChild(node.parentByte); }
    }

    // === entrySet + Iterator ===
    /** {@inheritDoc} */
    @Override public Set<Map.Entry<String, V>> entrySet() {
        return new AbstractSet<>() {
            /** {@inheritDoc} */
            @Override public int size() { return KSTrie.this.size; }
            /** {@inheritDoc} */
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

        /** {@inheritDoc} */
        @Override public boolean hasNext() { return leaf != null && index < leaf.count; }

        /** {@inheritDoc} */
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

        /** {@inheritDoc} */
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
    /** Counts entries whose key starts with {@code prefix} without materializing them.
     *  @return the number of matching entries */
    public int prefixCount(String prefix) {
        return prefixCountBytes(prefix.getBytes(StandardCharsets.UTF_8));
    }

    /** Byte-array variant of {@link #prefixCount}. */
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
        if (klen < rem) return 1; // entry too short to match prefix
        int fb = c.F(i) & 0xFF;
        int pb = prefix[consumed] & 0xFF;
        if (fb != pb) return fb > pb ? -1 : 1;
        int prefTail = rem - 1;
        int off = c.ksOff() + c.O(i);
        for (int j = 0; j < prefTail; j++) {
            int a = c.data[off + j] & 0xFF;
            int b = prefix[consumed + 1 + j] & 0xFF;
            if (a != b) return a > b ? -1 : 1;
        }
        return 0; // entry's suffix starts with the remaining prefix
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

        // Navigation: floor/ceiling/lower/higher
        t.clear();
        for (String s : new String[]{"apple", "banana", "cherry", "date", "elderberry"})
            t.put(s, s.length());

        assert t.ceilingKey("apple").equals("apple");
        assert t.ceilingKey("b").equals("banana");
        assert t.ceilingKey("cat").equals("cherry");
        assert t.ceilingKey("f") == null;
        assert t.ceilingKey("").equals("apple");

        assert t.floorKey("apple").equals("apple");
        assert t.floorKey("b").equals("apple");
        assert t.floorKey("cat").equals("banana");
        assert t.floorKey("zzz").equals("elderberry");
        assert t.floorKey("a") == null;

        assert t.higherKey("apple").equals("banana");
        assert t.higherKey("elderberry") == null;
        assert t.higherKey("b").equals("banana");

        assert t.lowerKey("apple") == null;
        assert t.lowerKey("banana").equals("apple");
        assert t.lowerKey("cat").equals("banana");
        System.out.println("Navigation (floor/ceil/lower/higher): PASS"); System.out.flush();

        // Verify navigation against TreeMap at scale
        t.clear();
        TreeMap<String, Integer> navRef = new TreeMap<>();
        var navRng = new java.util.Random(789);
        for (int i = 0; i < 5000; i++) {
            int len = 3 + navRng.nextInt(10);
            var sb = new StringBuilder();
            for (int j = 0; j < len; j++) sb.append((char)('a' + navRng.nextInt(26)));
            String k = sb.toString();
            t.put(k, i); navRef.put(k, i);
        }
        navRng = new java.util.Random(321);
        int navChecked = 0;
        for (int i = 0; i < 1000; i++) {
            int len = 3 + navRng.nextInt(10);
            var sb = new StringBuilder();
            for (int j = 0; j < len; j++) sb.append((char)('a' + navRng.nextInt(26)));
            String probe = sb.toString();
            assert Objects.equals(t.ceilingKey(probe), navRef.ceilingKey(probe))
                : "ceiling(" + probe + "): " + t.ceilingKey(probe) + " != " + navRef.ceilingKey(probe);
            assert Objects.equals(t.floorKey(probe), navRef.floorKey(probe))
                : "floor(" + probe + "): " + t.floorKey(probe) + " != " + navRef.floorKey(probe);
            assert Objects.equals(t.higherKey(probe), navRef.higherKey(probe))
                : "higher(" + probe + "): " + t.higherKey(probe) + " != " + navRef.higherKey(probe);
            assert Objects.equals(t.lowerKey(probe), navRef.lowerKey(probe))
                : "lower(" + probe + "): " + t.lowerKey(probe) + " != " + navRef.lowerKey(probe);
            navChecked++;
        }
        System.out.println("Navigation vs TreeMap: PASS (" + navChecked + " probes)"); System.out.flush();

        // SubMap view
        t.clear();
        String[] fruits = {"apple", "apricot", "banana", "blueberry", "cherry", "date"};
        for (int i = 0; i < fruits.length; i++) t.put(fruits[i], i);
        var sub = t.subMap("apricot", true, "cherry", false);
        assert sub.size() == 3 : "subMap size=" + sub.size(); // apricot, banana, blueberry
        assert sub.firstKey().equals("apricot");
        assert sub.lastKey().equals("blueberry");
        assert sub.containsKey("banana");
        assert !sub.containsKey("cherry");
        assert !sub.containsKey("apple");

        var head = t.headMap("banana", false);
        assert head.size() == 2; // apple, apricot
        var tail = t.tailMap("cherry", true);
        assert tail.size() == 2; // cherry, date
        System.out.println("SubMap/HeadMap/TailMap: PASS"); System.out.flush();

        // Prefix walk
        t.clear();
        t.put("/api/users/1", 1);
        t.put("/api/users/2", 2);
        t.put("/api/orders/1", 3);
        t.put("/static/main.css", 4);
        List<String> walked = new ArrayList<>();
        t.prefixWalk("/api/users/", (k, v) -> walked.add(k));
        assert walked.size() == 2 : "walk size=" + walked.size();
        assert walked.get(0).equals("/api/users/1");
        assert walked.get(1).equals("/api/users/2");

        var items = t.prefixItems("/api/");
        assert items.size() == 3;

        walked.clear();
        t.prefixWalk("", (k, v) -> walked.add(k));
        assert walked.size() == 4 : "walk all=" + walked.size();
        System.out.println("Prefix walk/items: PASS"); System.out.flush();

        // Prefix erase
        t.clear();
        t.put("/api/users/1", 1);
        t.put("/api/users/2", 2);
        t.put("/api/orders/1", 3);
        t.put("/static/main.css", 4);
        t.put("/static/app.js", 5);
        assert t.size() == 5;
        int erased = t.prefixErase("/api/users/");
        assert erased == 2 : "erased=" + erased;
        assert t.size() == 3;
        assert t.get("/api/users/1") == null;
        assert t.get("/api/users/2") == null;
        assert t.get("/api/orders/1") == 3;

        erased = t.prefixErase("/static/");
        assert erased == 2;
        assert t.size() == 1;

        erased = t.prefixErase("/nonexistent/");
        assert erased == 0;
        assert t.size() == 1;

        // Erase all
        erased = t.prefixErase("");
        assert erased == 1;
        assert t.size() == 0;
        System.out.println("Prefix erase: PASS"); System.out.flush();

        // Prefix copy
        t.clear();
        t.put("/api/users/1", 1);
        t.put("/api/users/2", 2);
        t.put("/api/orders/1", 3);
        t.put("/static/main.css", 4);
        KSTrie<Integer> copied = t.prefixCopy("/api/users/");
        assert copied.size() == 2;
        assert copied.get("/api/users/1") == 1;
        assert copied.get("/api/users/2") == 2;
        assert copied.get("/api/orders/1") == null;
        // Source unchanged
        assert t.size() == 4;
        assert t.get("/api/users/1") == 1;

        // Copy no match
        KSTrie<Integer> empty = t.prefixCopy("/nonexistent/");
        assert empty.size() == 0;

        // Copy all
        KSTrie<Integer> all = t.prefixCopy("");
        assert all.size() == 4;
        System.out.println("Prefix copy: PASS"); System.out.flush();

        // Prefix split
        t.clear();
        t.put("/api/users/1", 1);
        t.put("/api/users/2", 2);
        t.put("/api/orders/1", 3);
        t.put("/static/main.css", 4);
        t.put("/static/app.js", 5);
        KSTrie<Integer> split = t.prefixSplit("/api/users/");
        assert split.size() == 2 : "split.size=" + split.size();
        assert split.get("/api/users/1") == 1;
        assert split.get("/api/users/2") == 2;
        // Source lost the split entries
        assert t.size() == 3 : "t.size=" + t.size();
        assert t.get("/api/users/1") == null;
        assert t.get("/api/orders/1") == 3;
        assert t.get("/static/main.css") == 4;

        // Split no match
        KSTrie<Integer> splitNone = t.prefixSplit("/nonexistent/");
        assert splitNone.size() == 0;
        assert t.size() == 3;

        // Split all
        KSTrie<Integer> splitAll = t.prefixSplit("");
        assert splitAll.size() == 3;
        assert t.size() == 0;
        System.out.println("Prefix split: PASS"); System.out.flush();

        // Prefix ops at scale vs TreeMap
        t.clear();
        TreeMap<String, Integer> pfxRef = new TreeMap<>();
        var pfxRng = new java.util.Random(42);
        String[] pfxPrefixes = {"/api/v1/", "/api/v2/", "/static/", "/admin/"};
        for (int i = 0; i < 10000; i++) {
            String pfx = pfxPrefixes[pfxRng.nextInt(pfxPrefixes.length)];
            int slen = 4 + pfxRng.nextInt(8);
            var sb2 = new StringBuilder(pfx);
            for (int j = 0; j < slen; j++) sb2.append((char)('a' + pfxRng.nextInt(26)));
            t.put(sb2.toString(), i); pfxRef.put(sb2.toString(), i);
        }
        // Copy /api/v1/
        KSTrie<Integer> v1copy = t.prefixCopy("/api/v1/");
        int refV1Count = (int) pfxRef.keySet().stream().filter(k -> k.startsWith("/api/v1/")).count();
        assert v1copy.size() == refV1Count : "v1copy.size=" + v1copy.size() + " ref=" + refV1Count;
        assert t.size() == pfxRef.size(); // source unchanged

        // Split /static/
        int refStaticCount = (int) pfxRef.keySet().stream().filter(k -> k.startsWith("/static/")).count();
        KSTrie<Integer> staticSplit = t.prefixSplit("/static/");
        assert staticSplit.size() == refStaticCount;
        assert t.size() == pfxRef.size() - refStaticCount;

        // Erase /admin/
        int refAdminCount = (int) pfxRef.keySet().stream().filter(k -> k.startsWith("/admin/")).count();
        int adminErased = t.prefixErase("/admin/");
        assert adminErased == refAdminCount;
        System.out.println("Prefix ops at scale: PASS"); System.out.flush();

        // Descending map
        t.clear();
        t.put("apple", 1); t.put("banana", 2); t.put("cherry", 3);
        var desc = t.descendingMap();
        assert desc.firstKey().equals("cherry");
        assert desc.lastKey().equals("apple");
        var descIt = desc.entrySet().iterator();
        assert descIt.next().getKey().equals("cherry");
        assert descIt.next().getKey().equals("banana");
        assert descIt.next().getKey().equals("apple");
        assert !descIt.hasNext();
        // Round-trip
        assert desc.descendingMap() == t;
        System.out.println("DescendingMap: PASS"); System.out.flush();

        // NavigableKeySet
        var ks = t.navigableKeySet();
        assert ks.size() == 3;
        assert ks.first().equals("apple");
        assert ks.last().equals("cherry");
        assert ks.contains("banana");
        assert !ks.contains("date");
        assert ks.ceiling("b").equals("banana");
        assert ks.floor("b").equals("apple");
        assert ks.higher("banana").equals("cherry");
        assert ks.lower("banana").equals("apple");
        System.out.println("NavigableKeySet: PASS"); System.out.flush();

        // DescendingKeySet
        var dks = t.descendingKeySet();
        var dksIt = dks.iterator();
        assert dksIt.next().equals("cherry");
        assert dksIt.next().equals("banana");
        assert dksIt.next().equals("apple");
        assert !dksIt.hasNext();
        System.out.println("DescendingKeySet: PASS"); System.out.flush();

        // Descending at scale vs TreeMap
        t.clear();
        TreeMap<String, Integer> descRef = new TreeMap<>();
        var descRng = new java.util.Random(555);
        for (int i = 0; i < 5000; i++) {
            int len = 3 + descRng.nextInt(10);
            var sb = new StringBuilder();
            for (int j = 0; j < len; j++) sb.append((char)('a' + descRng.nextInt(26)));
            t.put(sb.toString(), i); descRef.put(sb.toString(), i);
        }
        var descTrieIt = t.descendingMap().entrySet().iterator();
        var descRefIt = descRef.descendingMap().entrySet().iterator();
        int descChecked = 0;
        while (descTrieIt.hasNext() && descRefIt.hasNext()) {
            var te = descTrieIt.next(); var re = descRefIt.next();
            assert te.getKey().equals(re.getKey()) : "desc key " + te.getKey() + " != " + re.getKey() + " at " + descChecked;
            descChecked++;
        }
        assert !descTrieIt.hasNext() && !descRefIt.hasNext();
        System.out.println("Descending vs TreeMap: PASS (" + descChecked + " entries)"); System.out.flush();

        System.out.println("\nALL PASS"); System.out.flush();
    }
}
