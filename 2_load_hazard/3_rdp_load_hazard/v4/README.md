# rdp_load_hazard v4

A diagnostic N64 ROM: how long an idle RDP keeps the load queue, and the
alignment rule of the Set_Texture_Image hazard.

## The model so far (v2 and v3, NTSC = PAL)

- The last 15 slots of a load stay queued. One slot is 8 source bytes; a
  Load_Tile row takes max(words + 1, 9) slots.
- Every state command retires one slot, using the tile state of that moment.
- Syncs, draws (rectangles, texture rectangles, triangles) and loads write
  the whole queue.
- With the RDP idle, the queue is written after 10 to 100 µs.
- How far Set_Texture_Image (STI) reaches depends on the RDRAM alignment of
  the rows.

## Parts

| Part | Question | Cases |
|---|---|---|
| E | What do the DP status bits and counters show while the queue is pending? | Status, CLOCK, BUFBUSY, PIPEBUSY and TMEM traced for 400 µs after a load, with the list ending right after the load. Loads: Load_Block 128 B, Load_Tile 16 × 128 B, 16 × 32 B, and Load_Block followed by Sync_Load as a control |
| D | How long must the RDP stay idle before the queue is written? | Load_Block 128 B: waits 10–100 µs in 2 µs steps, 3 runs each. Load_Tile 16 × 128 B: 20–140 µs in 4 µs steps, 2 runs each |
| F | What is the STI alignment rule? | STI reach (gaps 0–14) for every row width 8–64 bytes and source offset 0–56. F1: stride = width. F2: stride 128, so every row has the same alignment. F3: 2 rows with independent alignment, for 24- and 40-byte rows |

Every case is compared on the console with the same list that has a sync
before the change.

## Output

- **Screen:** D shows the last wait with an effect and the first wait
  without. F shows STI reach as grids (rows: width or p; columns: offset or
  q).
- **SD card:** `sd:/rdp_load_hazard/v4_NNN.txt`.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.

Made by Nekokabu.
