// HuggingFace tokenizer.json loader — round-trip verified
#include "nlohmann_json.hpp"
#include "ktoken.hpp"
#include <cstdio>
#include <fstream>

using tok_t = ktoken::tokenizer<ktoken::cl100k, ktoken::huggingface_format>;

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "gpt2_tokenizer.json";
    printf("Loading: %s\n", path);
    tok_t tok("UnicodeData.txt", path);
    printf("Vocab: %zu\n\n", tok.vocab_size());

    // Test strings
    struct tc { const char* name; const char* text; };
    tc tests[] = {
        {"hello", "Hello, world!"}, {"space", " The quick brown fox"},
        {"newline", "line1\nline2"}, {"digits", "12345 67890"},
        {"unicode", "caf\xc3\xa9"}, {"empty", ""}, {"single", "A"},
    };

    bool all_ok = true;
    for (auto& t : tests) {
        auto data = reinterpret_cast<const uint8_t*>(t.text);
        auto len = (uint32_t)strlen(t.text);
        auto ids = tok.encode(data, len);
        auto dec = tok.decode(ids.data(), ids.size());
        bool ok = (dec.size() == len) && (len == 0 || memcmp(dec.data(), data, len) == 0);
        printf("  %-10s \"%s\" -> %zu tokens: %s\n", t.name, t.text, ids.size(), ok ? "PASS" : "FAIL");
        all_ok &= ok;
    }

    // Corpus round-trip
    printf("\nCorpus round-trip...\n");
    std::ifstream cf("corpus.txt", std::ios::binary|std::ios::ate);
    size_t len = cf.tellg(); cf.seekg(0);
    std::vector<uint8_t> corpus(len);
    cf.read(reinterpret_cast<char*>(corpus.data()), len);

    auto ids = tok.encode(corpus.data(), len);
    auto dec = tok.decode(ids.data(), ids.size());
    bool ok = (dec.size() == len) && memcmp(dec.data(), corpus.data(), len) == 0;
    printf("  %zu bytes -> %zu tokens -> %zu bytes: %s\n", len, ids.size(), dec.size(), ok ? "PASS" : "FAIL");
    all_ok &= ok;

    printf("\nOverall: %s\n", all_ok ? "ALL PASS" : "SOME FAILURES");
    return all_ok ? 0 : 1;
}
