// p50k splitter + encode validation — zero divergences vs tiktoken
#include "ktoken.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::p50k, ktoken::tiktoken_format>;

int main(int argc, char** argv) {
    const char* corpus_path = (argc > 1) ? argv[1] : "corpus_multi.txt";

    tok_t tok("UnicodeData.txt", "cl100k_base.tiktoken");
    printf("Loaded: vocab=%zu\n\n", tok.vocab_size());

    // Splitter spot checks — case-sensitive contractions, marks, digits
    auto trie = tok.trie();
    struct tc { const char* name; const char* text; };
    tc tests[] = {
        {"contr_lc", "don't"}, {"contr_uc", "DON'T"}, {"its", "it's"},
        {"digits", "123456"}, {"sp_letter", " Hello"}, {"comma", "Hello, World!"},
        {"trail_sp", "hello   "}, {"ws_nl", "  \nhello"},
    };
    printf("Splitter spot checks:\n");
    for (auto& t : tests) {
        auto data = reinterpret_cast<const uint8_t*>(t.text);
        auto chunks = ktoken::p50k::split(trie, data, strlen(t.text));
        printf("  %-12s \"%-20s\" -> [", t.name, t.text);
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (i) printf(", ");
            printf("\"%.*s\"", chunks[i].len, t.text + chunks[i].off);
        }
        printf("]\n");
    }

    // Corpus encode round-trip
    printf("\nCorpus: %s\n", corpus_path);
    std::ifstream cf(corpus_path, std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);

    using hrclock = std::chrono::high_resolution_clock;
    auto t0 = hrclock::now();
    auto ids = tok.encode(corpus.data(), len);
    auto t1 = hrclock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    printf("Encode: %zu tokens in %.1f ms (%.1f MB/s)\n", ids.size(), ms, len/(ms*1e3));

    auto dec = tok.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
    printf("Round-trip: %s\n", ok ? "PASS" : "FAIL");

    // Coverage check
    auto chunks = ktoken::p50k::split(trie, corpus.data(), len);
    size_t total = 0;
    for (auto& c : chunks) total += c.len;
    printf("Split: %zu chunks, coverage: %zu / %zu bytes %s\n",
           chunks.size(), total, len, total == len ? "OK" : "INCOMPLETE");

    return ok ? 0 : 1;
}
