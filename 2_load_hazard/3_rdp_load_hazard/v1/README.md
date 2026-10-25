# rdp_load_hazard

A diagnostic N64 ROM. It maps out what happens when load state changes right
after a TMEM load, with no sync in between.

## Background

rdp_timing v3 found the effect on NTSC and PAL consoles:

- After `Load_Tile`, a `Set_Texture_Image` sent within about 4 commands
  makes the last 32 bytes of the load come from the **new** address.
- A `Set_Tile` on the load tile, sent soon enough, moves the tail of the
  load to the **new** TMEM address.
- `Load_Block` was not affected.
- Any sync prevented it.

angrylion-rdp-plus and paraLLEl-RDP do not model this.

## What the ROM tests

91 cases, 4 runs each.

| Group | Cases |
|---|---|
| Gap scan | 0–12, 16, 24 and 32 no-ops between Load_Tile and Set_Texture_Image, and the same for Set_Tile (TMEM +512 bytes) |
| Gap filler | Set_Prim_Color or Set_Env_Color instead of no-ops (2–8); a sync instead (controls) |
| Following command | Set_Texture_Image (width or texel size changed); Set_Tile on the load tile (line or format changed); Set_Tile_Size; Set_Tile on another tile; Set_Other_Modes; Set_Color_Image; Set_Prim_Color; Set_Texture_Image + Set_Tile |
| Load size | 8 / 16 / 64 rows; 16 / 64 / 128 bytes per row; 16-bit and 32-bit texels |
| Other loads | Load_Block, with and without dxt, 1 KiB and 4 KiB; Load_TLUT |

Each case runs these steps:

1. TMEM is cleared with zeros, using syncs.
2. The sequence under test runs.
3. After all syncs, the whole 4 KiB of TMEM is read back.

The source texture bytes are `(i*37+11) & 255`, and the "new" address holds
`(i*53+0x5A) & 255`. A byte value therefore identifies its source and its
offset modulo 256.

## Output

Results go to `sd:/rdp_load_hazard/NNN/`:

- `index.txt` describes each case.
- `case_CCC_R.bin` holds the 4096-byte read-back of run R.

`examples/angrylion/` is the same output from angrylion-rdp-plus, which has
no hazards. It is the reference for the analysis tool.

## Analysis

```sh
python3 tools/rdp_hazard_diff.py sd_copy/rdp_load_hazard/001 examples/angrylion
```

For each case, the tool lists the TMEM ranges that differ from the reference
and classifies each differing byte:

| Class | Meaning |
|---|---|
| B@same | The byte the load should have read, but taken from the new address. |
| A+d | A byte of the source, from a different offset (d = offset difference). |
| zero | Not written. |
| other | None of the above. |

It also flags cases whose four runs differ from each other.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.
