# BPE tokenizer

uses KTRIE (KNTRIE/KSTRIE) for BPE tokenizing

[CONCEPTS](ktoken.md)

## Notes:

- Written in C++ using C++23, benefits from at least -march=x86-64-v3 (`popcnt`, `tzcnt`, `lzcnt`)

- Supports Python via a pybind11 wrapper for the C++ implementations

*Note: This was done using AI-assisted development with Claude Opus 4.6*
