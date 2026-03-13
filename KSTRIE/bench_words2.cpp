#include "kstrie.hpp"
#include <cstdio>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <set>
#include <fstream>
#include <numeric>

static thread_local size_t g_alloc_total = 0;
template<typename T>
struct TrackingAlloc {
    using value_type = T;
    TrackingAlloc() noexcept = default;
    template<typename U> TrackingAlloc(const TrackingAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) { g_alloc_total += n * sizeof(T); return static_cast<T*>(::operator new(n * sizeof(T))); }
    void deallocate(T* p, std::size_t n) noexcept { g_alloc_total -= n * sizeof(T); ::operator delete(p); }
    template<typename U> bool operator==(const TrackingAlloc<U>&) const noexcept { return true; }
};

static double now_ms() {
    using clk = std::chrono::high_resolution_clock;
    static auto t0 = clk::now();
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}
template<typename T> static void do_not_optimize(T const& val) { asm volatile("" : : "r,m"(val) : "memory"); }

static int iters_for(size_t n) {
    if (n <= 200) return 2000;
    if (n <= 1000) return 500;
    if (n <= 10000) return 50;
    if (n <= 100000) return 5;
    return 1;
}

struct Row { size_t n; double k_fnd, k_nf, k_ins, ku_fnd, ku_nf, ku_ins, u_fnd, u_nf, u_ins; size_t k_mem, ku_mem, u_mem; };

int main(int argc, char* argv[]) {
    if (argc != 3) { fprintf(stderr, "Usage: %s <words_file> <verbose:y/n>\n", argv[0]); return 1; }
    bool verbose = argv[2][0] == 'y';

    std::vector<std::string> all_words;
    { std::ifstream f(argv[1]); std::string line;
      while (std::getline(f, line)) {
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (!line.empty()) all_words.push_back(std::move(line));
      }
    }
    std::mt19937_64 rng(42);
    std::shuffle(all_words.begin(), all_words.end(), rng);
    size_t max_n = all_words.size();
    fprintf(stderr, "Loaded %zu words\n", max_n);

    std::vector<size_t> sizes;
    for (double n = 100; n < max_n * 1.01; n *= 1.6)
        sizes.push_back((size_t)n);
    if (sizes.empty() || sizes.back() < max_n) sizes.push_back(max_n);

    std::vector<Row> rows;

    for (auto N : sizes) {
        if (verbose) fprintf(stderr, "  N=%zu...\n", N);
        int iters = iters_for(N);

        std::vector<std::string> keys(all_words.begin(), all_words.begin() + N);
        std::shuffle(keys.begin(), keys.end(), rng);

        std::vector<std::string> find_fnd = keys;
        std::shuffle(find_fnd.begin(), find_fnd.end(), rng);

        std::vector<std::string> find_nf;
        find_nf.reserve(N);
        std::set<std::string> existing(keys.begin(), keys.end());
        for (auto& k : keys) { std::string nk = k + "~NF~"; if (!existing.count(nk)) find_nf.push_back(nk); }
        if (find_nf.size() > find_fnd.size()) find_nf.resize(find_fnd.size());
        std::shuffle(find_nf.begin(), find_nf.end(), rng);

        // Memory
        size_t k_mem, ku_mem, u_mem;
        {
            using T = gteitelbaum::kstrie<int32_t, gteitelbaum::identity_char_map, TrackingAlloc<uint64_t>>;
            g_alloc_total = 0; T t;
            for (size_t i = 0; i < N; ++i) t.insert(keys[i], (int32_t)i);
            k_mem = g_alloc_total;
        }
        {
            using T = gteitelbaum::kstrie<int32_t, gteitelbaum::upper_char_map, TrackingAlloc<uint64_t>>;
            g_alloc_total = 0; T t;
            for (size_t i = 0; i < N; ++i) t.insert(keys[i], (int32_t)i);
            ku_mem = g_alloc_total;
        }
        {
            using U = std::unordered_map<std::string, int32_t, std::hash<std::string>,
                        std::equal_to<std::string>, TrackingAlloc<std::pair<const std::string, int32_t>>>;
            g_alloc_total = 0; U m; m.reserve(N);
            for (size_t i = 0; i < N; ++i) m.emplace(keys[i], (int32_t)i);
            u_mem = g_alloc_total;
        }

        double k_fnd=1e18, k_nf=1e18, k_ins=1e18;
        double ku_fnd=1e18, ku_nf=1e18, ku_ins=1e18;
        double u_fnd=1e18, u_nf=1e18, u_ins=1e18;
        std::vector<size_t> order(N);
        std::iota(order.begin(), order.end(), 0);

        for (int t = 0; t < 3; ++t) {
            std::shuffle(order.begin(), order.end(), rng);
            {
                gteitelbaum::kstrie<int32_t> trie;
                double t0 = now_ms();
                for (auto i : order) trie.insert(keys[i], (int32_t)i);
                k_ins = std::min(k_ins, now_ms() - t0);

                uint64_t cs = 0;
                double t1 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_fnd) { auto* v = trie.find(k); cs += v ? *v : 0; }
                k_fnd = std::min(k_fnd, (now_ms() - t1) / iters);
                do_not_optimize(cs);

                cs = 0;
                double t2 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_nf) { auto* v = trie.find(k); cs += v ? *v : 0; }
                k_nf = std::min(k_nf, (now_ms() - t2) / iters);
                do_not_optimize(cs);
            }
            {
                gteitelbaum::kstrie<int32_t, gteitelbaum::upper_char_map> trie;
                double t0 = now_ms();
                for (auto i : order) trie.insert(keys[i], (int32_t)i);
                ku_ins = std::min(ku_ins, now_ms() - t0);

                uint64_t cs = 0;
                double t1 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_fnd) { auto* v = trie.find(k); cs += v ? *v : 0; }
                ku_fnd = std::min(ku_fnd, (now_ms() - t1) / iters);
                do_not_optimize(cs);

                cs = 0;
                double t2 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_nf) { auto* v = trie.find(k); cs += v ? *v : 0; }
                ku_nf = std::min(ku_nf, (now_ms() - t2) / iters);
                do_not_optimize(cs);
            }
            {
                std::unordered_map<std::string, int32_t> m; m.reserve(N);
                double t0 = now_ms();
                for (auto i : order) m.emplace(keys[i], (int32_t)i);
                u_ins = std::min(u_ins, now_ms() - t0);

                uint64_t cs = 0;
                double t1 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_fnd) { auto it = m.find(k); cs += (it!=m.end()) ? it->second : 0; }
                u_fnd = std::min(u_fnd, (now_ms() - t1) / iters);
                do_not_optimize(cs);

                cs = 0;
                double t2 = now_ms();
                for (int r = 0; r < iters; ++r)
                    for (auto& k : find_nf) { auto it = m.find(k); cs += (it!=m.end()) ? it->second : 0; }
                u_nf = std::min(u_nf, (now_ms() - t2) / iters);
                do_not_optimize(cs);
            }
        }
        rows.push_back({N, k_fnd, k_nf, k_ins, ku_fnd, ku_nf, ku_ins, u_fnd, u_nf, u_ins, k_mem, ku_mem, u_mem});
    }

    // Emit HTML
    printf(R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/>
<title>kstrie vs umap — words.txt</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js"></script>
<style>
body{margin:0;background:#0f0f1a;color:#ddd;font-family:system-ui,sans-serif}
.wrap{max-width:720px;margin:0 auto;padding:16px 12px}
h2{margin:0 0 4px;font-size:18px;font-weight:700;text-align:center}
.sub{text-align:center;color:#777;font-size:12px;margin:0 0 12px}
.chart-box{margin-bottom:24px}
.chart-box h3{margin:0 0 6px;font-size:14px;font-weight:600;text-align:center}
canvas{background:#12122a;border-radius:8px}
</style></head><body><div class="wrap">
<h2>kstrie (identity / upper) vs unordered_map — %zu words</h2>
<p class="sub">Log-log · Per-entry · Lower is better</p>
<div class="chart-box"><h3>Find (ns/entry)</h3><canvas id="c_find"></canvas></div>
<div class="chart-box"><h3>Insert (ns/entry)</h3><canvas id="c_ins"></canvas></div>
<div class="chart-box"><h3>Memory (B/entry)</h3><canvas id="c_mem"></canvas></div>
</div><script>
)HTML", max_n);

    printf("const D=[");
    for (auto& r : rows)
        printf("{N:%zu,kf:%.4f,kn:%.4f,ki:%.4f,kuf:%.4f,kun:%.4f,kui:%.4f,uf:%.4f,un:%.4f,ui:%.4f,km:%zu,kum:%zu,um:%zu},\n",
               r.n, r.k_fnd, r.k_nf, r.k_ins, r.ku_fnd, r.ku_nf, r.ku_ins, r.u_fnd, r.u_nf, r.u_ins, r.k_mem, r.ku_mem, r.u_mem);
    printf("];\n");

    printf("%s", R"JS(
const L={
  find:[
    {k:"kf",color:"#3b82f6",dash:[],w:2.5,label:"identity FND"},
    {k:"kn",color:"#93c5fd",dash:[6,3],w:1.5,label:"identity NF"},
    {k:"kuf",color:"#f59e0b",dash:[],w:2.5,label:"upper FND"},
    {k:"kun",color:"#fcd34d",dash:[6,3],w:1.5,label:"upper NF"},
    {k:"uf",color:"#22c55e",dash:[],w:2.5,label:"umap FND"},
    {k:"un",color:"#86efac",dash:[6,3],w:1.5,label:"umap NF"},
  ],
  ins:[
    {k:"ki",color:"#3b82f6",dash:[],w:2.5,label:"identity"},
    {k:"kui",color:"#f59e0b",dash:[],w:2.5,label:"upper"},
    {k:"ui",color:"#22c55e",dash:[],w:2.5,label:"umap"},
  ],
  mem:[
    {k:"km",color:"#3b82f6",dash:[],w:2.5,label:"identity"},
    {k:"kum",color:"#f59e0b",dash:[],w:2.5,label:"upper"},
    {k:"um",color:"#22c55e",dash:[],w:2.5,label:"umap"},
  ]
};
function mk(id,lines,conv){
  const ctx=document.getElementById(id).getContext("2d");
  new Chart(ctx,{type:"line",data:{
    labels:D.map(d=>d.N),
    datasets:lines.map(l=>({label:l.label,data:D.map(d=>conv(d[l.k],d.N)),
      borderColor:l.color,backgroundColor:l.color+"22",borderWidth:l.w,
      borderDash:l.dash,pointRadius:0,pointHitRadius:8,tension:0.2,spanGaps:true}))
  },options:{responsive:true,interaction:{mode:"index",intersect:false},
    plugins:{legend:{display:true,labels:{color:"#bbb",font:{size:11},boxWidth:20,padding:10}},
      tooltip:{backgroundColor:"#1a1a2e",borderColor:"#444",borderWidth:1,titleColor:"#aaa",bodyColor:"#ddd",
        callbacks:{title:i=>{const v=i[0].parsed.x;return v>=1e6?"N="+(v/1e6).toFixed(1)+"M":v>=1e3?"N="+(v/1e3).toFixed(1)+"K":"N="+v},
          label:i=>{const v=i.parsed.y;if(v==null)return null;const s=v<0.1?v.toFixed(3):v<10?v.toFixed(2):v<1e3?v.toFixed(1):v.toFixed(0);return" "+i.dataset.label+": "+s}}}},
    scales:{x:{type:"logarithmic",ticks:{color:"#888",font:{size:11},callback:v=>v>=1e6?(v/1e6)+"M":v>=1e3?(v/1e3)+"K":v},grid:{color:"#2a2a3e"}},
      y:{type:"logarithmic",ticks:{color:"#888",font:{size:10},callback:v=>v<0.1?v.toFixed(2):v<10?v.toFixed(1):v>=1e3?v.toFixed(0):v.toFixed(1)},grid:{color:"#2a2a3e"}}}}});
}
mk("c_find",L.find,(ms,n)=>(ms*1e6)/n);
mk("c_ins",L.ins,(ms,n)=>(ms*1e6)/n);
mk("c_mem",L.mem,(b,n)=>b/n);
)JS");
    printf("</script></body></html>\n");
}
