// Round-trip verification: both loaded vocab and trained vocab
#include "ktoken.hpp"
#include <cstdio>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::cl100k, ktoken::tiktoken_format>;

static bool verify(const tok_t& tok, const uint8_t* data, size_t len, const char* label) {
    auto ids = tok.encode(data, len);
    auto dec = tok.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && (len == 0 || memcmp(dec.data(), data, len) == 0);
    printf("  %-20s %zu bytes -> %zu tokens -> %zu bytes: %s\n",
           label, len, ids.size(), dec.size(), ok ? "PASS" : "FAIL");
    return ok;
}

int main() {
    std::ifstream cf("corpus.txt", std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);

    std::ifstream mf("corpus_multi.txt", std::ios::binary|std::ios::ate);
    size_t mlen = mf.tellg(); mf.seekg(0);
    std::vector<uint8_t> multi(mlen);
    mf.read(reinterpret_cast<char*>(multi.data()), mlen);

    bool all_ok = true;

    // Test 1: loaded cl100k vocab
    printf("=== Loaded cl100k ===\n");
    tok_t loaded("UnicodeData.txt", "cl100k_base.tiktoken");
    all_ok &= verify(loaded, corpus.data(), len, "English (28MB)");
    all_ok &= verify(loaded, multi.data(), mlen, "Multilingual (2.6MB)");
    all_ok &= verify(loaded, (const uint8_t*)"", 0, "Empty");
    all_ok &= verify(loaded, (const uint8_t*)"A", 1, "Single byte");

    // Test 2: trained vocab
    printf("\n=== Trained (1000 merges) ===\n");
    auto trained = tok_t::train("UnicodeData.txt", corpus.data(), len, 1256);
    printf("  Vocab: %zu\n", trained.vocab_size());
    all_ok &= verify(trained, corpus.data(), len, "English (28MB)");
    all_ok &= verify(trained, (const uint8_t*)"Hello, world!", 13, "Hello world");

    // Test 3: o200k splitter
    printf("\n=== o200k splitter ===\n");
    using tok_o = ktoken::tokenizer<ktoken::o200k, ktoken::tiktoken_format>;
    tok_o o200("UnicodeData.txt", "cl100k_base.tiktoken");
    {
        auto ids = o200.encode(corpus.data(), len);
        auto dec = o200.decode(ids.data(), ids.size());
        bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
        printf("  %-20s %zu bytes -> %zu tokens -> %zu bytes: %s\n",
               "English (28MB)", len, ids.size(), dec.size(), ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    // Test 4: p50k splitter
    printf("\n=== p50k splitter ===\n");
    using tok_p = ktoken::tokenizer<ktoken::p50k, ktoken::tiktoken_format>;
    tok_p p50("UnicodeData.txt", "cl100k_base.tiktoken");
    {
        auto ids = p50.encode(corpus.data(), len);
        auto dec = p50.decode(ids.data(), ids.size());
        bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
        printf("  %-20s %zu bytes -> %zu tokens -> %zu bytes: %s\n",
               "English (28MB)", len, ids.size(), dec.size(), ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    printf("\nOverall: %s\n", all_ok ? "ALL PASS" : "SOME FAILURES");
    return all_ok ? 0 : 1;
}
