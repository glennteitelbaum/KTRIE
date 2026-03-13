# KTRIE: A Trie/B-Tree Hybrid for Ordered Associative Containers

## Abstract

We present the KTRIE, a hybrid of a trie and a B-tree. Each key is decomposed into three logical regions: a shared prefix absorbed into a single node, one or more branch levels handled in a trie-like fashion, fanning out only where the data requires, and a suffix region collected into a B-tree-like flat sorted compact leaf.

The KTRIE offers O(K) lookup, insertion, and deletion, where K is the key length in bytes. Its distinguishing contribution is suffix compression: the KTRIE collects remaining suffixes into flat sorted leaves. Whereas a traditional trie scatters sparse keys across many small deep nodes, suffix compression collapses them into contiguous arrays, eliminating per-node overhead, improving locality, and enabling O(1) amortized per-element ordered iteration, with effective depth often well below K for non-adversarial data distributions.

We evaluate two concrete instantiations: KNTRIE and KSTRIE. Both employ bitmap-compressed 256-way branch nodes with branchless dispatch, compact sorted leaves, and share a value storage layer specialized by type: packed bits for Booleans as per `std::vector<bool>`, inline embedding for small trivial types (≤ 8 bytes) as per `std::string` with Small String Optimization, and heap-indirected owned copies for larger or non-trivial types.

KNTRIE is specialized for fixed-width integer keys (16-, 32-, and 64-bit) which allows a bounded depth and supports narrowing key representation by depth in leaves whose size is governed by entry count.

KSTRIE is specialized for variable-length string keys, addressing the challenges of unbounded key length and unpredictable depth. KSTRIE extends prefix compression into the suffix leaves themselves via keysuffix sharing with leaf capacity governed by a compressed suffix byte budget.

Benchmarks demonstrate that across all operations, both KNTRIE and KSTRIE are competitive with `std::unordered_map` while providing full ordering, and consistently outperform `std::map`. By exploiting prefix compression and suffix truncation, KNTRIE and KSTRIE typically occupy a third or less of the space of `std::map` and `std::unordered_map`. Some benchmark distributions show KNTRIE occupying less space than the raw key-value data alone. 

The primary contribution is the prefix/branch/suffix decomposition as a unified hybrid design, with the novel integration of keysuffix sharing.

The design draws on established techniques: the trie structure (Fredkin, 1960; Knuth, 1973, 1998); prefix compression from the Patricia trie (Morrison, 1968); the B-tree (Bayer and McCreight, 1972); bitmap-indexed child compression from Judy arrays (Baskins, 2004) and the Adaptive Radix Tree (Leis et al., 2013); the branchless binary search (Khuong and Morin, 2017); boolean bit-packing (Stepanov and Lee, 1994; Austern, 1998).

**Keywords:** trie, B-tree, ordered associative container, prefix/suffix decomposition, keysuffix sharing, cache locality, bitmap indexing, branchless search, variable-length keys