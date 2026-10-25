# rdp_carry_probe v5

A diagnostic N64 ROM: which pixel does the odd read after fill mode come
from, and how does that depend on the primitive being drawn?

## v4 results (NTSC and PAL, 4 MB and 8 MB, two runs each)

**16-bit framebuffer.** The low and high address codes combine to one
address: framebuffer + 0x10266 bytes. That is pixel (115, 103), and it
stayed there when the framebuffer moved by 0x12340.

- The read is relative to the framebuffer, at the same place every time.
- That pixel lies in the **last row** of the probe rectangle (x 64..127,
  rows 100..103), 51 pixels from its left edge.
- It also explains v2's `99C7`: with v2's code layout, that value is
  x = 115 and y ≡ 7 (mod 32), and 103 mod 32 = 7.

**32-bit framebuffer.** The odd value was always `000C00E0`, on 4 MB and
8 MB consoles alike, at both framebuffer positions. It never appears in
RDRAM, so it does not come from memory.

## Method

The 16-bit tests vary the probe rectangle and see where the odd pixel moves.
Each geometry runs twice: once with the low address code (address bits
1..15) and once with the high one (bits 8..22).

| Test | Probe x | Probe rows |
|---|---|---|
| base | 64..127 | 100..103 |
| x32 | 32..95 | 100..103 |
| wide | 64..191 | 100..103 |
| rows2 | 64..127 | 100..101 |
| rows8 | 64..127 | 100..107 |
| y150 | 64..127 | 150..153 |

Three 32-bit tests (fb32_base, fb32_x32, fb32_wide) check whether
`000C00E0` changes with the geometry.

Each test runs 256 times.

## Output

- **Screen:** one line per test: odd count, the most common odd value, and
  its decoded address bits.
- **SD card:** `sd:/rdp_carry/rdp_carry_v5_NNN.txt` holds every run.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.
