#pragma once
// ktoken.hpp — compact BPE tokenizer built on kntrie
//
// Usage:
//   ktoken::tokenizer<> t("UnicodeData.txt", "cl100k_base.tiktoken");  // defaults: cl100k + tiktoken
//   auto ids = t.encode(text, len);
//   auto bytes = t.decode(ids.data(), ids.size());
//
//   // Other models/formats:
//   ktoken::tokenizer<ktoken::o200k> t2("UnicodeData.txt", "o200k_base.tiktoken");
//   ktoken::tokenizer<ktoken::p50k>  t3("UnicodeData.txt", "p50k_base.tiktoken");
//   ktoken::tokenizer<ktoken::cl100k, ktoken::huggingface_format> t4("UnicodeData.txt", "tokenizer.json");
//
//   // Train a new vocabulary:
//   auto trained = ktoken::tokenizer<>::train("UnicodeData.txt", corpus, len, 50000);

#include "kntrie.hpp"
#include "kstrie.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace ktoken {

// SSO byte string: 15 bytes inline (covers 99% of tokens), no heap alloc.
// Replaces vector<uint8_t> in decode table — eliminates ~100K heap allocations.
using byte_string = std::basic_string<uint8_t>;

// ===========================================================================
// Constants
// ===========================================================================

static constexpr uint32_t BASE_TOKENS       = 256;
static constexpr uint32_t NO_RANK           = UINT32_MAX;
static constexpr size_t   PAIR_SHIFT        = 32;
static constexpr int      MAX_PARTS         = 256;
static constexpr size_t   ASCII_COUNT       = 128;
static constexpr int      MAX_DIGIT_RUN     = 3;
static constexpr uint32_t SURROGATE_LO      = 0xD800;
static constexpr uint32_t SURROGATE_HI      = 0xDFFF;

inline constexpr uint64_t pack_pair(uint32_t l, uint32_t r) {
    return (uint64_t(l) << PAIR_SHIFT) | r;
}
inline constexpr uint32_t pair_left(uint64_t k)  { return (uint32_t)(k >> PAIR_SHIFT); }
inline constexpr uint32_t pair_right(uint64_t k) { return (uint32_t)(k & 0xFFFFFFFF); }

// ===========================================================================
// Character classification
// ===========================================================================

enum char_class : uint8_t {
    CL_LOWER = 0, CL_UPPER = 1, CL_OTHER_LETTER = 2, CL_MARK = 3,
    CL_DIGIT = 4, CL_WHITESPACE = 5, CL_NEWLINE = 6, CL_APOSTROPHE = 7,
    CL_OTHER = 8,
};

inline constexpr bool is_letter_or_mark(uint8_t c) { return c <= CL_MARK; }
inline constexpr bool is_ws(uint8_t c) { return c == CL_WHITESPACE || c == CL_NEWLINE; }

// ===========================================================================
// UTF-8
// ===========================================================================

inline constexpr int utf8_len(uint8_t lead) {
    if (lead < 0x80) return 1; if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3; return 4;
}

inline constexpr uint32_t decode_utf8(const uint8_t* p, int len) {
    if (len == 1) return p[0];
    if (len == 2) return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (len == 3) return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

inline int encode_utf8(uint32_t cp, char* buf) {
    if (cp <= 0x7F) { buf[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) { buf[0] = (char)(0xC0|(cp>>6)); buf[1] = (char)(0x80|(cp&0x3F)); return 2; }
    if (cp <= 0xFFFF) { buf[0] = (char)(0xE0|(cp>>12)); buf[1] = (char)(0x80|((cp>>6)&0x3F)); buf[2] = (char)(0x80|(cp&0x3F)); return 3; }
    buf[0] = (char)(0xF0|(cp>>18)); buf[1] = (char)(0x80|((cp>>12)&0x3F)); buf[2] = (char)(0x80|((cp>>6)&0x3F)); buf[3] = (char)(0x80|(cp&0x3F)); return 4;
}

// ===========================================================================
// Category trie + ASCII fast-path
// ===========================================================================

using cat_trie_t = gteitelbaum::kntrie<uint32_t, uint8_t>;

inline uint8_t g_ascii_class[ASCII_COUNT];

struct classify_result { uint8_t cls; uint8_t len; };

inline classify_result classify(const cat_trie_t& trie, const uint8_t* data, size_t remaining) {
    if (remaining == 0) return {CL_OTHER, 0};
    if (data[0] < ASCII_COUNT) return {g_ascii_class[data[0]], 1};
    int clen = utf8_len(data[0]);
    if ((size_t)clen > remaining) return {CL_OTHER, 1};
    uint32_t cp = decode_utf8(data, clen);
    auto it = trie.find(cp);
    return {static_cast<uint8_t>(it != trie.end() ? (*it).second : CL_OTHER), static_cast<uint8_t>(clen)};
}

inline cat_trie_t build_category_trie(const char* path) {
    cat_trie_t trie;
    std::ifstream f(path); std::string line;
    auto c2c = [](const char* cat) -> uint8_t {
        char c0 = cat[0], c1 = cat[1];
        if (c0 == 'L') { if (c1 == 'l') return CL_LOWER; if (c1 == 'u' || c1 == 't') return CL_UPPER; return CL_OTHER_LETTER; }
        if (c0 == 'M') return CL_MARK; if (c0 == 'N') return CL_DIGIT;
        if (c0 == 'Z' && c1 == 's') return CL_WHITESPACE; return CL_OTHER;
    };
    uint32_t rs = 0; uint8_t rc = 0;
    while (std::getline(f, line)) {
        const char* s = line.c_str();
        uint32_t cp = (uint32_t)strtoul(s, nullptr, 16);
        const char* p1 = strchr(s, ';'); if (!p1) continue; p1++;
        const char* p2 = strchr(p1, ';'); if (!p2) continue; p2++;
        char cat[3] = {p2[0], p2[1], 0};
        if (cp >= SURROGATE_LO && cp <= SURROGATE_HI) continue;
        uint8_t cls = c2c(cat);
        if (strstr(p1, "First>")) { rs = cp; rc = cls; continue; }
        if (strstr(p1, "Last>")) {
            for (uint32_t c = rs; c <= cp; ++c) {
                if (c >= SURROGATE_LO && c <= SURROGATE_HI) continue;
                trie.insert(c, rc);
            }
            continue;
        }
        trie.insert(cp, cls);
    }
    trie.insert_or_assign(0x27, CL_APOSTROPHE);
    trie.insert_or_assign(0x0A, CL_NEWLINE); trie.insert_or_assign(0x0D, CL_NEWLINE);
    trie.insert_or_assign(0x09, CL_WHITESPACE); trie.insert_or_assign(0x0B, CL_WHITESPACE);
    trie.insert_or_assign(0x0C, CL_WHITESPACE);
    std::memset(g_ascii_class, CL_OTHER, sizeof(g_ascii_class));
    for (uint32_t cp = 0; cp < ASCII_COUNT; ++cp) {
        auto it = trie.find(cp);
        if (it != trie.end()) g_ascii_class[cp] = (*it).second;
    }
    return trie;
}

// ===========================================================================
// Contraction check (case-insensitive, apostrophe at data[0])
// ===========================================================================

inline int check_contraction(const cat_trie_t& trie, const uint8_t* data, size_t remaining) {
    if (remaining < 2 || data[0] != '\'') return 0;
    uint8_t c1 = (uint8_t)std::tolower(data[1]);
    if (c1 == 's' || c1 == 't' || c1 == 'd' || c1 == 'm') {
        if (remaining >= 3) { auto [nc, nl] = classify(trie, data+2, remaining-2); if (is_letter_or_mark(nc)) return 0; }
        return 2;
    }
    if (remaining >= 3) {
        uint8_t c2 = (uint8_t)std::tolower(data[2]);
        if ((c1=='l'&&c2=='l')||(c1=='v'&&c2=='e')||(c1=='r'&&c2=='e')) {
            if (remaining >= 4) { auto [nc,nl] = classify(trie,data+3,remaining-3); if (is_letter_or_mark(nc)) return 0; }
            return 3;
        }
    }
    return 0;
}

// ===========================================================================
// Chunk range
// ===========================================================================

struct chunk_range { uint32_t off; uint32_t len; };

// ===========================================================================
// Whitespace handler (shared by cl100k and o200k)
// Implements: \s*[\r\n]+ | \s+(?!\S) | \s+
// ===========================================================================

inline size_t handle_whitespace(const cat_trie_t& trie, const uint8_t* data, size_t len,
                                 size_t pos, size_t start) {
    size_t run_end = pos, last_nl = SIZE_MAX, last_char_start = pos;
    while (run_end < len) {
        auto [c, l] = classify(trie, data + run_end, len - run_end);
        if (!is_ws(c)) break;
        if (c == CL_NEWLINE) last_nl = run_end + l;
        last_char_start = run_end;
        run_end += l;
    }
    if (last_nl != SIZE_MAX) return last_nl;
    if (run_end < len && last_char_start > start) return last_char_start;
    return run_end;
}

// ===========================================================================
// Model policy: cl100k
// ===========================================================================

struct cl100k {
    static constexpr bool is_punct(uint8_t c) {
        return !is_ws(c) && c > CL_MARK && c != CL_DIGIT;
    }
    static constexpr bool is_prefix(uint8_t c) {
        return c != CL_NEWLINE && c > CL_MARK && c != CL_DIGIT;
    }

    static std::vector<chunk_range> split(const cat_trie_t& trie, const uint8_t* data, size_t len) {
        std::vector<chunk_range> chunks;
        chunks.reserve(len / 3);
        size_t pos = 0;
        while (pos < len) {
            size_t start = pos;
            auto [cls, clen] = classify(trie, data+pos, len-pos);

            if (cls == CL_APOSTROPHE) {
                int cm = check_contraction(trie, data+pos, len-pos);
                if (cm > 0) { chunks.push_back({(uint32_t)start,(uint32_t)cm}); pos += cm; continue; }
            }
            if (is_prefix(cls)) {
                size_t probe = pos + clen;
                if (probe < len) {
                    auto [nc, nl] = classify(trie, data+probe, len-probe);
                    if (is_letter_or_mark(nc)) {
                        pos = probe;
                        while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_letter_or_mark(c2)) break; pos+=l2; }
                        chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)}); continue;
                    }
                }
            }
            if (is_letter_or_mark(cls)) {
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_letter_or_mark(c2)) break; pos+=l2; }
                chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)}); continue;
            }
            if (cls == CL_DIGIT) {
                int count = 0;
                while (pos < len && count < MAX_DIGIT_RUN) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (c2!=CL_DIGIT) break; pos+=l2; count++; }
                chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)}); continue;
            }
            if (cls == CL_WHITESPACE) {
                size_t probe = pos + clen;
                if (probe < len) { auto [nc,nl] = classify(trie,data+probe,len-probe); if (is_punct(nc)) { pos = probe; goto punct; } }
            }
            if (is_punct(cls)) { punct:
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_punct(c2)) break; pos+=l2; }
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (c2!=CL_NEWLINE) break; pos+=l2; }
                chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)}); continue;
            }
            if (is_ws(cls)) {
                pos = handle_whitespace(trie, data, len, pos, start);
                chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)}); continue;
            }
            pos += clen; chunks.push_back({(uint32_t)start,(uint32_t)(pos-start)});
        }
        return chunks;
    }
};

// ===========================================================================
// Model policy: o200k
// ===========================================================================

struct o200k {
    static constexpr bool in_upper_set(uint8_t c) { return c == CL_UPPER || c == CL_OTHER_LETTER || c == CL_MARK; }
    static constexpr bool in_lower_set(uint8_t c) { return c == CL_LOWER || c == CL_OTHER_LETTER || c == CL_MARK; }
    static constexpr bool is_punct(uint8_t c) { return c == CL_MARK || c == CL_APOSTROPHE || c == CL_OTHER; }
    static constexpr bool is_prefix(uint8_t c) { return c == CL_MARK || c == CL_WHITESPACE || c == CL_APOSTROPHE || c == CL_OTHER; }

    static size_t scan_lower(const cat_trie_t& t, const uint8_t* d, size_t len, size_t pos) {
        while (pos < len) { auto [c,l] = classify(t,d+pos,len-pos); if (!in_lower_set(c)) break; pos+=l; } return pos;
    }
    static size_t try_contraction(const cat_trie_t& t, const uint8_t* d, size_t len, size_t pos) {
        if (pos < len) { int cm = check_contraction(t, d+pos, len-pos); if (cm > 0) pos += cm; } return pos;
    }
    static size_t scan_punct_run(const cat_trie_t& t, const uint8_t* d, size_t len, size_t pos) {
        while (pos < len) { auto [c,l] = classify(t,d+pos,len-pos); if (!is_punct(c)) break; pos+=l; }
        while (pos < len) { auto [c,l] = classify(t,d+pos,len-pos); if (c==CL_NEWLINE){pos+=l;continue;} if(d[pos]=='/'){pos+=1;continue;} break; }
        return pos;
    }

    static std::vector<chunk_range> split(const cat_trie_t& trie, const uint8_t* data, size_t len) {
        std::vector<chunk_range> out;
        out.reserve(len / 3);
        size_t pos = 0;
        auto emit = [&](size_t s, size_t e) { if (e > s) out.push_back({(uint32_t)s,(uint32_t)(e-s)}); };

        while (pos < len) {
            size_t start = pos;
            auto [cls, clen] = classify(trie, data+pos, len-pos);

            // Optional prefix
            size_t letter_start = pos;
            if (is_prefix(cls)) {
                size_t after = pos + clen;
                if (after < len) {
                    auto [nc,nl] = classify(trie,data+after,len-after);
                    if (in_upper_set(nc)||in_lower_set(nc)) { letter_start = after; cls = nc; clen = nl; }
                }
            }

            // Upper run with backtracking
            if (in_upper_set(cls)) {
                size_t upper_end = letter_start + clen;
                while (upper_end < len) { auto [c2,l2] = classify(trie,data+upper_end,len-upper_end); if (!in_upper_set(c2)) break; upper_end+=l2; }

                if (upper_end < len) { auto [c2,l2] = classify(trie,data+upper_end,len-upper_end);
                    if (in_lower_set(c2)) { pos = try_contraction(trie,data,len,scan_lower(trie,data,len,upper_end)); emit(start,pos); continue; }
                }

                // Backtrack: find last non-CL_UPPER
                size_t split_pos = letter_start; bool found = false;
                { struct ci { size_t p; uint8_t c; uint8_t l; }; std::vector<ci> chars;
                  size_t scan = letter_start;
                  while (scan < upper_end) { auto [c2,l2] = classify(trie,data+scan,len-scan); chars.push_back({scan,c2,l2}); scan+=l2; }
                  for (int i = (int)chars.size()-1; i >= 0; --i) { if (chars[i].c != CL_UPPER) { split_pos = chars[i].p; found = true; break; } }
                }
                if (found) { pos = try_contraction(trie,data,len,scan_lower(trie,data,len,split_pos)); emit(start,pos); continue; }

                pos = try_contraction(trie,data,len,upper_end); emit(start,pos); continue;
            }

            if (in_lower_set(cls)) { pos = try_contraction(trie,data,len,scan_lower(trie,data,len,letter_start+clen)); emit(start,pos); continue; }
            if (cls == CL_DIGIT) { int count=0; while(pos<len&&count<MAX_DIGIT_RUN){auto[c,l]=classify(trie,data+pos,len-pos);if(c!=CL_DIGIT)break;pos+=l;count++;} emit(start,pos); continue; }
            if (cls == CL_WHITESPACE && data[pos] == 0x20) {
                size_t probe = pos+1;
                if (probe<len) { auto[nc,nl]=classify(trie,data+probe,len-probe); if(is_punct(nc)){pos=scan_punct_run(trie,data,len,probe);emit(start,pos);continue;} }
            }
            if (is_punct(cls)) { pos = scan_punct_run(trie,data,len,pos); emit(start,pos); continue; }
            if (is_ws(cls)) { pos = handle_whitespace(trie,data,len,pos,start); emit(start,pos); continue; }
            pos += clen; emit(start,pos);
        }
        return out;
    }
};

// ===========================================================================
// Model policy: p50k (GPT-2 / GPT-3 pattern)
// '(?:[sdmt]|ll|ve|re)| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+$|\s+(?!\S)|\s+
// Key differences from cl100k:
//   - Contractions are case-SENSITIVE and standalone (not attached to preceding word)
//   - No digit grouping limit
//   - Prefix is literal space (0x20) only
//   - \s+$ matches trailing whitespace at end of string
// ===========================================================================

struct p50k {
    // [^\s\p{L}\p{N}]+ — \p{M} is NOT \p{L}, so marks are punctuation
    static constexpr bool is_punct(uint8_t c) {
        return c == CL_MARK || c == CL_APOSTROPHE || c == CL_OTHER;
    }

    // p50k \p{L}+ does NOT include \p{M}
    static constexpr bool is_letter(uint8_t c) {
        return c <= CL_OTHER_LETTER;  // LOWER, UPPER, OTHER_LETTER only
    }

    // Case-sensitive contraction: 's 't 'd 'm 'll 've 're (lowercase only)
    // Lookahead uses is_letter (no marks) — p50k's \p{L} excludes \p{M}
    static int check_contraction_cs(const cat_trie_t& trie, const uint8_t* data, size_t remaining) {
        if (remaining < 2 || data[0] != '\'') return 0;
        uint8_t c1 = data[1]; // no tolower — case sensitive
        if (c1 == 's' || c1 == 't' || c1 == 'd' || c1 == 'm') {
            if (remaining >= 3) {
                auto [nc, nl] = classify(trie, data + 2, remaining - 2);
                if (is_letter(nc)) return 0;
            }
            return 2;
        }
        if (remaining >= 3) {
            uint8_t c2 = data[2];
            if ((c1 == 'l' && c2 == 'l') || (c1 == 'v' && c2 == 'e') || (c1 == 'r' && c2 == 'e')) {
                if (remaining >= 4) {
                    auto [nc, nl] = classify(trie, data + 3, remaining - 3);
                    if (is_letter(nc)) return 0;
                }
                return 3;
            }
        }
        return 0;
    }

    static std::vector<chunk_range> split(const cat_trie_t& trie, const uint8_t* data, size_t len) {
        std::vector<chunk_range> chunks;
        chunks.reserve(len / 3);
        size_t pos = 0;
        while (pos < len) {
            size_t start = pos;
            auto [cls, clen] = classify(trie, data + pos, len - pos);

            // Contractions: '(?:[sdmt]|ll|ve|re) — standalone, case-sensitive
            if (cls == CL_APOSTROPHE) {
                int cm = check_contraction_cs(trie, data + pos, len - pos);
                if (cm > 0) { chunks.push_back({(uint32_t)start, (uint32_t)cm}); pos += cm; continue; }
            }

            // Optional space prefix + letters: " ?\p{L}+"
            if (cls == CL_WHITESPACE && data[pos] == 0x20) {
                size_t probe = pos + 1;
                if (probe < len) {
                    auto [nc, nl] = classify(trie, data + probe, len - probe);
                    if (is_letter(nc)) {
                        pos = probe;
                        while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_letter(c2)) break; pos+=l2; }
                        chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
                    }
                    // " ?[^\s\p{L}\p{N}]+"
                    if (is_punct(nc)) {
                        pos = probe;
                        while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_punct(c2)) break; pos+=l2; }
                        chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
                    }
                    // " ?\p{N}+"
                    if (nc == CL_DIGIT) {
                        pos = probe;
                        while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (c2!=CL_DIGIT) break; pos+=l2; }
                        chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
                    }
                }
            }

            // Letters: \p{L}+ (no marks)
            if (is_letter(cls)) {
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_letter(c2)) break; pos+=l2; }
                chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
            }

            // Digits: \p{N}+ (no limit)
            if (cls == CL_DIGIT) {
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (c2!=CL_DIGIT) break; pos+=l2; }
                chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
            }

            // Punctuation: [^\s\p{L}\p{N}]+
            if (is_punct(cls)) {
                while (pos < len) { auto [c2,l2] = classify(trie,data+pos,len-pos); if (!is_punct(c2)) break; pos+=l2; }
                chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
            }

            // Whitespace: \s+$|\s+(?!\S)|\s+
            if (is_ws(cls)) {
                size_t run_end = pos;
                while (run_end < len) { auto [c2,l2] = classify(trie,data+run_end,len-run_end); if (!is_ws(c2)) break; run_end+=l2; }
                // \s+$ — at end of string, consume all
                if (run_end == len) { pos = run_end; chunks.push_back({(uint32_t)start, (uint32_t)(pos-start)}); continue; }
                // \s+(?!\S) — leave last char if followed by non-ws
                size_t last_char_start = pos;
                { size_t scan = pos; while (scan < run_end) { last_char_start = scan; auto [c2,l2] = classify(trie,data+scan,len-scan); scan+=l2; } }
                if (last_char_start > start) { pos = last_char_start; }
                else { pos = run_end; }
                chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)}); continue;
            }

            // Fallback
            pos += clen;
            chunks.push_back({(uint32_t)start, (uint32_t)(pos - start)});
        }
        return chunks;
    }
};

// ===========================================================================
// Vocab data — the core encoder state
// ===========================================================================

struct vocab_data {
    std::vector<byte_string> decode;
    uint32_t byte_rank[BASE_TOKENS];
    gteitelbaum::kntrie<uint64_t, uint32_t> pair_trie;
    // Flat lookup for byte-byte pairs: eliminates kntrie traversal on init.
    // 256 * 256 * 4 = 256KB, fits in L2 cache.
    uint32_t byte_pair_rank[BASE_TOKENS][BASE_TOKENS];

    void build_byte_pair_table() {
        for (uint32_t a = 0; a < BASE_TOKENS; ++a)
            for (uint32_t b = 0; b < BASE_TOKENS; ++b) {
                auto it = pair_trie.find(pack_pair(byte_rank[a], byte_rank[b]));
                byte_pair_rank[a][b] = it != pair_trie.end() ? (*it).second : NO_RANK;
            }
    }
};

// ===========================================================================
// BPE encode/decode engine
// ===========================================================================

struct bpe_scratch {
    uint32_t parts[MAX_PARTS];
    uint32_t pair_ranks[MAX_PARTS];
    uint32_t result[MAX_PARTS];
    uint32_t result_count;
};

// Min-scan: [[unlikely]] tells the compiler the update is rare —
// the minimum is typically found in the first few positions (BPE rank ordering),
// then the branch stays "not taken" for the remaining elements.
// NOTE: This hint is input-distribution-dependent. On adversarial inputs
// (ascending rank sequences, short repeated merge sequences) the branch becomes
// frequently taken and the cmov codegen is a pessimization. Paper benchmarks
// should note this distribution dependency.
// Kept as a separate inline function for clarity.
inline uint32_t find_min_pair(const uint32_t* pair_ranks, uint32_t n, uint32_t& out_rank) {
    uint32_t mr = NO_RANK, mi = 0;
    for (uint32_t i = 0; i + 1 < n; ++i)
        if (pair_ranks[i] < mr) [[unlikely]]
        { mr = pair_ranks[i]; mi = i; }
    out_rank = mr;
    return mi;
}

inline void bpe_encode_chunk(const vocab_data& vd, const uint8_t* data, uint32_t len,
                              bpe_scratch& s) {
    s.result_count = 0;
    if (len == 0) return;
    if (len == 1) { s.result[0] = vd.byte_rank[data[0]]; s.result_count = 1; return; }

    uint32_t n = (len > MAX_PARTS) ? MAX_PARTS : len;
    for (uint32_t i = 0; i < n; ++i) s.parts[i] = vd.byte_rank[data[i]];
    // First pass is always byte-byte pairs: use flat table, no kntrie
    for (uint32_t i = 0; i+1 < n; ++i)
        s.pair_ranks[i] = vd.byte_pair_rank[data[i]][data[i+1]];
    s.pair_ranks[n-1] = NO_RANK;

    for (;;) {
        uint32_t mr;
        uint32_t mi = find_min_pair(s.pair_ranks, n, mr);
        if (mr == NO_RANK) break;

        // Merge at mi, shift tail left, shrink n
        s.parts[mi] = mr;
        uint32_t tail = n - mi - 2;
        std::memmove(&s.parts[mi+1], &s.parts[mi+2], tail * 4);
        std::memmove(&s.pair_ranks[mi+1], &s.pair_ranks[mi+2], tail * 4);
        n--;

        // Left neighbor
        if (mi > 0) [[likely]] {
            auto it = vd.pair_trie.find(pack_pair(s.parts[mi-1], s.parts[mi]));
            s.pair_ranks[mi-1] = it != vd.pair_trie.end() ? (*it).second : NO_RANK;
        }

        // Right neighbor
        if (mi + 1 < n) [[likely]] {
            auto it = vd.pair_trie.find(pack_pair(s.parts[mi], s.parts[mi+1]));
            s.pair_ranks[mi] = it != vd.pair_trie.end() ? (*it).second : NO_RANK;
        }
    }
    std::memcpy(s.result, s.parts, n * sizeof(uint32_t));
    s.result_count = n;
}

// ===========================================================================
// BPE simulation merge recovery (for tiktoken format)
// ===========================================================================

inline void recover_merge_pairs(vocab_data& vd) {
    uint32_t max_rank = (uint32_t)vd.decode.size() - 1;
    uint32_t parts[MAX_PARTS], pr[MAX_PARTS];
    for (uint32_t r = BASE_TOKENS; r <= max_rank; ++r) {
        const auto& bytes = vd.decode[r];
        if (bytes.empty() || bytes.size() == 1 || bytes.size() > (size_t)MAX_PARTS) continue;
        uint32_t n = (uint32_t)bytes.size(); bool ok = true;
        for (uint32_t i = 0; i < n; ++i) { parts[i] = vd.byte_rank[static_cast<uint8_t>(bytes[i])]; if (parts[i] == NO_RANK) { ok = false; break; } }
        if (!ok) continue;
        for (uint32_t i = 0; i+1 < n; ++i) {
            auto it = vd.pair_trie.find(pack_pair(parts[i], parts[i+1]));
            pr[i] = (it != vd.pair_trie.end() && (*it).second < r) ? (*it).second : NO_RANK;
        }
        pr[n-1] = NO_RANK;
        while (n > 2) {
            uint32_t mr = NO_RANK, mi = 0;
            for (uint32_t i = 0; i+1 < n; ++i) if (pr[i] < mr) { mr = pr[i]; mi = i; }
            if (mr == NO_RANK) break;
            parts[mi] = mr; uint32_t rm = mi+1, tail = n-rm-1;
            if (tail > 0) { std::memmove(&parts[rm],&parts[rm+1],tail*4); std::memmove(&pr[rm],&pr[rm+1],tail*4); }
            n--;
            if (mi+1<n) { auto it = vd.pair_trie.find(pack_pair(parts[mi], parts[mi+1]));
                           pr[mi] = (it != vd.pair_trie.end() && (*it).second < r) ? (*it).second : NO_RANK;
            } else pr[mi] = NO_RANK;
            if (mi>0) { auto it = vd.pair_trie.find(pack_pair(parts[mi-1], parts[mi]));
                         pr[mi-1] = (it != vd.pair_trie.end() && (*it).second < r) ? (*it).second : NO_RANK; }
        }
        if (n == 2) vd.pair_trie.insert(pack_pair(parts[0], parts[1]), r);
    }
}

// ===========================================================================
// Format policy: tiktoken (.tiktoken base64 format)
// ===========================================================================

namespace detail {
    inline constexpr std::array<uint8_t, 256> make_b64_table() {
        std::array<uint8_t,256> t{}; for (auto& v:t) v=255;
        for (int i=0;i<26;++i){t['A'+i]=i;t['a'+i]=i+26;}
        for (int i=0;i<10;++i) t['0'+i]=i+52; t['+']=62;t['/']=63; return t;
    }
    inline byte_string b64_decode(std::string_view s) {
        static constexpr auto B64 = make_b64_table();
        byte_string o; o.reserve(s.size()*3/4);
        uint32_t a=0;int b=0;
        for (char c:s){uint8_t v=B64[(uint8_t)c];if(v==255)continue;a=(a<<6)|v;b+=6;if(b>=8){b-=8;o.push_back(static_cast<uint8_t>((a>>b)&0xFF));}}
        return o;
    }
}

struct tiktoken_format {
    static vocab_data load(const char* path) {
        vocab_data vd;
        std::ifstream f(path); std::string line;
        std::vector<std::pair<byte_string,uint32_t>> entries;
        entries.reserve(200000);
        uint32_t max_rank = 0;
        while (std::getline(f, line)) {
            size_t sp = line.find(' '); if (sp == std::string::npos) continue;
            auto bytes = detail::b64_decode(std::string_view(line.data(), sp));
            uint32_t rank = (uint32_t)strtoul(line.data()+sp+1, nullptr, 10);
            if (rank > max_rank) max_rank = rank;
            entries.push_back({std::move(bytes), rank});
        }
        vd.decode.resize(max_rank + 1);
        for (auto& [bytes, rank] : entries) vd.decode[rank] = std::move(bytes);
        std::memset(vd.byte_rank, 0xFF, sizeof(vd.byte_rank));
        for (uint32_t r = 0; r < BASE_TOKENS && r <= max_rank; ++r)
            if (vd.decode[r].size() == 1) vd.byte_rank[static_cast<uint8_t>(vd.decode[r][0])] = r;
        recover_merge_pairs(vd);
        vd.build_byte_pair_table();
        return vd;
    }
};

// ===========================================================================
// Format policy: HuggingFace (tokenizer.json, requires nlohmann/json)
// Include nlohmann_json.hpp before ktoken.hpp to enable this.
// ===========================================================================

struct byte_unicode_map {
    char32_t byte_to_unicode[BASE_TOKENS];
    uint8_t unicode_to_byte[512];
    static constexpr size_t UNI_MAP_SIZE = 512;

    byte_unicode_map() {
        std::memset(byte_to_unicode, 0, sizeof(byte_to_unicode));
        std::memset(unicode_to_byte, 0xFF, sizeof(unicode_to_byte));
        bool direct[BASE_TOKENS] = {};
        auto mark = [&](int lo, int hi) { for (int b=lo;b<=hi;++b) { direct[b]=true; byte_to_unicode[b]=(char32_t)b; unicode_to_byte[b]=(uint8_t)b; } };
        mark(0x21,0x7E); mark(0xA1,0xAC); mark(0xAE,0xFF);
        char32_t next = 0x0100;
        for (int b=0;b<256;++b) { if (!direct[b]) { byte_to_unicode[b]=next; if(next<UNI_MAP_SIZE)unicode_to_byte[next]=(uint8_t)b; next++; } }
    }

    byte_string decode_str(const std::string& s) const {
        byte_string result;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
        size_t len = s.size(), i = 0;
        while (i < len) {
            char32_t cp;
            if (p[i]<0x80){cp=p[i];i+=1;} else if(p[i]<0xE0){cp=((p[i]&0x1F)<<6)|(p[i+1]&0x3F);i+=2;}
            else if(p[i]<0xF0){cp=((p[i]&0x0F)<<12)|((p[i+1]&0x3F)<<6)|(p[i+2]&0x3F);i+=3;}
            else{cp=((p[i]&0x07)<<18)|((p[i+1]&0x3F)<<12)|((p[i+2]&0x3F)<<6)|(p[i+3]&0x3F);i+=4;}
            if (cp < UNI_MAP_SIZE && unicode_to_byte[cp] != 0xFF) { result.push_back(unicode_to_byte[cp]); }
            else { uint8_t buf[4]; int n;
                if(cp<=0x7F){buf[0]=(uint8_t)cp;n=1;}else if(cp<=0x7FF){buf[0]=(uint8_t)(0xC0|(cp>>6));buf[1]=(uint8_t)(0x80|(cp&0x3F));n=2;}
                else if(cp<=0xFFFF){buf[0]=(uint8_t)(0xE0|(cp>>12));buf[1]=(uint8_t)(0x80|((cp>>6)&0x3F));buf[2]=(uint8_t)(0x80|(cp&0x3F));n=3;}
                else{buf[0]=(uint8_t)(0xF0|(cp>>18));buf[1]=(uint8_t)(0x80|((cp>>12)&0x3F));buf[2]=(uint8_t)(0x80|((cp>>6)&0x3F));buf[3]=(uint8_t)(0x80|(cp&0x3F));n=4;}
                for (int j=0;j<n;++j) result.push_back(buf[j]);
            }
        }
        return result;
    }
};

#ifdef NLOHMANN_JSON_VERSION_MAJOR
struct huggingface_format {
    static vocab_data load(const char* path) {
        vocab_data vd;
        std::ifstream f(path);
        nlohmann::json j; f >> j;
        byte_unicode_map bmap;
        auto& model = j["model"];
        auto& vocab = model["vocab"];

        vd.decode.resize((uint32_t)vocab.size());
        std::memset(vd.byte_rank, 0xFF, sizeof(vd.byte_rank));
        gteitelbaum::kstrie<uint32_t> str_to_rank;
        for (auto& [key, val] : vocab.items()) {
            uint32_t rank = val.get<uint32_t>();
            if (rank >= vd.decode.size()) vd.decode.resize(rank + 1);
            vd.decode[rank] = bmap.decode_str(key);
            if (vd.decode[rank].size() == 1) vd.byte_rank[static_cast<uint8_t>(vd.decode[rank][0])] = rank;
            str_to_rank.insert_or_assign(key, rank);
        }

        auto& merges = model["merges"];
        for (size_t mi = 0; mi < merges.size(); ++mi) {
            std::string ms = merges[mi].get<std::string>();
            size_t sp = ms.find(' '); if (sp == std::string::npos) continue;
            auto lit = str_to_rank.find(ms.substr(0, sp));
            auto rit = str_to_rank.find(ms.substr(sp + 1));
            if (lit == str_to_rank.end() || rit == str_to_rank.end()) continue;
            auto mit = str_to_rank.find(ms.substr(0, sp) + ms.substr(sp + 1));
            if (mit == str_to_rank.end()) continue;
            vd.pair_trie.insert(pack_pair((*lit).second, (*rit).second), (*mit).second);
        }
        vd.build_byte_pair_table();
        return vd;
    }
};
#endif

// ===========================================================================
// Tokenizer — the public API
// ===========================================================================

template<typename Model = cl100k, typename Format = tiktoken_format>
class tokenizer {
    cat_trie_t cat_trie_;
    vocab_data vocab_;
    bool ready_ = false;

public:
    tokenizer() = default;

    // Construct from vocab file
    tokenizer(const char* unicode_db_path, const char* vocab_path) {
        cat_trie_ = build_category_trie(unicode_db_path);
        vocab_ = Format::load(vocab_path);
        ready_ = true;
    }

    // Construct from pre-built vocab (used by trainer)
    tokenizer(cat_trie_t trie, vocab_data vd)
        : cat_trie_(std::move(trie)), vocab_(std::move(vd)), ready_(true) {}

    bool ready() const { return ready_; }
    size_t vocab_size() const { return vocab_.decode.size(); }
    const vocab_data& vocab() const { return vocab_; }
    const cat_trie_t& trie() const { return cat_trie_; }

    // Encode bytes → token IDs
    std::vector<uint32_t> encode(const uint8_t* data, size_t len) const {
        auto chunks = Model::split(cat_trie_, data, len);
        std::vector<uint32_t> tokens;
        tokens.reserve(len / 3);
        bpe_scratch s;
        for (auto& c : chunks) {
            bpe_encode_chunk(vocab_, data + c.off, c.len, s);
            tokens.insert(tokens.end(), s.result, s.result + s.result_count);
        }
        return tokens;
    }

    // Decode token IDs → bytes
    std::vector<uint8_t> decode(const uint32_t* tokens, size_t count) const {
        std::vector<uint8_t> out;
        for (size_t i = 0; i < count; ++i) {
            if (tokens[i] < vocab_.decode.size()) {
                auto& bytes = vocab_.decode[tokens[i]];
                out.insert(out.end(), bytes.begin(), bytes.end());
            }
        }
        return out;
    }

    // Train a new vocabulary from corpus
    static tokenizer train(const char* unicode_db_path,
                           const uint8_t* corpus, size_t len,
                           uint32_t target_vocab_size) {
        uint32_t num_merges = (target_vocab_size > BASE_TOKENS) ? target_vocab_size - BASE_TOKENS : 0;
        cat_trie_t trie = build_category_trie(unicode_db_path);
        auto ranges = Model::split(trie, corpus, len);

        static constexpr size_t   HOT_INDEX_BITS = 16;
        static constexpr uint16_t NOT_HOT        = 0;
        static constexpr uint32_t HOT_DIVISOR    = 2;
        static constexpr uint32_t HOT_MAX_SLOTS  = UINT16_MAX;

        auto pack_hot = [](uint16_t hl, uint16_t hr) -> uint32_t {
            return (uint32_t(hl) << HOT_INDEX_BITS) | hr;
        };

        // Deduplicate chunks
        struct unique_chunk { std::vector<uint32_t> tokens; uint64_t freq; };
        std::vector<unique_chunk> chunks;
        { gteitelbaum::kstrie<uint32_t> seen;
          for (auto& r : ranges) {
              std::string_view key(reinterpret_cast<const char*>(corpus + r.off), r.len);
              auto it = seen.find(key);
              if (it != seen.end()) { chunks[(*it).second].freq++; }
              else {
                  seen.insert(key, (uint32_t)chunks.size());
                  unique_chunk uc; uc.freq = 1; uc.tokens.resize(r.len);
                  for (uint32_t j = 0; j < r.len; ++j) uc.tokens[j] = corpus[r.off + j];
                  chunks.push_back(std::move(uc));
              }
          }
        }

        // Hot index — fill in merge order, no eviction
        uint32_t max_hot = std::min(num_merges / HOT_DIVISOR, HOT_MAX_SLOTS);
        std::vector<uint16_t> rank_to_hot;
        std::vector<uint32_t> hot_to_rank;
        uint16_t next_hot_index = 1;

        auto get_hot = [&](uint32_t rank) -> uint16_t {
            return (rank < rank_to_hot.size()) ? rank_to_hot[rank] : NOT_HOT;
        };
        auto make_hot = [&](uint32_t rank) -> uint16_t {
            if (rank >= rank_to_hot.size()) rank_to_hot.resize(rank + 1, NOT_HOT);
            if (rank_to_hot[rank] != NOT_HOT) return rank_to_hot[rank];
            if (next_hot_index > max_hot) return NOT_HOT;
            uint16_t idx = next_hot_index++;
            rank_to_hot[rank] = idx;
            if (idx >= hot_to_rank.size()) hot_to_rank.resize(idx + 1);
            hot_to_rank[idx] = rank;
            return idx;
        };

        // Make all base bytes hot
        rank_to_hot.resize(BASE_TOKENS, NOT_HOT);
        for (uint32_t b = 0; b < BASE_TOKENS; ++b) make_hot(b);

        // Full pair counts + hot cache + hot inverted index
        gteitelbaum::kntrie<uint64_t, int64_t> pair_counts;
        gteitelbaum::kntrie<uint32_t, int64_t> hot_cache;
        gteitelbaum::kntrie<uint32_t, std::vector<uint32_t>> hot_inv;

        for (uint32_t ci = 0; ci < (uint32_t)chunks.size(); ++ci) {
            auto& c = chunks[ci];
            int64_t w = (int64_t)c.freq;
            for (size_t i = 0; i + 1 < c.tokens.size(); ++i) {
                uint32_t L = c.tokens[i], R = c.tokens[i + 1];
                pair_counts[pack_pair(L, R)] += w;
                uint16_t hL = get_hot(L), hR = get_hot(R);
                if (hL != NOT_HOT && hR != NOT_HOT) {
                    uint32_t hkey = pack_hot(hL, hR);
                    hot_cache[hkey] += w;
                    hot_inv[hkey].push_back(ci);
                }
            }
        }
        for (auto it = hot_inv.begin(); it != hot_inv.end(); ++it) {
            auto& v = (*it).second;
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        using heap_entry = std::pair<int64_t, uint64_t>;
        std::priority_queue<heap_entry> heap;
        for (auto [key, count] : pair_counts)
            if (count > 0) heap.push({count, key});

        // Decode table + byte ranks
        vocab_data vd;
        vd.decode.resize(BASE_TOKENS);
        for (uint32_t b = 0; b < BASE_TOKENS; ++b) vd.decode[b] = byte_string(1, static_cast<uint8_t>(b));
        std::memset(vd.byte_rank, 0xFF, sizeof(vd.byte_rank));
        for (uint32_t b = 0; b < BASE_TOKENS; ++b) vd.byte_rank[b] = b;
        uint32_t next_rank = BASE_TOKENS;

        // Adjust pair count in both full and hot cache
        auto adjust_pair = [&](uint32_t L, uint32_t R, int64_t delta, uint32_t chunk_id) {
            uint64_t key64 = pack_pair(L, R);
            auto& pc = pair_counts[key64];
            pc += delta;
            int64_t nc = pc;
            if (delta > 0 && nc > 0) heap.push({nc, key64});
            uint16_t hL = get_hot(L), hR = get_hot(R);
            if (hL != NOT_HOT && hR != NOT_HOT) {
                uint32_t hkey = pack_hot(hL, hR);
                hot_cache[hkey] += delta;
                if (delta > 0) hot_inv[hkey].push_back(chunk_id);
            }
        };

        // Merge a pair in a set of chunks
        auto merge_chunks = [&](const std::vector<uint32_t>& chunk_ids,
                                uint32_t left, uint32_t right, uint32_t new_rank) {
            for (uint32_t ci : chunk_ids) {
                auto& c = chunks[ci];
                auto& tok = c.tokens;
                int64_t w = (int64_t)c.freq;
                size_t i = 0;
                while (i + 1 < tok.size()) {
                    if (tok[i] != left || tok[i + 1] != right) { ++i; continue; }
                    if (i > 0) adjust_pair(tok[i - 1], left, -w, ci);
                    if (i + 2 < tok.size()) adjust_pair(right, tok[i + 2], -w, ci);
                    tok[i] = new_rank;
                    tok.erase(tok.begin() + i + 1);
                    if (i > 0) adjust_pair(tok[i - 1], new_rank, w, ci);
                    if (i + 1 < tok.size()) adjust_pair(new_rank, tok[i + 1], w, ci);
                }
            }
        };

        // Train
        for (uint32_t m = 0; m < num_merges; ++m) {
            // Find best pair
            int64_t best_count = 0; uint64_t best_key = 0;
            while (!heap.empty()) {
                auto [c, k] = heap.top(); heap.pop();
                auto cur = pair_counts.find(k);
                if (cur != pair_counts.end() && (*cur).second == c && c > 0) { best_count = c; best_key = k; break; }
            }
            if (best_count <= 0) break;

            uint32_t left = pair_left(best_key), right = pair_right(best_key);
            uint32_t nr = next_rank++;
            vd.decode.resize(nr + 1);
            vd.decode[nr] = vd.decode[left] + vd.decode[right];
            vd.pair_trie.insert(pack_pair(left, right), nr);

            // Zero out merged pair
            pair_counts.insert_or_assign(best_key, int64_t(0));
            uint16_t hL = get_hot(left), hR = get_hot(right);
            if (hL != NOT_HOT && hR != NOT_HOT)
                hot_cache.insert_or_assign(pack_hot(hL, hR), int64_t(0));

            // Capture hot chunk list before promotion
            std::vector<uint32_t> hot_chunk_ids;
            bool is_hot = (hL != NOT_HOT && hR != NOT_HOT);
            if (is_hot) {
                uint32_t hkey = pack_hot(hL, hR);
                auto inv_it = hot_inv.find(hkey);
                if (inv_it != hot_inv.end()) {
                    hot_chunk_ids = (*inv_it).second;
                    hot_inv.erase(hkey);
                    std::sort(hot_chunk_ids.begin(), hot_chunk_ids.end());
                    hot_chunk_ids.erase(std::unique(hot_chunk_ids.begin(), hot_chunk_ids.end()),
                                        hot_chunk_ids.end());
                }
            }

            // Promote new rank to hot before scan
            make_hot(nr);

            // Hot path: scan only tagged chunks. Cold: full scan.
            if (is_hot) {
                if (!hot_chunk_ids.empty())
                    merge_chunks(hot_chunk_ids, left, right, nr);
            } else {
                std::vector<uint32_t> all_ids(chunks.size());
                for (uint32_t i = 0; i < (uint32_t)chunks.size(); ++i) all_ids[i] = i;
                merge_chunks(all_ids, left, right, nr);
            }
        }

        vd.build_byte_pair_table();
        return tokenizer(std::move(trie), std::move(vd));
    }
};

} // namespace ktoken
