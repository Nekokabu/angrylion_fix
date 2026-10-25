# rdp_carry_probe v7

A diagnostic N64 ROM: the odd memory colour after fill mode, part 3.

## Known so far (v1–v6, NTSC and PAL)

After fill or copy mode, in about 3% of runs, the first pixel of the next
2-cycle primitive (and only that pixel) gets a different memory value:

- **16-bit framebuffer:** the value read 12 pixels before the last pixel
  the list reads, counting back across rows.
- **32-bit framebuffer:** the constant `000C00E0`.

With two 2-cycle primitives after the fill, NTSC put the second one's value
into the first one's first pixel (27/512 runs). PAL never did (0/512).

## Tests

| Group | Test | Question |
|---|---|---|
| A | tail_noread2, tail_ir1, tail_probe2, tail_fill | A primitive drawn **after** the probe: 2-cycle without image read, 1-cycle with image read, a second 2-cycle probe, a fill rectangle. Does the source move to the end of the list? |
| B | mid_probe2, mid_row10, mid_w16 (1024 runs each) | The NTSC/PAL difference: v6's mid_probe2 again, then with the middle primitive above the probe, then narrower |
| C | w1–w7, w2_rows8, w4_rows8 | Narrow probes: how many read slots does a short row take? |

Every run's probe area, and the rows of the extra primitives, are compared
with the normal result.

## Output

- **Screen:** one line per test: odd runs, and the most common source pixel.
- **SD card:** `sd:/rdp_carry/rdp_carry_v7_NNN.txt`, in the same format as v6.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.

Made by Nekokabu.
