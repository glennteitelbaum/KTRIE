# KTRIE

> It's what would happen if a B-Tree had a child with a Trie

## [KTRIE CONCEPTS](./ktrie_concepts.md)

Concepts that apply to both implementations

## Directories

|Directory|What|Concepts|README (includes benchmark results)|
|-------------|----|--------|------|
|KNTRIE|Numeric Keyed KTRIE|[KNTRIE Concepts](KNTRIE/kntrie_concepts.md)|[KNTRIE](KNTRIE/README.md)|
|KSTRIE|String Keyed KTRIE|[KSTRIE Concepts](KSTRIE/kstrie_concepts.md)|[KSTRIE](KSTRIE/README.md)|
|KTOKEN|BPE tokenizer - KTRIE Usage Example|[KTOKEN Concepts](KTOKEN/ktoken.md)|[KTOKEN](KTOKEN/README.md)|

## [ABSTRACT](./ktrie_abstract.md)

## Notes:

- Each written in C++ using C++23, all benefit from at least -march=x86-64-v3

- Each supports Python via a pybind11 wrapper for the C++ implementations

- Be aware that: KNTRIE/KSTRIE Use snapshot iterators, so a modify method is included

- A Java port is available for KNTRIE and KSTRIE
  - The Java ports use live iterators, not snapshots
  

*Note: This was done using AI-assisted development with Claude Opus 4.6*
