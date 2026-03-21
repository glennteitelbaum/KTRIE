### KNTRIE



A KTRIE for Integral keys



> It's what would happen if a B-Tree had a child with a Trie



A **sorted**, **compressed** associative container for integer keys (`uint8\_t` through `uint64\_t`, signed and unsigned). Follows the `std::map` interface. **Header-only**


* [KTRIE Concepts](../ktrie_concepts.md)

* [KNTRIE Concepts](./kntrie_concepts.md)



Performance charts:



* [u16 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart16.html)


* [u32 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart32.html)


* [u64 vs map/umap](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chart64.html)


* [u16 vs u32 vs u64](https://glennteitelbaum.github.io/KTRIE/KNTRIE/chartX.html)


## Notes:

- Written in C++ using C++23, benefits from at least -march=x86-64-v3 (`popcnt`, `tzcnt`, `lzcnt`)

- Supports Python via a pybind11 wrapper for the C++ implementations

- Be aware that: KNTRIE uses a snapshot iterator, so a modify method is included

- A Java port is available
  - The Java ports use live iterators, not snapshots
  


*Note: This was done using AI-assisted development with Claude Opus 4.6*

