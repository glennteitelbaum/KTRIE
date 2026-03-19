// o200k splitter + encode validation
#include "ktoken.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::o200k, ktoken::tiktoken_format>;

int main(int argc, char** argv) {
    const char* corpus_path = (argc > 1) ? argv[1] : "corpus.txt";

    tok_t tok("UnicodeData.txt", "cl100k_base.tiktoken");
    printf("Loaded: vocab=%zu\n\n", tok.vocab_size());

    std::ifstream cf(corpus_path, std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);
    printf("Corpus: %zu bytes (%s)\n\n", len, corpus_path);

    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();
    auto ids = tok.encode(corpus.data(), len);
    auto t1 = clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    printf("Encode: %zu tokens in %.1f ms (%.1f MB/s)\n", ids.size(), ms, len/(ms*1e3));

    auto dec = tok.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
    printf("Round-trip: %s\n", ok ? "PASS" : "FAIL");

    // Also dump splitter chunks for diff against tiktoken
    auto chunks = ktoken::o200k::split(tok.trie(), corpus.data(), len);
    printf("Split: %zu chunks\n", chunks.size());
    size_t total = 0;
    for (auto& c : chunks) total += c.len;
    printf("Coverage: %zu / %zu bytes %s\n", total, len, total == len ? "OK" : "INCOMPLETE");

    return ok ? 0 : 1;
}
