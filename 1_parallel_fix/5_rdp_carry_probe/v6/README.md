# rdp_carry_probe v6

A diagnostic N64 ROM: the odd memory colour after fill mode, in more depth.

## Known so far (v1–v5, NTSC and PAL)

After fill mode drawing, in 1–6% of runs the next 2-cycle primitive does not
start with the carried memory colour:

- 16-bit framebuffer: it starts with the pixel at (right edge − 13, last row
  of the same primitive).
- 32-bit framebuffer: it starts with the constant `000C00E0`.
- Fill colour, fill count, the commands in between (even Sync_Full) and the
  VI make no difference.

## Tests (512 runs each)

| Test | Question |
|---|---|
| base | Reference: probe x 64..127, rows 20..23 |
| w4, w8, w12, w16, w24 | Probe width 4–24 pixels: what happens when the row is shorter than 13 pixels? |
| h1 | 1-row probe (the first row is also the last) |
| tri | Probe drawn as a triangle |
| cyc1 | 1-cycle probe with image read |
| mid_ir1, mid_noread2, mid_probe2 | A primitive between the fill and the probe: 1-cycle with image read, 2-cycle without image read, a second 2-cycle probe. Which one gets the odd value? |
| copy | Copy mode texture rectangles instead of fill mode |
| split | Fill and probe in separate lists, 1 ms apart |
| nofill | Control: no fill |
| fb32_base, fb32_w8 | 32-bit framebuffer |

Every run's probe area (and the middle primitive's row) is compared with the
normal result, the majority of the first three runs.

## Output

- **Screen:** one line per test: odd runs, and the most common source pixel
  of the first changed pixel.
- **SD card:** `sd:/rdp_carry/rdp_carry_v6_NNN.txt`. For every odd run:
  the VI line at submission, the number of changed pixels, and up to 8 of
  them as `x,y=value(source pixel)`.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.

Made by Nekokabu.
