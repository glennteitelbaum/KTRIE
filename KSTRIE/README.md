### KNTRIE



A KTRIE for String keys



> It's what would happen if a B-Tree had a child with a Trie



A **sorted**, **compressed** associative container for string keys. Follows the `std::map` interface. **Header-only**, **C++23**, requires **x86-64-v3** (`popcnt`, `tzcnt`, `lzcnt`) for best performance.


* [KTRIE Concepts](../ktrie_concepts.md)


* [KSTRIE Concepts](./kstrie_concepts.md)



Performance charts:

* [400K+ words vs std::unordered_map](https://glennteitelbaum.github.io/KTRIE/KSTRIE/bench_words.html) [Ref: [400K+ words](https://glennteitelbaum.github.io/KTRIE/KSTRIE/words.txt) ]

* [100K elements vs std::map and std::unordered_map](https://glennteitelbaum.github.io/KTRIE/KSTRIE/bench_kstrie.html)

*Note: This was done using AI-assisted development with Claude Opus 4.6*
