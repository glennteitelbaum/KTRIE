### KNTRIE



A KTRIE for Integral keys



> It's what would happen if a B-Tree had a child with a Trie



A **sorted**, **compressed** associative container for integer keys (`uint8\_t` through `uint64\_t`, signed and unsigned). Follows the `std::map` interface. **Header-only**, **C++23**, requires **x86-64-v3** (`popcnt`, `tzcnt`, `lzcnt`) for best performance.



* [Concepts](./concepts.md)



Performance charts:



* [u16 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart16.html)


* [u32 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart32.html)


* [u64 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart64.html)


* [u16 vs u32 vs u64](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chartX.html)




*Note: This was done vibe coding with Claude Opus 4.6*

