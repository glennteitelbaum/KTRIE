# KTRIE

> It's what would happen if a B-Tree had a child with a Trie

## [ABSTRACT](./ktrie_abstract.md)

A Short Abstract about KTRIE

## [KTRIE CONCEPTS](./ktrie_concepts.md)

Concepts that apply to both implementations: KNTRIE (Numeric Key) and KSTRIE (String Key)

## [FULL PAPER](./ktrie.md)

A full paper describing KTRIE as implemented in KNTRIE (Numeric Key) and KSTRIE (String Key)

## Directories

|Directory|What|Concepts|README (includes benchmark results)|
|-------------|----|--------|------|
|**KNTRIE**|Numeric Keyed Imlementation of KTRIE|[KNTRIE Concepts](KNTRIE/kntrie_concepts.md)|[KNTRIE](KNTRIE/README.md)|
|**KSTRIE**|String Keyed Imlementation of KTRIE|[KSTRIE Concepts](KSTRIE/kstrie_concepts.md)|[KSTRIE](KSTRIE/README.md)|
|KTOKEN|BPE tokenizer - KTRIE Usage Example|[KTOKEN Concepts](KTOKEN/ktoken.md)|[KTOKEN](KTOKEN/README.md)|



## Notes:

- Each written in C++ using C++23, all benefit from at least -march=x86-64-v3

- Each supports Python via a pybind11 wrapper for the C++ implementations

- A Java port is available for KNTRIE and KSTRIE

*Note: This was done using AI-assisted development with Claude Opus 4.6*
