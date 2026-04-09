## KNTRIE Bitmask Nodes

Same four u32 keys with two levels of bitmask dispatch. Byte 0 splits {00, FE}. Byte 1 splits the 00-subtree into {00, 04}. Suffix type narrows from u32 → u16 at depth 2.

```mermaid
block
  columns 2

  space
  ROOT(("ROOT"))
  
  NF{"NOT FOUND"}
  space

  space
  block:BM0
    columns 5
    space
    block:bm0_title("bitmask — byte 0 • bitmap: {00, FE}"):3
      space
    end
    space
    bm0_bmp["bitmap: 00 FE"] bm0_sent["SENTINEL"] bm0_c0["→ 00"] bm0_c1["→ FE"] bm0_desc["desc: 4"]
  end

  space:2

    block:BM1
      columns 5
      space
      block:bm1_title("bitmask via 00 — byte 1 • bitmap: {00, 04}"):3
        space
      end
      space
      bm1_bmp["bitmap: 00 04"] bm1_sent["SENTINEL"] bm1_c0["→ 00"] bm1_c1["→ 04"] bm1_desc["desc: 3"]
    end

    block:CK_FE
      columns 4
      space
      block:ckfe_title("compact via FE — 1 entry, depth 1"):2
        space
      end
      space
      ckfe_k0["0xEDFACE"] space ckfe_V0[["HAMBURGER"]] space 
    end

    space:2

    block:CK_00
      columns 4
      space
      block:ck00_title("compact via 00→00 — 2 entries, suffix: u16"):2
        space
      end
      space
      ck00_k0["0x0000"] ck00_k1["0x0004"] ck00_V0[["NULL"]] ck00_V1[["FOUR"]]
    end

    block:CK_04
      columns 4
      space
      block:ck04_title("compact via 00→04 — 1 entry, suffix: u16"):2
        space
      end
      space
      ck04_k0["0x01BC"] space ck04_V0[["POTITUS"]] space
    end

  ROOT --> BM0
  bm0_c0 --> BM1
  bm0_c1 --> CK_FE
  bm1_c0 --> CK_00
  bm1_c1 --> CK_04
  bm0_sent-- "X" -->NF
  bm1_sent-- "X" -->NF

  style ROOT fill:#555,color:#fff,stroke:#333

  style bm0_bmp fill:#e67e22,color:#fff,stroke:#d35400
  style bm0_sent fill:#888,color:#fff,stroke:#666
  style bm0_c0 fill:#3498db,color:#fff,stroke:#2980b9
  style bm0_c1 fill:#e74c3c,color:#fff,stroke:#c0392b
  style bm0_desc fill:#888,color:#fff,stroke:#666

  style bm1_bmp fill:#e67e22,color:#fff,stroke:#d35400
  style bm1_sent fill:#888,color:#fff,stroke:#666
  style bm1_c0 fill:#3498db,color:#fff,stroke:#2980b9
  style bm1_c1 fill:#9b59b6,color:#fff,stroke:#8e44ad
  style bm1_desc fill:#888,color:#fff,stroke:#666

  style ck00_k0 fill:#3498db,color:#fff,stroke:#2980b9
  style ck00_k1 fill:#3498db,color:#fff,stroke:#2980b9
  style ck00_V0 fill:#2ecc71,color:#fff,stroke:#27ae60
  style ck00_V1 fill:#2ecc71,color:#fff,stroke:#27ae60

  style ck04_k0 fill:#9b59b6,color:#fff,stroke:#8e44ad
  style ck04_V0 fill:#2ecc71,color:#fff,stroke:#27ae60

  style ckfe_k0 fill:#e74c3c,color:#fff,stroke:#c0392b
  style ckfe_V0 fill:#2ecc71,color:#fff,stroke:#27ae60
```

**Level 0**: root bitmask dispatches byte 0. Bitmap {00, FE} — 2 children out of 256 possible.
**Level 1**: 00-subtree bitmask dispatches byte 1. Bitmap {00, 04} — 2 children. FE-subtree is a single-entry compact node (no further branching needed).
**Level 2**: compact leaves with suffix narrowed to u16 (2 bytes consumed, 2 remaining). Sorted within each leaf.

Suffix narrowing: the root stores u32 suffixes, but after consuming 2 bytes of dispatch, the leaves store u16 — halving per-entry key storage. The same four entries that occupied 9 × 256 slots in the digital trie (Figure 2) now occupy 2 bitmask nodes + 3 compact nodes, each holding only the entries that belong there.
