# rdp_firstpixel_demo

A small N64 test ROM that makes the RDP's 2-cycle "first pixel of a span"
behaviour visible on screen. It was written to show the angrylion-rdp-plus
parallel-mode bug, where the first pixel of a span used a value from another
worker's scanline, and its fix.

## What it tests

In 2-cycle mode, the first blender cycle blends with the memory colour of
the *previous* pixel the pipeline read. For the first pixel of a span, that
is the last pixel read on the previous span. A span that ends at the
framebuffer's right edge reads one pixel past it, at x = 320, which is
pixel 0 of the next row.

The ROM draws a background of single rows in different shades. Two bands are
blended over it with memory selected in **both** blender cycles. Most games
only blend with memory in cycle 1, which is why the bug is hard to see in
games.

| Band | Columns | Colour carried into the first pixel of row y |
|---|---|---|
| A (red) | 0–319 | pixel (0, y): the row's own first pixel, before the band covers it |
| B (green) | 16–319 | pixel (0, y), which the band does not cover |

Either way, the carried colour equals the background of the same row. On
hardware, the first pixel of every band row should therefore match the rest
of that row.

The first row of a band has no previous row in the same primitive. It
inherits the last pixel read before the band:

- **Band B** follows band A, so it inherits band A's last read, (0, 104).
- **Band A** follows the background, which is drawn in fill mode. Across
  fill mode drawing, real hardware sometimes carries an unrelated constant:
  a few percent of the time on NTSC and PAL, regardless of fill colour,
  fill count or the commands in between (see rdp_carry_probe v2).
  angrylion-rdp-plus does not model this.

To keep band A deterministic, an invisible 2-cycle primitive on row 7 runs
after the background. It writes every pixel back unchanged, and its last
read is (0, 8). Rows 8 and 24 have the same colour, as do rows 104 and 120.

The first version of the ROM did not have this primitive. On real consoles
it showed exactly one wrong row in band A in almost every frame. Band B
showed none.

rdp_carry_probe v1 and v2 confirmed on NTSC and PAL consoles that the
carry-over rules this demo relies on match angrylion-rdp-plus exactly:

- the previous row within a primitive, including spans that do not end at
  the right edge and triangles in both directions;
- the previous primitive across Sync_Full and mode changes.

## What you see

| Screen area | Shows |
|---|---|
| Status lines (A at the top, B at the bottom) | `WRONG ROWS`: rows in this frame whose first pixel differs from the second. `BAD FRAMES`: frames with at least one such row, out of all frames so far. The first frame is not counted. |
| Yellow boxes | The left edge of each band, magnified 4x. In the correct image, the first column continues each stripe. In the wrong image, it stands out as a column in other rows' colours. |

Results in mupen64plus 2.6.0 on a 2-CPU machine, at frame 150:

| Renderer | Band A wrong rows | Band B wrong rows |
|---|---|---|
| angrylion-rdp-plus, `Parallel = False` | 0 | 0 |
| angrylion-rdp-plus 9c8b9ed, parallel, 2 workers | 80 / 80 | 80 / 80 |
| angrylion-rdp-plus 9c8b9ed, parallel, 3 workers (busy loop) | 74 / 80 (varies) | 65 / 80 (varies) |
| angrylion-rdp-plus 9c8b9ed, parallel, 4 workers | 80 / 80 | 80 / 80 |
| angrylion-rdp-plus with the fix, parallel, 2 / 3 / 4 workers | 0 | 0 |

`rdp_firstpixel_demo_before_after.png` shows the original parallel mode on
the left and the fixed version on the right.

With the invisible primitive in place, real hardware shows 0 wrong rows in
both bands:

| Console | Band A bad frames | Band B bad frames |
|---|---|---|
| NTSC | 0 / 1034 | 0 / 1034 |
| PAL | 0 / 449 | 0 / 449 |

So the single-threaded renderer and the fixed parallel renderer match
hardware. The original parallel renderer does not.

## Building

The ROM needs [libdragon](https://github.com/DragonMinded/libdragon) (preview
branch) and its toolchain in `$N64_INST`.

```sh
make            # builds rdp_firstpixel_demo.z64
```

The RDP is driven directly through the DP registers; there is no RSP
microcode. The ROM therefore also runs with a high-level RSP plugin, and
every RDP command is exactly as written in `main.c`.
