// BPE training benchmark using tokenizer::train API
#include "ktoken.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::cl100k, ktoken::tiktoken_format>;

int main(int argc, char** argv) {
    uint32_t target_vocab = (argc > 1) ? (uint32_t)atoi(argv[1]) + ktoken::BASE_TOKENS : 756;

    std::ifstream cf("corpus.txt", std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);
    printf("Corpus: %zu bytes, target vocab: %u (%u merges)\n\n",
           len, target_vocab, target_vocab - ktoken::BASE_TOKENS);

    using clock = std::chrono::high_resolution_clock;
    auto t0 = clock::now();
    auto trained = tok_t::train("UnicodeData.txt", corpus.data(), len, target_vocab);
    auto t1 = clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    printf("Trained: vocab=%zu in %.0f ms (%.0f merges/sec)\n",
           trained.vocab_size(), ms, (target_vocab - ktoken::BASE_TOKENS) / (ms / 1e3));

    // Encode + decode round-trip
    auto ids = trained.encode(corpus.data(), len);
    auto dec = trained.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
    printf("Encode: %zu tokens (%.1f bytes/token)\n", ids.size(), (double)len / ids.size());
    printf("Round-trip: %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}
