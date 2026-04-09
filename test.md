## KNTRIE Compact Node

Four u32 keys stored in a single compact node. Root skip = 0 (first byte diverges: 0x00 vs 0xFE). Suffix type is u32 (full key width, depth 0). Sorted array of suffix/value pairs.

```mermaid
block
  columns 1

  ROOT(("ROOT"))

  block:NODE
    columns 8
      space
      block:k_lbl["sorted keys (u32)"]:2
        space
      end
      space
      space
      block:v_lbl["values"]:2
        space
      end
      space
      k0["0x00000000"] k1["0x00000004"] k2["0x000401BC"] k3["0xFEEDFACE"]
      V0[["NULL"]] V1[["FOUR"]] V2[["POTITUS"]] V3[["HAMBURGER"]]
    end

  ROOT --> NODE

  style ROOT fill:#555,color:#fff,stroke:#333

  style k0 fill:#3498db,color:#fff,stroke:#2980b9
  style k1 fill:#9b59b6,color:#fff,stroke:#8e44ad
  style k2 fill:#e74c3c,color:#fff,stroke:#c0392b
  style k3 fill:#e67e22,color:#fff,stroke:#d35400

  style V0 fill:#2ecc71,color:#fff,stroke:#27ae60
  style V1 fill:#2ecc71,color:#fff,stroke:#27ae60
  style V2 fill:#2ecc71,color:#fff,stroke:#27ae60
  style V3 fill:#2ecc71,color:#fff,stroke:#27ae60
```

**Blue/purple/red/orange**: sorted u32 suffix keys — one color per entry.
**Green**: values.

No skip prefix (byte 0 diverges: 0x00 vs 0xFE). No branch nodes needed — 4 entries is well below COMPACT_MAX (4096). Lookup is a branchless binary search over the 4-element u32 array: `bit_width(3) = 2`, count2 = 2, diff = 2, one diff comparison + one loop iteration.
