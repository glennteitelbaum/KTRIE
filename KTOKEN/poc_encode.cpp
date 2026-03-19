// cl100k encoder benchmark — validates token-for-token match with tiktoken
#include "ktoken.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::cl100k, ktoken::tiktoken_format>;
using hrclock = std::chrono::high_resolution_clock;

int main() {
    printf("Loading...\n");
    auto t0 = hrclock::now();
    tok_t tok("UnicodeData.txt", "cl100k_base.tiktoken");
    printf("  vocab=%zu in %.0f ms\n\n", tok.vocab_size(),
           std::chrono::duration<double,std::milli>(hrclock::now()-t0).count());

    std::ifstream cf("corpus.txt", std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);
    printf("Corpus: %zu bytes\n\n", len);

    // Warmup
    tok.encode(corpus.data(), len);

    // Benchmark
    auto t1 = hrclock::now();
    auto ids = tok.encode(corpus.data(), len);
    auto t2 = hrclock::now();
    double ms = std::chrono::duration<double,std::milli>(t2-t1).count();
    printf("Encode: %zu tokens in %.1f ms (%.1f MB/s)\n", ids.size(), ms, len/(ms*1e3));

    // Decode round-trip
    auto dec = tok.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
    printf("Decode: %zu bytes, round-trip %s\n\n", dec.size(), ok ? "PASS" : "FAIL");

    // Stability
    printf("Stability (10 runs):\n");
    for (int i = 0; i < 10; ++i) {
        auto ta = hrclock::now();
        auto r = tok.encode(corpus.data(), len);
        auto tb = hrclock::now();
        double m = std::chrono::duration<double,std::milli>(tb-ta).count();
        printf("  run %d: %zu tokens, %.1f ms (%.1f MB/s)\n", i, r.size(), m, len/(m*1e3));
    }

    printf("\nTokens: %zu\n", ids.size());
    return ok ? 0 : 1;
}
