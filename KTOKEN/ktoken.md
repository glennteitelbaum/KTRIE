# ktoken

A compact BPE tokenizer built on kntrie. 4.2MB total working set shared across all threads — at 128 threads, tiktoken needs 145MB while ktoken stays at 4.2MB. 2.7x faster single-threaded. No regex engine. Trains vocabularies (tiktoken is inference-only). Three model policies (p50k, cl100k, o200k) cover all OpenAI encodings.

**Why size matters more than speed.** Tokenization is 0.1% of inference time — the GPU matmul dominates by 1000x. But memory compounds: each loaded encoding consumes working set, and inference servers load multiple encodings, run hundreds of concurrent requests, and compete with the KV-cache for memory.

Extensible via `tokenizer<Model, Format>` template. Two format loaders (tiktoken, HuggingFace). Zero-divergence split validation on English + multilingual corpora for all three models.

| | tiktoken | ktoken |
|---|---|---|
| Memory (1 thread) | ~18MB | 4.2MB |
| Memory (128 threads) | ~145MB | **4.2MB** |
| Per-thread overhead | ~1MB (regex clone) | 0 (immutable shared state) |
| Single-thread speed | ~6 MB/s | **~16 MB/s** |
| Correctness | Reference implementation | Token-for-token identical (English + multilingual) |
| Regex dependency | fancy_regex or PCRE2 | None |
| Vocabulary training | No (inference only) | Yes (1,323 merges/sec on 28MB, 33K/sec on 208MB) |
| Model policies | cl100k, o200k | p50k, cl100k, o200k (extensible via template) |

## 1 Overview

```
input bytes → pre-split into chunks → BPE encode each chunk → token IDs
```

**Pre-split:** `kntrie<uint32_t, uint8_t>` category trie (287K codepoints, 451KB) classifies each codepoint. A 128-byte ASCII fast-path table resolves 98% of English text at 3ns; non-ASCII falls through to kntrie at 15ns. A model-specific state machine splits at class transitions (contractions, digit groups, whitespace attachment). Splitter throughput: 113 MB/s on English text — effectively free.

**Encode:** Flat `uint32_t[]` merge loop with shrinking window. Init uses a 256KB flat byte-pair table (one array index per pair, no kntrie traversal). Post-merge pair recomputes use `kntrie<uint64_t, uint32_t>` pair trie: 23ns hits, 14ns misses. Miss-fast matters — kntrie's bitmap dispatch resolves misses via a `NOT_FOUND_BIT` tag without an indirect call. Merge pairs recovered at load time via BPE simulation.

**Decode:** Flat `rank → bytes` lookup table. Trivial.

**Train:** Deduplicated chunks with frequency weights, max-heap over pair counts, hot/cold inverted index with frequency-ranked eviction. Strict BPE ordering preserved — cold fallback to full scan for correctness.

### 1.1 Split Patterns

The pre-split regex prevents BPE merges from crossing word boundaries. Each encoding defines its pattern:

**p50k_base (GPT-2/3):** Case-sensitive contractions (`'s 't 'd 'm 'll 've 're` — lowercase only, `DON'T` splits as `DON` `'` `T`). Optional literal space + `\p{L}+` (marks excluded — `\p{M}` is NOT `\p{L}`), optional space + `\p{N}+` (no digit limit), optional space + `[^\s\p{L}\p{N}]+`, `\s+$`, `\s+(?!\S)`, `\s+`.

**cl100k_base (GPT-4):** Case-insensitive contractions, `[^\r\n\p{L}\p{N}]?\p{L}+` (non-letter-non-digit optionally attaches to letter run), 1-3 digit groups, punctuation+newlines, `\s*[\r\n]`, `\s+(?!\S)`, `\s+`.

**o200k_base (GPT-4o):** Case-aware letter splitting (`\p{Lu}`, `\p{Ll}`, `\p{Lo}`), contractions attached to words, 1-3 digits, punctuation+newlines/slashes, whitespace variants. The most complex pattern — requires backtracking when Lo/Lm/M chars appear in runs with Lu/Lt, and treating \p{M} as punctuation when not consumed by letter patterns. All three splitters validated against tiktoken: zero divergences on English and multilingual corpora.

The category trie maps codepoints to 9 character classes: `LOWER`, `UPPER`, `OTHER_LETTER`, `MARK`, `DIGIT`, `WHITESPACE`, `NEWLINE`, `APOSTROPHE`, `OTHER`. Each model's state machine defines its own predicates over these classes (e.g. p50k uses `is_letter` excluding marks; cl100k/o200k use `is_letter_or_mark`). All Unicode complexity is absorbed by the trie.

## 2 C++ API

`ktoken.hpp` is a single header-only library (~940 lines, depends on `kntrie.hpp` and `kstrie.hpp`).

```cpp
#include "ktoken.hpp"

// tokenizer<Model, Format> — defaults to cl100k + tiktoken_format
ktoken::tokenizer<> t("UnicodeData.txt", "cl100k_base.tiktoken");

// Encode / decode
auto ids   = t.encode(data, len);                    // → vector<uint32_t>
auto bytes = t.decode(ids.data(), ids.size());        // → vector<uint8_t>

// Other models
ktoken::tokenizer<ktoken::o200k> t2("UnicodeData.txt", "o200k_base.tiktoken");
ktoken::tokenizer<ktoken::p50k>  t3("UnicodeData.txt", "p50k_base.tiktoken");

// HuggingFace format (include nlohmann_json.hpp first)
ktoken::tokenizer<ktoken::cl100k, ktoken::huggingface_format> t4("UnicodeData.txt", "tokenizer.json");

// Train a new vocabulary from corpus
auto trained = ktoken::tokenizer<>::train("UnicodeData.txt", corpus, len, 100256);
auto ids2 = trained.encode(data, len);
```

**Model policies** — each defines `split(trie, data, len) → vector<chunk_range>`:
- `ktoken::p50k` — GPT-2/GPT-3 split pattern
- `ktoken::cl100k` — GPT-4 split pattern
- `ktoken::o200k` — GPT-4o split pattern

**Format policies** — each defines `load(path) → vocab_data`:
- `ktoken::tiktoken_format` — base64-encoded `.tiktoken` files, BPE simulation merge recovery
- `ktoken::huggingface_format` — `tokenizer.json` with explicit merge pairs and `bytes_to_unicode()` mapping (requires `nlohmann_json.hpp` included before `ktoken.hpp`)

**Key types:**
- `tokenizer<Model, Format>` — the public API. Owns the category trie and vocab. Immutable after construction — safe for concurrent reads.
- `vocab_data` — merge pair trie, byte-pair flat table, byte-to-rank table, decode table. Shared by all encode calls.
- `chunk_range` — `{offset, length}` into the input byte stream.

## 3 Architecture

```
┌───────────────────────────────────────────────────────┐
│              tokenizer<Model, Format>                  │
│  encode(data, len) → vector<uint32_t>                  │
│  decode(tokens, count) → vector<uint8_t>               │
│  train(unicode_db, corpus, len, vocab_size) → tokenizer│
├────────────────────┬──────────────────────────────────┤
│   Model::split()   │         vocab_data               │
│    p50k            │  pair_trie: kntrie<u64, u32>     │
│    cl100k          │  byte_pair_rank[256][256]         │
│    o200k           │  byte_rank[256]                   │
│                    │  decode: rank → bytes             │
├────────────────────┴──────────────────────────────────┤
│                    cat_trie_t                          │
│           kntrie<u32, u8> + ascii_class[128]           │
└───────────────────────────────────────────────────────┘
```

**Encoder data** — constructed once, then immutable:
- `kntrie<uint64_t, uint32_t>` merge pair trie: packed `(left_rank << 32 | right_rank)` → result rank. 100K entries, 1.5MB.
- `uint32_t byte_pair_rank[256][256]`: flat byte-byte pair lookup. 256KB. Init pair_ranks via array index (~1ns) instead of kntrie traversal (~20ns). Only 3,830 of 65,536 entries are valid pairs; the rest are NO_RANK.
- `kntrie<uint32_t, uint8_t>` category trie: codepoint → character class. 287K entries, 451KB. Plus 128-byte ASCII fast-path table.
- `uint32_t byte_rank[256]`: byte → base rank. 1KB.
- `vector<vector<uint8_t>> decode`: rank → byte sequence. ~2MB.

**BPE merge loop:**
- Init fills pair_ranks from the flat `byte_pair_rank` table — zero kntrie calls.
- Min-scan with `[[unlikely]]` on the update branch: BPE rank ordering means the minimum is found early, then the branch stays "not taken" for remaining elements. The hint produces cmov codegen when inlined under register pressure.
- Merge at mi, shift tail left, decrement n. The scan window shrinks every merge — no dead entries, no wasted comparisons.
- Recompute 1-2 neighbor pair_ranks via kntrie. `[[likely]]` on the kntrie call paths (~65-75% taken); boundary cases skip the lookup.
- kntrie `find_loop` checks `NOT_FOUND_BIT` before the indirect call — misses resolve with a bit test instead of dispatching through a function pointer.

**Trainer** — called via `tokenizer::train()`, returns a new tokenizer:
- Same `Model::split()` for chunk boundaries (train-time = inference-time splits)
- Deduplicated chunks with frequency weights
- `kntrie<uint64_t, int64_t>` pair counts with max-heap and lazy deletion
- Hot/cold inverted index: hot set covers `num_merges / 2` most recent merges for O(1) pair count updates; cold fallback to full scan for rare merges

**Threading:** All `tokenizer` state is immutable after construction — 4.2MB shared across any number of threads with zero synchronization, zero per-thread allocation.

## 4 Benchmarks

**Encoding (cl100k_base, single-threaded):**

| Corpus | Size | Tokens | Throughput |
|--------|------|--------|-----------|
| War and Peace (English) | 3.4MB | 780,460 | **16.3 MB/s median, 16.7 peak** |
| Multilingual (CJK/Cyrillic/emoji) | 2.6MB | 1,028,033 | ~14 MB/s |

Token-for-token identical to tiktoken. Zero divergences on both corpora.

**Per-lookup latencies:**

| Lookup | Latency |
|--------|---------|
| Category (ASCII) | 3ns (flat table) |
| Category (non-ASCII) | 15ns (kntrie) |
| Merge pair (hit) | 23ns (kntrie) |
| Merge pair (miss) | 14ns (kntrie, NOT_FOUND_BIT) |
| Byte-pair init | ~1ns (flat array) |
| Byte-to-rank | ~1ns (flat array) |

**Memory:**

| Threads | ktoken | tiktoken |
|---------|--------|----------|
| 1 | 4.2MB | ~18MB |
| 8 | 4.2MB | ~25MB |
| 32 | 4.2MB | ~49MB |
| 128 | **4.2MB** | **~145MB** |

| Component | ktoken | tiktoken |
|-----------|--------|----------|
| Category trie | 451KB + 128B | ~1MB+ per regex clone |
| Merge pair trie | 1.5MB | ~7MB (FxHashMap) |
| Byte-pair flat table | 256KB | N/A |
| Decode table | ~2MB | ~7MB (FxHashMap) |
| **Total (shared)** | **4.2MB** | **~17MB + 1MB/thread** |

**Training (hot/cold, single-threaded):**

| Corpus | Unique chunks | Dedup | Merges | Time | Rate |
|--------|--------------|-------|--------|------|------|
| 28MB | 207K | 26.5x | 100K | 76s | 1,323/sec |
| 208MB | 216K | 191.5x | 200K | 6s | 33,465/sec |

## 5 Build and Distribution

### Directory structure

```
ktrie/
    kntrie/
        kntrie.hpp  kntrie_impl.hpp  kntrie_support.hpp
        kntrie_compact.hpp  kntrie_bitmask.hpp  kntrie_ops.hpp
        kntrie_pyproject.toml  py_kntrie.cpp
    kstrie/
        kstrie.hpp  kstrie_impl.hpp  kstrie_support.hpp
        kstrie_compact.hpp  kstrie_bitmask.hpp
        kstrie_pyproject.toml  py_kstrie.cpp
    ktoken/
        ktoken.hpp  nlohmann_json.hpp
        ktoken_pyproject.toml  py_ktoken.cpp
```

Each directory is flat — headers and source together. ktoken includes
kntrie and kstrie headers via `../kntrie/` and `../kstrie/`.

**C++ build:** C++23, `-O2 -march=x86-64-v4`. All headers are header-only.

**Python bindings** (pybind11, wrapping the C++ template API):
```python
import ktoken

tok = ktoken.tiktoken.Cl100k("cl100k_base.tiktoken")
ids: list[int] = tok.encode("Hello, world!")
text: bytes = tok.decode(ids)
trained = ktoken.tiktoken.Cl100k.train(corpus=b"...", vocab_size=100256)
```

**Wheels:** `manylinux_2_28_x86_64`, `macosx_14_0_arm64`, `win_amd64`.

**Dependencies:** pybind11 (build-time). No regex engine. No protobuf. All C++ is header-only. Unicode character database embedded at build time (category trie builds in 40ms).

## 6 Validation

All correctness targets verified via 6 POC test files:

| Test | What it verifies |
|------|-----------------|
| `poc_encode.cpp` | cl100k encode benchmark, decode round-trip, 10-run stability |
| `poc_o200k.cpp` | o200k case-aware splitter, chunk coverage, encode round-trip |
| `poc_p50k.cpp` | p50k case-sensitive splitter, spot checks, multilingual round-trip |
| `poc_train.cpp` | `tokenizer::train()` API, trained vocab round-trip |
| `poc_roundtrip.cpp` | All paths: loaded cl100k + trained + o200k + p50k, English + multilingual + edge cases |
| `poc_hf_loader.cpp` | HuggingFace `tokenizer.json` format, test strings + corpus round-trip |

All pass. Round-trip verified on English (3.4MB) and multilingual (2.6MB). Zero divergences against tiktoken for all three model policies.

## 7 Pending

1. **Special tokens** — `<|endoftext|>`, `<|fim_prefix|>`, etc. Exact string match before splitting, bypass BPE.
2. **encode_batch / decode_batch** — Thread pool wrapper. Architecture supports it (immutable state), just needs wiring.
3. **Python bindings** — nanobind wrapping of `tokenizer<Model, Format>`. GIL release before C++ calls.
4. **`encode` string overload** — Currently takes `const uint8_t*`, Python will pass str/bytes.
