# rdp_load_hazard v3

A diagnostic N64 ROM: the open questions of the RDP load hazard model.

## The model so far (v2, NTSC = PAL, RDRAM = DMEM)

- The last 15 slots of a load stay queued after the load command.
- Every later command retires one slot, using the tile state of that moment.
- A sync flushes the queue.
- Load_Block: one 8-byte write per slot. Load_TLUT: one entry per slot.
- Load_Tile: each row takes max(words + 1, 9) slots. The first slots of a row
  write nothing.
- Set_Texture_Image affects a row when it comes at least 4 slots before the
  row's first TMEM write. 48- and 96-byte rows do not fit this rule (they
  reach 4 commands further), and 24-byte rows show 2 rows at gap 0.

## Parts

| Part | Question | Cases |
|---|---|---|
| A | Why do some row sizes break the STI rule? | 40 Load_Tile geometries, each scanned with STI and with ST7 (gap 0–20): row widths 8–128 bytes (with 40, 56, 80, 112), source offsets 8/16/32, image stride wider than the row, 2/4/8 rows, RGBA16 and RGBA32 |
| B | What counts as one command? | 1, 2, 4 and 8 commands of one type between the load and ST7, for 30 types: state commands, Set_Tile/Set_Tile_Size on other tiles, syncs, rectangles, texture rectangles (2 words), a triangle (4 words), a second load. Loads: Load_Block 128 B and Load_Tile 16 × 128 B |
| C | Does the queue drain while the RDP is idle? | The list is submitted in two parts, split after the load (and 0 or 4 no-ops). Waits between the parts: 0, 1, 10, 100, 1000, 10000 µs |

Every case is compared on the console with the same list that has a sync
before the change. All lists run from RDRAM.

## Output

- **Screen:** part A shows the reach for STI (T) and ST7 (7) per geometry.
  Parts B and C show moved words (B) or differing words (C).
- **SD card:** `sd:/rdp_load_hazard/v3_NNN.txt`. Every line has the differing
  bytes and 8-byte words, the first and last differing TMEM address, and the
  number of non-zero bytes in the reference (a check that the read-back
  works).

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.

Made by Nekokabu.
