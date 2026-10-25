# rdp_load_hazard v5

A diagnostic N64 ROM: the Set_Texture_Image hazard rule for rows wider than
64 bytes, and the exact point at which an idle RDP writes its load queue.

## Findings so far (v2 to v4, NTSC = PAL)

- The last 15 slots of a load stay queued. One slot is 8 source bytes; a
  Load_Tile row takes max(words + 1, 9) slots.
- Every state command retires one slot, using the tile state of that moment.
  Syncs, draws and loads write the whole queue.
- When the RDP has no more commands, it writes the queue about 30 µs after
  the load ends.
- Set_Texture_Image (STI): the source address of row r is fixed at a slot
  that depends on the alignment of row r − 1. For rows up to 64 bytes the
  rule matches all 256 v4 scans; wider rows need another formula.

## Parts

| Part | Question | Cases |
|---|---|---|
| D | Where exactly is the idle edge, and is it sharp? | Waits in 0.5 µs steps, 5 runs each: Load_Block 128 B (25–38 µs), Load_Tile 16 × 32 B (30–48 µs), Load_Tile 16 × 128 B (40–56 µs) |
| F | STI rule for wide rows | Row widths 72–128 bytes × source offsets 0–56. F1: stride = width. F2: stride 128 (every row aligned the same). F3: 2 rows with independent alignment, 80- and 104-byte rows |

## Output

- **Screen:** D shows the last wait with an effect and the first without. F
  shows STI reach as grids.
- **SD card:** `sd:/rdp_load_hazard/v5_NNN.txt`.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.

Made by Nekokabu.
