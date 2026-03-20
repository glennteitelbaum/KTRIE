// KNTrieBench.java — compare KNTrie vs TreeMap vs HashMap
//
// javac KNTrie.java KNTrieBench.java && java -ea -Xmx2g KNTrieBench

import java.util.*;

public class KNTrieBench {

    static final int WARMUP = 8;
    static final int RUNS = 7;
    static final int[] SIZES = {1_000, 10_000, 100_000};

    static long[] randomKeys(int n, long seed) {
        var rng = new Random(seed);
        var keys = new long[n];
        for (int i = 0; i < n; i++) keys[i] = rng.nextLong();
        return keys;
    }

    static long[] sequentialKeys(int n) {
        var keys = new long[n];
        for (int i = 0; i < n; i++) keys[i] = i;
        return keys;
    }

    // ========================================================================
    // Memory estimation
    // ========================================================================

    static long usedMemory() {
        for (int i = 0; i < 5; i++) { System.gc(); try { Thread.sleep(50); } catch (Exception e) {} }
        return Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory();
    }

    // ========================================================================
    // Benchmarks
    // ========================================================================

    static long benchPut(Map<Long, Integer> map, long[] keys) {
        long t0 = System.nanoTime();
        for (int i = 0; i < keys.length; i++) map.put(keys[i], i);
        return System.nanoTime() - t0;
    }

    static long benchGet(Map<Long, Integer> map, long[] keys) {
        long t0 = System.nanoTime();
        int found = 0;
        for (long k : keys) if (map.get(k) != null) found++;
        long elapsed = System.nanoTime() - t0;
        if (found != keys.length) throw new AssertionError("missed " + (keys.length - found));
        return elapsed;
    }

    static long benchGetMiss(Map<Long, Integer> map, long[] missKeys) {
        long t0 = System.nanoTime();
        int found = 0;
        for (long k : missKeys) if (map.get(k) != null) found++;
        long elapsed = System.nanoTime() - t0;
        if (found != 0) throw new AssertionError("unexpected hit");
        return elapsed;
    }

    static long benchIterate(Map<Long, Integer> map) {
        long t0 = System.nanoTime();
        long sum = 0;
        for (var e : map.entrySet()) sum += e.getKey() + e.getValue();
        return System.nanoTime() - t0;
    }

    static long benchRemove(Map<Long, Integer> map, long[] keys) {
        long t0 = System.nanoTime();
        for (long k : keys) map.remove(k);
        return System.nanoTime() - t0;
    }

    // ========================================================================
    // Run one size
    // ========================================================================

    static void runSize(int n) {
        long[] keys = randomKeys(n, 42);
        long[] missKeys = randomKeys(n, 999);  // different seed → almost all misses

        System.out.printf("\n=== N = %,d (random keys) ===%n", n);
        System.out.printf("%-14s %10s %10s %10s %10s %10s %10s%n",
            "Container", "put ns/op", "get ns/op", "miss ns/op", "iter ms", "rm ns/op", "MB");

        for (String name : new String[]{"KNTrie", "TreeMap", "HashMap"}) {
            // Warm all paths: put, get, miss, iterate, remove
            for (int w = 0; w < WARMUP; w++) {
                Map<Long, Integer> m = makeMap(name);
                benchPut(m, keys);
                benchGet(m, keys);
                benchGetMiss(m, missKeys);
                benchIterate(m);
                benchRemove(m, keys);
            }

            // Memory: insert, measure, then benchmark
            long memBefore = usedMemory();
            Map<Long, Integer> m = makeMap(name);
            benchPut(m, keys);
            long memAfter = usedMemory();
            double mb = (memAfter - memBefore) / (1024.0 * 1024.0);

            // Timed runs
            long bestPut = Long.MAX_VALUE, bestGet = Long.MAX_VALUE;
            long bestMiss = Long.MAX_VALUE, bestIter = Long.MAX_VALUE;
            long bestRm = Long.MAX_VALUE;

            for (int r = 0; r < RUNS; r++) {
                m.clear();
                bestPut = Math.min(bestPut, benchPut(m, keys));
                bestGet = Math.min(bestGet, benchGet(m, keys));
                bestMiss = Math.min(bestMiss, benchGetMiss(m, missKeys));
                if (m instanceof NavigableMap || m instanceof TreeMap)
                    bestIter = Math.min(bestIter, benchIterate(m));
                else
                    bestIter = Math.min(bestIter, benchIterate(m));
                // Remove bench: re-insert then remove
                m.clear(); benchPut(m, keys);
                bestRm = Math.min(bestRm, benchRemove(m, keys));
            }

            System.out.printf("%-14s %10.0f %10.0f %10.0f %10.1f %10.0f %10.1f%n",
                name,
                (double) bestPut / n,
                (double) bestGet / n,
                (double) bestMiss / n,
                bestIter / 1_000_000.0,
                (double) bestRm / n,
                mb);
        }
    }

    @SuppressWarnings("unchecked")
    static Map<Long, Integer> makeMap(String name) {
        return switch (name) {
            case "KNTrie" -> new KNTrie<>();
            case "TreeMap" -> new TreeMap<>();
            case "HashMap" -> new HashMap<>();
            default -> throw new IllegalArgumentException(name);
        };
    }

    // ========================================================================
    // Sequential keys (best case for trie prefix capture)
    // ========================================================================

    static void runSequential(int n) {
        long[] keys = sequentialKeys(n);

        System.out.printf("\n=== N = %,d (sequential keys) ===%n", n);
        System.out.printf("%-14s %10s %10s %10s%n",
            "Container", "put ns/op", "get ns/op", "MB");

        for (String name : new String[]{"KNTrie", "TreeMap", "HashMap"}) {
            for (int w = 0; w < WARMUP; w++) {
                Map<Long, Integer> m = makeMap(name);
                benchPut(m, keys);
                benchGet(m, keys);
                benchRemove(m, keys);
            }

            long memBefore = usedMemory();
            Map<Long, Integer> m = makeMap(name);
            benchPut(m, keys);
            long memAfter = usedMemory();
            double mb = (memAfter - memBefore) / (1024.0 * 1024.0);

            long bestPut = Long.MAX_VALUE, bestGet = Long.MAX_VALUE;
            for (int r = 0; r < RUNS; r++) {
                m.clear();
                bestPut = Math.min(bestPut, benchPut(m, keys));
                bestGet = Math.min(bestGet, benchGet(m, keys));
            }

            System.out.printf("%-14s %10.0f %10.0f %10.1f%n",
                name, (double) bestPut / n, (double) bestGet / n, mb);
        }
    }

    // ========================================================================
    // Main
    // ========================================================================

    public static void main(String[] args) {
        System.out.println("KNTrie Benchmark — KNTrie vs TreeMap vs HashMap");
        System.out.println("JVM: " + System.getProperty("java.vm.name") + " " + System.getProperty("java.version"));
        System.out.println("Warmup=" + WARMUP + " Runs=" + RUNS + " (best of " + RUNS + ")\n");

        for (int n : SIZES) runSize(n);

        System.out.println("\n--- Sequential keys (prefix capture advantage) ---");
        for (int n : SIZES) runSequential(n);
    }
}
