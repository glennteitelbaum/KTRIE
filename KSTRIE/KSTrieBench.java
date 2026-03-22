// KSTrieBench.java — compare KSTrie vs TreeMap vs HashMap
//
// javac KSTrie.java KSTrieBench.java && java -ea -Xmx2g KSTrieBench

import java.util.*;
import java.util.stream.*;

public class KSTrieBench {

    static final int WARMUP = 8;
    static final int RUNS = 7;
    static final int[] SIZES = {1_000, 10_000, 100_000};

    // === Key generators ===

    static String[] randomKeys(int n, long seed, int minLen, int maxLen) {
        var rng = new Random(seed);
        var keys = new String[n];
        for (int i = 0; i < n; i++) {
            int len = minLen + rng.nextInt(maxLen - minLen + 1);
            var sb = new StringBuilder(len);
            for (int j = 0; j < len; j++) sb.append((char) ('a' + rng.nextInt(26)));
            keys[i] = sb.toString();
        }
        return keys;
    }

    static String[] urlKeys(int n, long seed) {
        var rng = new Random(seed);
        String[] prefixes = {
            "/api/v1/users/", "/api/v1/orders/", "/api/v1/products/",
            "/api/v2/users/", "/api/v2/orders/", "/api/v2/products/",
            "/static/css/", "/static/js/", "/static/img/",
            "/admin/settings/", "/admin/users/", "/admin/logs/"
        };
        var keys = new String[n];
        for (int i = 0; i < n; i++) {
            String pfx = prefixes[rng.nextInt(prefixes.length)];
            int suffLen = 4 + rng.nextInt(12);
            var sb = new StringBuilder(pfx);
            for (int j = 0; j < suffLen; j++) sb.append((char) ('a' + rng.nextInt(26)));
            keys[i] = sb.toString();
        }
        return keys;
    }

    // === Memory ===

    static long usedMemory() {
        for (int i = 0; i < 5; i++) { System.gc(); try { Thread.sleep(50); } catch (Exception e) {} }
        return Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory();
    }

    // === Benchmarks ===

    static long benchPut(Map<String, Integer> map, String[] keys) {
        long t0 = System.nanoTime();
        for (int i = 0; i < keys.length; i++) map.put(keys[i], i);
        return System.nanoTime() - t0;
    }

    static long benchGet(Map<String, Integer> map, String[] keys) {
        long t0 = System.nanoTime();
        int found = 0;
        for (String k : keys) if (map.get(k) != null) found++;
        long elapsed = System.nanoTime() - t0;
        assert found == keys.length : "get: found=" + found + " expected=" + keys.length;
        return elapsed;
    }

    static long benchGetMiss(Map<String, Integer> map, String[] missKeys) {
        long t0 = System.nanoTime();
        int found = 0;
        for (String k : missKeys) if (map.get(k) != null) found++;
        long elapsed = System.nanoTime() - t0;
        assert found == 0 : "miss: found=" + found;
        return elapsed;
    }

    static long benchIterate(Map<String, Integer> map) {
        long t0 = System.nanoTime();
        long sum = 0;
        for (var e : map.entrySet()) sum += e.getValue();
        return System.nanoTime() - t0;
    }

    static long benchRemove(Map<String, Integer> map, String[] keys) {
        long t0 = System.nanoTime();
        for (String k : keys) map.remove(k);
        return System.nanoTime() - t0;
    }

    // === Prefix bench (KSTrie only) ===

    static long benchPrefixCount(KSTrie<Integer> t, String[] prefixes) {
        long t0 = System.nanoTime();
        int total = 0;
        for (String p : prefixes) total += t.prefixCount(p);
        long elapsed = System.nanoTime() - t0;
        return elapsed;
    }

    // === Run one config ===

    static void runConfig(String label, int n, String[] keys, String[] missKeys) {
        int unique = new HashSet<>(Arrays.asList(keys)).size();
        System.out.printf("\n=== N = %,d (%s, %,d unique) ===%n", n, label, unique);
        System.out.printf("%-14s %10s %10s %10s %10s %10s %10s%n",
            "Container", "put ns/op", "get ns/op", "miss ns/op", "iter ms", "rm ns/op", "MB");

        for (String name : new String[]{"KSTrie", "TreeMap", "HashMap"}) {
            // Warm all paths: put, get, miss, iterate, remove
            for (int w = 0; w < WARMUP; w++) {
                Map<String, Integer> m = makeMap(name);
                benchPut(m, keys);
                benchGet(m, keys);
                benchGetMiss(m, missKeys);
                benchIterate(m);
                benchRemove(m, keys);
            }

            long memBefore = usedMemory();
            Map<String, Integer> m = makeMap(name);
            benchPut(m, keys);
            long memAfter = usedMemory();
            double mb = (memAfter - memBefore) / (1024.0 * 1024.0);

            long bestPut = Long.MAX_VALUE, bestGet = Long.MAX_VALUE;
            long bestMiss = Long.MAX_VALUE, bestIter = Long.MAX_VALUE;
            long bestRm = Long.MAX_VALUE;

            for (int r = 0; r < RUNS; r++) {
                m.clear();
                bestPut = Math.min(bestPut, benchPut(m, keys));
                bestGet = Math.min(bestGet, benchGet(m, keys));
                bestMiss = Math.min(bestMiss, benchGetMiss(m, missKeys));
                bestIter = Math.min(bestIter, benchIterate(m));
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

    // === Prefix-specific bench ===

    static void runPrefixBench(int n) {
        String[] keys = urlKeys(n, 42);
        String[] prefixes = {
            "/api/", "/api/v1/", "/api/v1/users/", "/api/v2/",
            "/static/", "/admin/", "/nonexistent/"
        };

        KSTrie<Integer> t = new KSTrie<>();
        for (int i = 0; i < keys.length; i++) t.put(keys[i], i);
        int unique = t.size();

        System.out.printf("\n=== Prefix ops (N=%,d, %,d unique, URL keys) ===%n", n, unique);
        System.out.printf("%-24s %10s %10s%n", "Prefix", "count", "ns/op");

        for (String pfx : prefixes) {
            // Warmup
            for (int w = 0; w < WARMUP; w++) t.prefixCount(pfx);

            long best = Long.MAX_VALUE;
            int cnt = 0;
            for (int r = 0; r < RUNS; r++) {
                long t0 = System.nanoTime();
                cnt = t.prefixCount(pfx);
                best = Math.min(best, System.nanoTime() - t0);
            }
            System.out.printf("%-24s %10d %10d%n", pfx, cnt, best);
        }
    }

    @SuppressWarnings("unchecked")
    static Map<String, Integer> makeMap(String name) {
        return switch (name) {
            case "KSTrie" -> new KSTrie<>();
            case "TreeMap" -> new TreeMap<>();
            case "HashMap" -> new HashMap<>();
            default -> throw new IllegalArgumentException(name);
        };
    }

    // === Main ===

    public static void main(String[] args) {
        System.out.println("KSTrie Benchmark — KSTrie vs TreeMap vs HashMap");
        System.out.println("JVM: " + System.getProperty("java.vm.name") + " " + System.getProperty("java.version"));
        System.out.println("Warmup=" + WARMUP + " Runs=" + RUNS + " (best of " + RUNS + ")\n");

        System.out.println("--- Random alpha keys (len 5-20) ---");
        for (int n : SIZES) {
            String[] keys = randomKeys(n, 42, 5, 20);
            var hitSet = new HashSet<>(Arrays.asList(keys));
            String[] miss = Arrays.stream(randomKeys(n * 2, 999, 5, 20))
                .filter(k -> !hitSet.contains(k)).limit(n).toArray(String[]::new);
            runConfig("random alpha", n, keys, miss);
        }

        System.out.println("\n--- URL-like keys (shared prefixes) ---");
        for (int n : SIZES) {
            String[] keys = urlKeys(n, 42);
            var hitSet = new HashSet<>(Arrays.asList(keys));
            String[] miss = Arrays.stream(urlKeys(n * 2, 999))
                .filter(k -> !hitSet.contains(k)).limit(n).toArray(String[]::new);
            runConfig("URL-like", n, keys, miss);
        }

        runPrefixBench(100_000);
    }
}
