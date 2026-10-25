# rdp_carry_probe

A diagnostic N64 ROM. It finds out which memory colour the first pixel of a
2-cycle primitive blends with, and how that depends on the commands drawn
before the primitive.

## Background

In 2-cycle mode, the first blender cycle uses the memory colour of the
*previous* pixel the pipeline read:

- **Inside a primitive**, that is the last pixel read on the previous
  scanline.
- **For the first scanline of a primitive**, it is whatever the RDP read
  last before the primitive started.

rdp_firstpixel_demo showed that real hardware and angrylion-rdp-plus
disagree in the second case.

## Method

The CPU fills a 320x240 32-bit framebuffer with **address codes**: every
pixel holds its own coordinates.

| Channel | Contents |
|---|---|
| R | x, low 8 bits |
| G | y |
| B | `0x80` with x bit 8 in the lowest bit |

The probe is a 2-cycle rectangle, x 64..127 and rows 100..103. Its blender
passes the first cycle's memory input straight through to the output:

```
cycle 0: P = memory, A = 0, M = memory, B = 1   -> memory colour
cycle 1: P = cycle 0, A = 0, M = cycle 0, B = 1  -> cycle 0
```

Each probe pixel therefore shows the code of the pixel read before it:

- The first pixel of row 100 shows the value carried in from before the
  probe.
- The first pixel of each later row shows the last pixel read on the row
  above it.

Every test first draws an **anchor**: a one-row 2-cycle rectangle at
x 200..239, y 50, so the last pixel it reads is (240, 50). The test then
runs the commands under test, then the probe. Each test runs 16 times, with
slightly different start times.

| Test | Between anchor and probe |
|---|---|
| direct | nothing |
| syncfull | Sync_Full; the probe is in a new command list |
| fill1 | a fill mode rectangle, 320x1 at y 150 |
| fillsmall | a fill mode rectangle, 8x1 at (8, 150) |
| fill240 | fill mode rows over the whole screen except near the probe (the demo's situation) |
| 1cyc_read | a 1-cycle rectangle with image read, x 20..59, y 60 |
| 1cyc_prim | a 1-cycle PRIM rectangle without image read, y 62 |
| 2cyc_nord | a 2-cycle rectangle without image read, y 64 |
| syncpipe | Sync_Pipe |
| som_fill | Set_Other_Modes to fill mode, then back to the probe mode |
| colorimage | Set_Color_Image (same buffer) |
| anchorwrap | nothing, but the anchor ends at the right edge (x 200..319), so its last read wraps to (0, 51) |
| probewrap | nothing; the probe spans x 0..319 |
| tri_lft1 | nothing; the probe is drawn as a triangle, left major |
| tri_lft0 | nothing; the probe is drawn as a triangle, right major (drawn right to left) |

## Output

The screen shows one line per test:

| Column | Shows |
|---|---|
| ROW0 | The first pixel of row 100, which is the value carried in. `x16` etc. means the value varied between runs. |
| ROW1 | The first pixel of row 101. A trailing `*` means it varied between runs. |

`sd:/rdp_carry/rdp_carry_NNN.txt` holds every run and every probe row: the
decoded first and second pixel and their raw values.

**What angrylion-rdp-plus shows**, single-threaded or with the parallel fix:

| Test | ROW0 | ROW1 |
|---|---|---|
| direct, syncfull, fill1, fillsmall, fill240, 1cyc_prim, 2cyc_nord, syncpipe, som_fill, colorimage, tri_lft1 | (240,50) | (128,100) |
| 1cyc_read | (60,60) | (128,100) |
| anchorwrap | (0,51) | (128,100) |
| probewrap | (240,50) | (0,101) |
| tri_lft0 | (128,100) | (128,101) |

For tri_lft0, the first pixel the pipeline processes is x = 128, which has
no coverage and is not written. The pixel shown at x = 127 is therefore
already the second pixel of the row.

## Building

The ROM needs [libdragon](https://github.com/DragonMinded/libdragon) (preview
branch) and its toolchain in `$N64_INST`.

```sh
make            # builds rdp_carry_probe.z64
```

The RDP is driven directly through the DP registers; there is no RSP
microcode.
