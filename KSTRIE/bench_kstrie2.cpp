// bench_kstrie2.cpp
// Usage: ./bench_kstrie2 <max_n> <verbose:y/n> > report.html
// Benchmarks kstrie vs std::unordered_map for string keys.
// Outputs a self-contained HTML file with Chart.js log-log graphs.

#include "kstrie.hpp"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <set>
#include <cstring>
#include <new>

// ==========================================================================
// Tracking allocator
// ==========================================================================

static thread_local size_t g_alloc_total = 0;

template<typename T>
struct TrackingAlloc {
    using value_type = T;
    TrackingAlloc() noexcept = default;
    template<typename U> TrackingAlloc(const TrackingAlloc<U>&) noexcept {}
    T* allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        g_alloc_total += bytes;
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return static_cast<T*>(::operator new(bytes, std::align_val_t{alignof(T)}));
        else
            return static_cast<T*>(::operator new(bytes));
    }
    void deallocate(T* p, size_t n) noexcept {
        g_alloc_total -= n * sizeof(T);
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            ::operator delete(p, std::align_val_t{alignof(T)});
        else
            ::operator delete(p);
    }
    template<typename U> bool operator==(const TrackingAlloc<U>&) const noexcept { return true; }
};

// ==========================================================================
// Helpers
// ==========================================================================

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

template<typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

// ==========================================================================
// Key generators — produce N unique strings per pattern
// ==========================================================================

static std::vector<std::string> gen_random(size_t n, std::mt19937_64& rng) {
    std::set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(n);
    while (keys.size() < n) {
        int len = 4 + rng() % 16;
        std::string k(len, ' ');
        for (char& c : k) c = 'a' + rng() % 26;
        if (seen.insert(k).second) keys.push_back(k);
    }
    return keys;
}

static std::vector<std::string> gen_url(size_t n, std::mt19937_64& rng) {
    static const char* tlds[]  = {"com","net","org","io","dev","ai","co"};
    static const char* paths[] = {"api","v1","v2","user","item","product",
                                   "search","admin","public","static","assets"};
    std::set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(n);
    while (keys.size() < n) {
        // scheme
        std::string k = (rng() % 4 == 0) ? "https://" : "http://";
        // subdomain (50% chance)
        if (rng() % 2 == 0) {
            int sl = 3 + rng() % 6;
            for (int i = 0; i < sl; ++i) k += 'a' + rng() % 26;
            k += '.';
        }
        // domain
        int dl = 4 + rng() % 8;
        for (int i = 0; i < dl; ++i) k += 'a' + rng() % 26;
        k += '.';
        k += tlds[rng() % 7];
        // path segments (1-4)
        int segs = 1 + rng() % 4;
        for (int s = 0; s < segs; ++s) {
            k += '/';
            k += paths[rng() % 11];
            if (rng() % 2) { k += '/'; k += std::to_string(rng() % 100000); }
        }
        if (seen.insert(k).second) keys.push_back(k);
    }
    return keys;
}

static std::vector<std::string> gen_path(size_t n, std::mt19937_64& rng) {
    static const char* roots[] = {"/usr/local","/var/log","/etc",
                                   "/home/user","/opt/app","/srv/data"};
    static const char* parts[] = {"bin","lib","share","include","src","conf",
                                   "logs","data","cache","tmp","pkg","modules"};
    std::set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(n);
    while (keys.size() < n) {
        std::string k = roots[rng() % 6];
        int depth = 1 + rng() % 4;
        for (int d = 0; d < depth; ++d) {
            k += '/';
            k += parts[rng() % 12];
            if (rng() % 3 == 0) { k += '_'; k += std::to_string(rng() % 1000); }
        }
        // optional extension
        if (rng() % 2 == 0) {
            static const char* exts[] = {".conf",".log",".so",".h",".cpp",".json"};
            k += exts[rng() % 6];
        }
        if (seen.insert(k).second) keys.push_back(k);
    }
    return keys;
}

static std::vector<std::string> gen_shared(size_t n, std::mt19937_64& rng) {
    // Long shared prefix + short unique suffix — best case for trie compression
    static const char* prefixes[] = {
        "com.example.application.service.handler.",
        "org.springframework.boot.autoconfigure.",
        "/api/v2/organizations/acme-corp/projects/",
        "https://storage.example.com/bucket/uploads/2024/"
    };
    std::set<std::string> seen;
    std::vector<std::string> keys;
    keys.reserve(n);
    while (keys.size() < n) {
        std::string k = prefixes[rng() % 4];
        int sl = 4 + rng() % 12;
        for (int i = 0; i < sl; ++i) k += 'a' + rng() % 26;
        if (seen.insert(k).second) keys.push_back(k);
    }
    return keys;
}

// ==========================================================================
// Workload
// ==========================================================================

struct Workload {
    std::string              pattern;
    std::vector<std::string> keys;
    std::vector<std::string> erase_keys;
    std::vector<std::string> find_fnd;
    std::vector<std::string> find_nf;
    int                      find_iters;
};

static int iters_for(size_t n) {
    if      (n <= 200)    return 2000;
    else if (n <= 1000)   return 500;
    else if (n <= 10000)  return 50;
    else if (n <= 100000) return 5;
    else if (n <= 250000) return 2;
    else                  return 1;
}

static Workload make_workload(size_t n, const std::string& pattern,
                               std::mt19937_64& rng) {
    Workload w;
    w.pattern = pattern;
    w.find_iters = iters_for(n);

    if      (pattern == "random") w.keys = gen_random(n, rng);
    else if (pattern == "url")    w.keys = gen_url(n, rng);
    else if (pattern == "path")   w.keys = gen_path(n, rng);
    else                          w.keys = gen_shared(n, rng);

    // Shuffle insert order
    std::shuffle(w.keys.begin(), w.keys.end(), rng);

    // Erase every other key
    for (size_t i = 0; i < w.keys.size(); i += 2)
        w.erase_keys.push_back(w.keys[i]);

    // find_fnd = shuffled copy of all keys
    w.find_fnd = w.keys;
    std::shuffle(w.find_fnd.begin(), w.find_fnd.end(), rng);

    // find_nf = keys that definitely don't exist
    // Append a suffix that makes them unique
    w.find_nf.reserve(w.keys.size());
    std::set<std::string> existing(w.keys.begin(), w.keys.end());
    for (auto& k : w.keys) {
        std::string nk = k + "~NOTFOUND~";
        if (!existing.count(nk)) {
            w.find_nf.push_back(nk);
            existing.insert(nk);
        }
    }
    // Trim to same size as find_fnd
    if (w.find_nf.size() > w.find_fnd.size())
        w.find_nf.resize(w.find_fnd.size());
    std::shuffle(w.find_nf.begin(), w.find_nf.end(), rng);

    return w;
}

// ==========================================================================
// Row
// ==========================================================================

struct Row {
    std::string pattern;
    size_t      n;
    const char* container;
    double      find_fnd_ms;
    double      find_nf_ms;
    double      insert_ms;
    double      erase_ms;
    double      iter_ms;
    size_t      mem_bytes;
};

static int trials_for(size_t n) {
    if (n <= 100000) return 3;
    return 1;
}

// ==========================================================================
// bench_one
// ==========================================================================

static void bench_one(const Workload& w, std::vector<Row>& rows, bool verbose) {
    size_t n = w.keys.size();
    if (verbose) std::fprintf(stderr, "  %s N=%zu...\n", w.pattern.c_str(), n);

    std::mt19937_64 rng(12345);

    // Pre-generate shuffled find orders
    std::vector<std::vector<size_t>> fnd_idx(w.find_iters), nf_idx(w.find_iters);
    {
        std::vector<size_t> base(w.find_fnd.size());
        std::iota(base.begin(), base.end(), 0);
        for (int r = 0; r < w.find_iters; ++r) {
            fnd_idx[r] = base;
            std::shuffle(fnd_idx[r].begin(), fnd_idx[r].end(), rng);
        }
        std::vector<size_t> base2(w.find_nf.size());
        std::iota(base2.begin(), base2.end(), 0);
        for (int r = 0; r < w.find_iters; ++r) {
            nf_idx[r] = base2;
            std::shuffle(nf_idx[r].begin(), nf_idx[r].end(), rng);
        }
    }

    // ---- Memory measurement ----
    size_t kstrie_mem, umap_mem;
    {
        using TrieT = gteitelbaum::kstrie<int32_t,
                        gteitelbaum::identity_char_map,
                        TrackingAlloc<uint64_t>>;
        g_alloc_total = 0;
        TrieT t;
        for (size_t i = 0; i < n; ++i) t.insert(w.keys[i], (int32_t)i);
        kstrie_mem = g_alloc_total;
    }
    {
        using UMapT = std::unordered_map<std::string, int32_t,
                        std::hash<std::string>, std::equal_to<std::string>,
                        TrackingAlloc<std::pair<const std::string, int32_t>>>;
        g_alloc_total = 0;
        UMapT m;
        m.reserve(n);
        for (size_t i = 0; i < n; ++i) m.emplace(w.keys[i], (int32_t)i);
        umap_mem = g_alloc_total;
    }

    double k_fnd=1e18, k_nf=1e18, k_ins=1e18, k_ers=1e18, k_iter=1e18;
    double u_fnd=1e18, u_nf=1e18, u_ins=1e18, u_ers=1e18, u_iter=1e18;

    std::vector<size_t> insert_order(n);
    std::iota(insert_order.begin(), insert_order.end(), 0);

    for (int t = 0; t < trials_for(n); ++t) {
        std::shuffle(insert_order.begin(), insert_order.end(), rng);
        std::vector<size_t> erase_order(w.erase_keys.size());
        std::iota(erase_order.begin(), erase_order.end(), 0);
        std::shuffle(erase_order.begin(), erase_order.end(), rng);

        // ---- kstrie ----
        {
            gteitelbaum::kstrie<int32_t> trie;
            double t0 = now_ms();
            for (auto i : insert_order) trie.insert(w.keys[i], (int32_t)i);
            k_ins = std::min(k_ins, now_ms() - t0);

            { uint64_t s = 0; double ti = now_ms();
              for (auto it = trie.begin(); it != trie.end(); ++it) s += it.value();
              k_iter = std::min(k_iter, now_ms() - ti); do_not_optimize(s); }

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < w.find_iters; ++r)
                for (auto i : fnd_idx[r]) { auto* v = trie.find(w.find_fnd[i]); cs += v ? *v : 0; }
            k_fnd = std::min(k_fnd, (now_ms() - t1) / w.find_iters);
            do_not_optimize(cs);

            cs = 0;
            double t1n = now_ms();
            for (int r = 0; r < w.find_iters; ++r)
                for (auto i : nf_idx[r]) { auto* v = trie.find(w.find_nf[i]); cs += v ? *v : 0; }
            k_nf = std::min(k_nf, (now_ms() - t1n) / w.find_iters);
            do_not_optimize(cs);

            if (true) {
                // Repeated erase+reinsert loop for stable timing at small N.
                // After each erase pass we reinsert so the next iteration is valid.
                double t2 = now_ms();
                for (int r = 0; r < w.find_iters; ++r) {
                    for (auto i : erase_order) trie.erase(w.erase_keys[i]);
                    for (auto i : erase_order) trie.insert(w.erase_keys[i], (int32_t)i);
                }
                k_ers = std::min(k_ers, (now_ms() - t2) / (w.find_iters * 2));
            }
        }

        // ---- unordered_map ----
        {
            std::unordered_map<std::string, int32_t> m;
            m.reserve(n);
            double t0 = now_ms();
            for (auto i : insert_order) m.emplace(w.keys[i], (int32_t)i);
            u_ins = std::min(u_ins, now_ms() - t0);

            { uint64_t s = 0; double ti = now_ms();
              for (auto& [k,v] : m) s += v;
              u_iter = std::min(u_iter, now_ms() - ti); do_not_optimize(s); }

            uint64_t cs = 0;
            double t1 = now_ms();
            for (int r = 0; r < w.find_iters; ++r)
                for (auto i : fnd_idx[r]) { auto it = m.find(w.find_fnd[i]); cs += (it!=m.end()) ? it->second : 0; }
            u_fnd = std::min(u_fnd, (now_ms() - t1) / w.find_iters);
            do_not_optimize(cs);

            cs = 0;
            double t1n = now_ms();
            for (int r = 0; r < w.find_iters; ++r)
                for (auto i : nf_idx[r]) { auto it = m.find(w.find_nf[i]); cs += (it!=m.end()) ? it->second : 0; }
            u_nf = std::min(u_nf, (now_ms() - t1n) / w.find_iters);
            do_not_optimize(cs);

            if (true) {
                double t2 = now_ms();
                for (int r = 0; r < w.find_iters; ++r) {
                    for (auto i : erase_order) m.erase(w.erase_keys[i]);
                    for (auto i : erase_order) m.emplace(w.erase_keys[i], (int32_t)i);
                }
                u_ers = std::min(u_ers, (now_ms() - t2) / (w.find_iters * 2));
            }
        }
    }

    rows.push_back({w.pattern, n, "kstrie", k_fnd, k_nf, k_ins, k_ers, k_iter, kstrie_mem});
    rows.push_back({w.pattern, n, "umap",   u_fnd, u_nf, u_ins, u_ers, u_iter, umap_mem});
}

// ==========================================================================
// emit_html
// ==========================================================================

static void emit_html(const std::vector<Row>& rows) {
    struct DataPoint {
        std::string pattern;
        size_t      N;
        double      vals[2][6];
        bool        has[2];
    };

    auto cidx = [](const char* c) -> int {
        if (std::strcmp(c, "kstrie") == 0) return 0;
        return 1;
    };

    std::vector<DataPoint> points;
    for (auto& r : rows) {
        int pi = -1;
        for (int i = 0; i < (int)points.size(); ++i)
            if (points[i].pattern == r.pattern && points[i].N == r.n) { pi = i; break; }
        if (pi < 0) {
            pi = (int)points.size();
            DataPoint dp{};
            dp.pattern = r.pattern;
            dp.N = r.n;
            std::memset(dp.has, 0, sizeof(dp.has));
            points.push_back(dp);
        }
        int ci = cidx(r.container);
        points[pi].vals[ci][0] = r.find_fnd_ms;
        points[pi].vals[ci][1] = r.find_nf_ms;
        points[pi].vals[ci][2] = r.insert_ms;
        points[pi].vals[ci][3] = r.erase_ms;
        points[pi].vals[ci][4] = r.iter_ms;
        points[pi].vals[ci][5] = static_cast<double>(r.mem_bytes);
        points[pi].has[ci] = true;
    }

    std::sort(points.begin(), points.end(), [](const DataPoint& a, const DataPoint& b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        return a.N < b.N;
    });

    const char* names[]    = {"kstrie", "umap"};
    const char* suffixes[] = {"fnd", "nf", "insert", "erase", "iter", "mem"};

    std::printf(R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>kstrie Benchmark</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js"></script>
<style>
  body { margin:0; background:#0f0f1a; color:#ddd; font-family:system-ui,sans-serif; }
  .wrap { max-width:720px; margin:0 auto; padding:16px 12px; }
  h2 { margin:0 0 4px; font-size:18px; font-weight:700; text-align:center; }
  .sub { text-align:center; color:#777; font-size:12px; margin:0 0 12px; }
  .btns { display:flex; justify-content:center; gap:8px; margin-bottom:16px; flex-wrap:wrap; }
  .btns button { padding:6px 16px; border-radius:6px; border:1px solid #444;
    background:#1a1a2e; color:#aaa; cursor:pointer; font-size:13px; font-weight:600; }
  .btns button.active { background:#3b82f6; color:#fff; }
  .chart-box { margin-bottom:24px; }
  .chart-box h3 { margin:0 0 6px; font-size:14px; font-weight:600; text-align:center; }
  canvas { background:#12122a; border-radius:8px; }
</style>
</head>
<body>
<div class="wrap">
  <h2>kstrie Benchmark (string &rarr; int32)</h2>
  <p class="sub">Log-log &middot; Per-entry &middot; Lower is better &middot; FND=100%% hit, NF=100%% miss</p>
  <div class="btns">
    <button class="active" onclick="show('random',event)">random</button>
    <button onclick="show('url',event)">url-like</button>
    <button onclick="show('path',event)">path-like</button>
    <button onclick="show('shared',event)">shared prefix</button>
  </div>
  <div class="chart-box"><h3>Find (ns/entry)</h3><canvas id="c_find"></canvas></div>
  <div class="chart-box"><h3>Insert (ns/entry)</h3><canvas id="c_insert"></canvas></div>
  <div class="chart-box"><h3>Iteration (ns/entry)</h3><canvas id="c_iter"></canvas></div>
  <div class="chart-box"><h3>Erase N/2 (ns/entry)</h3><canvas id="c_erase"></canvas></div>
  <div class="chart-box"><h3>Memory (B/entry)</h3><canvas id="c_mem"></canvas></div>
</div>
<script>
)HTML");

    std::printf("const RAW_DATA = [\n");
    for (auto& p : points) {
        std::printf("  {pattern:\"%s\",N:%zu", p.pattern.c_str(), p.N);
        for (int ci = 0; ci < 2; ++ci) {
            if (!p.has[ci]) continue;
            for (int mi = 0; mi < 6; ++mi) {
                if (mi == 5)
                    std::printf(",%s_%s:%.0f", names[ci], suffixes[mi], p.vals[ci][mi]);
                else
                    std::printf(",%s_%s:%.4f", names[ci], suffixes[mi], p.vals[ci][mi]);
            }
        }
        std::printf("},\n");
    }
    std::printf("];\n\n");

    std::printf("%s", R"JS(
const LINES_FIND = [
  { key: "kstrie", suffix: "fnd", color: "#3b82f6", dash: [],    width: 2.5, label: "kstrie FND" },
  { key: "kstrie", suffix: "nf",  color: "#93c5fd", dash: [6,3], width: 1.5, label: "kstrie NF"  },
  { key: "umap",   suffix: "fnd", color: "#22c55e", dash: [],    width: 2.5, label: "umap FND"   },
  { key: "umap",   suffix: "nf",  color: "#86efac", dash: [6,3], width: 1.5, label: "umap NF"    },
];
const LINES_INSERT = [
  { key: "kstrie", suffix: "insert", color: "#3b82f6", dash: [], width: 2.5, label: "kstrie" },
  { key: "umap",   suffix: "insert", color: "#22c55e", dash: [], width: 2.5, label: "umap"   },
];
const LINES_ITER = [
  { key: "kstrie", suffix: "iter", color: "#3b82f6", dash: [], width: 2.5, label: "kstrie" },
  { key: "umap",   suffix: "iter", color: "#22c55e", dash: [], width: 2.5, label: "umap"   },
];
const LINES_ERASE = [
  { key: "kstrie", suffix: "erase", color: "#3b82f6", dash: [], width: 2.5, label: "kstrie" },
  { key: "umap",   suffix: "erase", color: "#22c55e", dash: [], width: 2.5, label: "umap"   },
];
const LINES_MEM = [
  { key: "kstrie", suffix: "mem", color: "#3b82f6", dash: [],    width: 2.5, label: "kstrie" },
  { key: "umap",   suffix: "mem", color: "#22c55e", dash: [],    width: 2.5, label: "umap"   },
];

const METRICS = [
  { id: "find",   lines: LINES_FIND,   convert: (ms, n) => (ms * 1e6) / n          },
  { id: "insert", lines: LINES_INSERT, convert: (ms, n) => (ms * 1e6) / n          },
  { id: "iter",   lines: LINES_ITER,   convert: (ms, n) => (ms * 1e6) / n          },
  { id: "erase",  lines: LINES_ERASE,  convert: (ms, n) => (ms * 1e6) / (n / 2)   },
  { id: "mem",    lines: LINES_MEM,    convert: (b,  n) => b / n                   },
];

function buildData(pattern, metric) {
  return RAW_DATA
    .filter(r => r.pattern === pattern)
    .map(r => {
      const pt = { N: r.N };
      for (const l of metric.lines) {
        const raw = r[l.key + "_" + l.suffix];
        if (raw != null) pt[l.key + "_" + l.suffix] = metric.convert(raw, r.N);
      }
      return pt;
    });
}

const charts = {};

function makeChart(canvasId, metric) {
  const ctx = document.getElementById(canvasId).getContext("2d");
  const data = buildData("random", metric);
  charts[canvasId] = new Chart(ctx, {
    type: "line",
    data: {
      labels: data.map(d => d.N),
      datasets: metric.lines.map(l => ({
        label: l.label,
        data: data.map(d => d[l.key + "_" + l.suffix] ?? null),
        borderColor: l.color,
        backgroundColor: l.color + "22",
        borderWidth: l.width,
        borderDash: l.dash,
        pointRadius: 0,
        pointHitRadius: 8,
        tension: 0.2,
        spanGaps: true,
      })),
    },
    options: {
      responsive: true,
      interaction: { mode: "index", intersect: false },
      plugins: {
        legend: { display: true, labels: { color: "#bbb", font: { size: 11 }, boxWidth: 20, padding: 10 } },
        tooltip: {
          backgroundColor: "#1a1a2e",
          borderColor: "#444",
          borderWidth: 1,
          titleColor: "#aaa",
          bodyColor: "#ddd",
          callbacks: {
            title: (items) => {
              const v = items[0].parsed.x;
              if (v >= 1e6) return "N = " + (v/1e6).toFixed(1) + "M";
              if (v >= 1e3) return "N = " + (v/1e3).toFixed(1) + "K";
              return "N = " + v;
            },
            label: (item) => {
              const v = item.parsed.y;
              if (v == null) return null;
              const s = v < 0.1 ? v.toFixed(3) : v < 10 ? v.toFixed(2) : v < 1000 ? v.toFixed(1) : v.toFixed(0);
              return " " + item.dataset.label + ": " + s;
            },
          },
        },
      },
      scales: {
        x: {
          type: "logarithmic",
          ticks: { color: "#888", font: { size: 11 },
            callback: v => v >= 1e6 ? (v/1e6)+"M" : v >= 1e3 ? (v/1e3)+"K" : v },
          grid: { color: "#2a2a3e" },
        },
        y: {
          type: "logarithmic",
          ticks: { color: "#888", font: { size: 10 },
            callback: v => v < 0.1 ? v.toFixed(2) : v < 10 ? v.toFixed(1) : v >= 1000 ? v.toFixed(0) : v.toFixed(1) },
          grid: { color: "#2a2a3e" },
        },
      },
    },
  });
  charts[canvasId]._metric = metric;
}

METRICS.forEach(m => makeChart("c_" + m.id, m));

function show(pattern, ev) {
  document.querySelectorAll(".btns button").forEach(b => b.classList.remove("active"));
  ev.target.classList.add("active");
  for (const [id, chart] of Object.entries(charts)) {
    const m = chart._metric;
    const data = buildData(pattern, m);
    chart.data.labels = data.map(d => d.N);
    m.lines.forEach((l, i) => {
      chart.data.datasets[i].data = data.map(d => d[l.key + "_" + l.suffix] ?? null);
    });
    chart.update("none");
  }
}
)JS");

    std::printf("</script>\n</body>\n</html>\n");
}

// ==========================================================================
// main
// ==========================================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr,
            "Usage: %s <max_entries> <verbose:y/n> > report.html\n"
            "  Example: %s 200000 y > bench.html\n",
            argv[0], argv[0]);
        return 1;
    }

    size_t max_n  = static_cast<size_t>(std::atof(argv[1]));
    bool verbose  = (argv[2][0] == 'y' || argv[2][0] == 'Y');

    // Build size ladder: ~20 points from 100 to max_n
    std::vector<size_t> sizes;
    for (double n = 100; n < static_cast<double>(max_n) * 1.01; n *= 1.6)
        sizes.push_back(static_cast<size_t>(n));
    if (sizes.empty() || sizes.back() < max_n)
        sizes.push_back(max_n);

    const char* patterns[] = {"random", "url", "path", "shared"};

    std::vector<Row> rows;
    std::mt19937_64 rng(42);

    for (auto* pat : patterns) {
        if (verbose) std::fprintf(stderr, "Pattern: %s\n", pat);
        for (auto n : sizes) {
            auto w = make_workload(n, pat, rng);
            bench_one(w, rows, verbose);
        }
    }

    emit_html(rows);
    return 0;
}
