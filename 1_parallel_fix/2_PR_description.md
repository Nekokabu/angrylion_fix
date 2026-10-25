# Fix parallel mode memory colour carry-over between scanlines

## Problem

With `Parallel = True`, the first pixel of some scanlines renders differently
from `Parallel = False` and from real hardware. It happens in 2-cycle mode when
the blender reads the framebuffer (`IMAGE_READ`, as with blending or AA). The
result also changes from run to run.

On a test ROM I ran on real NTSC and PAL consoles (their dumps are
byte-identical), the single-threaded renderer matches hardware in every
1-cycle and 2-cycle case. Parallel mode differs in 9–12 cases, depending on
the run. In each one, the differences are only at the first pixel of a
scanline.

## Cause

In every `render_spans_2cycle_*` function, the order per pixel is:

```c
wstate->fbread2_ptr(wstate, curpixel, &curpixel_memcvg);   // -> pre_memory_color
...
wen = blender_2cycle_cycle0(wstate, ...);                  // uses memory_color
...
wstate->memory_color = wstate->pre_memory_color;
```

- The first blender cycle therefore sees the memory colour of the
  *previous* pixel the pipeline processed. For the first pixel of a span,
  that is the last pixel read on the previous span.
- The last pixel read is at x = `lx`. A rectangle ending at the
  framebuffer's right edge has a final, zero-coverage iteration at x = width,
  which addresses (0, y+1).
- Without image read, `fbread` keeps the colour channels, so they also
  carry over from earlier primitives.

Hardware does the same thing; the single-threaded renderer matches it, even
for a rectangle starting at x = 20 over a patterned framebuffer.

In parallel mode, each worker renders only scanlines `j % stride == offset`
and has its own `memory_color` / `pre_memory_color`. A span's carried value
therefore came from scanline y − stride and from that worker's own history.
The pixel it needed often lies in another worker's row, and that row may or
may not have been written yet.

## Who is affected

The carried value is used only when the **first** blender cycle selects
memory: memory colour as an input, or memory alpha as the B input.

- The usual 2-cycle setups put fog or pass-through in cycle 0 and the memory
  blend in cycle 1. That covers libultra's `G_RM_FOG_SHADE_A` / `G_RM_PASS`
  plus a `*2` mode, and libdragon's rdpq two-pass blender. These setups are
  not affected.
- Render modes that blend with memory in both cycles are affected, for
  example `G_RM_XLU_SURF, G_RM_XLU_SURF2`.
- Where it does apply, the wrong value depends on thread timing, so the
  output is not deterministic. That matters for things like TAS and netplay.

## Fix

- **A shared view of the carried registers.** Every worker tracks
  `memory_color` and `pre_memory_color` as the single-threaded renderer has
  them between primitives, and loads them before each 1-cycle or 2-cycle
  primitive. Without image read, this needs no framebuffer access.
- **Framebuffer values through a mailbox.** With image read, the worker that
  owns the needed pixel's row reads it at the start of the primitive and
  posts it to a mailbox. Only that worker writes that row. The worker that
  needs the value waits for it.
  - There is one post per scanline per primitive.
  - Waiting happens only when the owner has not started the primitive yet.
- **Mailbox slots.** Slots are indexed by scanline and by primitive number
  modulo `CMD_BUFFER_SIZE`. A batch never holds more primitives than that,
  and every worker resolves pending values before the batch ends, so a slot
  is never reused while still needed. An `#error` guards the size relation.
- **Single-threaded mode is unaffected.** All of it is skipped when
  `stride <= 1`, so the single-threaded path is unchanged.

Files changed:

| File | Change |
|---|---|
| `parallel.cpp` / `parallel.h` | The mailbox (64-bit atomics, release/acquire). |
| `rdp.c` | `span.validline_all` (validity before the per-worker filter) and the per-worker view state. |
| `rasterizer.c` | The hand-over logic and the hooks in the edgewalker and the four 2-cycle span renderers. |
| `n64video.c` | Resolving pending values at the end of each batch, and initialising the view. |

## Testing

Test setup:
- **Test ROM:** `rdp_timing` v3 (libdragon, drives the RDP directly). 462
  cases: fill, copy, 1-cycle and 2-cycle rectangles and triangles, textures,
  Z, blending, AA. There are framebuffer dumps of 88 of them, plus a CRC
  of every case after 10 runs.
- **Emulator:** mupen64plus 2.6.0, on a machine with 2 CPUs.

Results:

| Configuration | Differs from `Parallel = False` | Differs from hardware |
|---|---|---|
| before, 2 / 3 (busy) / 4 workers | 9–12 cases (first pixels) | the same cases, plus the 6 below |
| after, 2 workers (×2), 3 busy (×2), 4, 5 busy, 8 | **0** (all 88 dumps byte-identical) | only the 6 below |
| after, full ROM, CRCs of all 462 cases, 3 busy / 4 workers | **0** | only the 6 below |

A small visual test ROM, `rdp_firstpixel_demo`, is attached. It blends two
bands with memory in both cycles over single-row stripes, magnifies their
left edges, and counts wrong rows on screen.

- Before the fix, with 2 or 4 workers, every row is wrong in both bands.
  With 3 workers the count varies from frame to frame.
- After the fix, and with `Parallel = False`, no row is wrong.
- Real NTSC and PAL consoles also show no wrong row: 0 bad frames out of
  1034 (NTSC) and 449 (PAL).

See `rdp_firstpixel_demo_before_after.png` for the two screens side by side.

The 6 remaining hardware differences are load-hazard cases, which no
configuration reproduces. They come from `Set_Texture_Image` or `Set_Tile`
right after `Load_Tile` with no sync, and are unrelated to this change.

The patch adds no new compiler warnings with `-Wall -Wextra`.

## Related hardware findings (not part of this change)

The single-threaded carry-over rules this fix reproduces were checked on
NTSC and PAL consoles with a probe ROM. Its blender outputs the first
cycle's memory input, so each pixel shows the address of the pixel read
before it. In every case the consoles matched angrylion exactly:

- the previous row within a primitive, including spans that do not end at
  the right edge and triangles drawn in both directions;
- the previous primitive across `Sync_Full`, mode changes, and 1-cycle or
  2-cycle primitives without image read;
- a 1-cycle primitive with image read, which replaces the value.

The one thing angrylion does not model is what happens after fill mode
drawing. In 1–6% of runs, the next 2-cycle primitive does not start with
the value from before the fill. Probe ROM versions 2–7 narrowed this down.

**What happens.** Only one pixel is affected: the first pixel of the first
2-cycle primitive after fill or copy mode drawing. Its value is not decided
when the pixel is drawn. It is filled in when the **last 1-cycle or 2-cycle
primitive of the command list** finishes, with the colour that was in the
pipeline about 12 pixels before that primitive's last pixel. Where the pixel
is written does not change.

**16-bit framebuffer.** With the probe's blender passing the memory colour
through, that colour is the framebuffer pixel read 12 pixels before the
last pixel, counting back across rows. For a probe covering x 64..127 and
rows 20..23, that is pixel (115, 23).
- A 2-cycle primitive drawn after the probe moves the source into that
  primitive (12 pixels before its end). A 1-cycle primitive with image read
  moves it to 11 pixels before its end. A 2-cycle primitive without image
  read puts its own output colour there. A fill rectangle at the end does
  not count.
- With a second 2-cycle primitive between the fill and the probe, the
  value from the end of the list lands in that second primitive's first
  pixel.
- For rows of 8 pixels or more, each row counts as its width. Rows of 4–7
  pixels always give (x0 + 3, last row − 2). With 1–3 pixels the count
  reaches back before the primitive, and the pixel gets the combiner colour
  instead.

**32-bit framebuffer.** The value is the constant `000C00E0`. It appears
nowhere in RDRAM and does not change with geometry, framebuffer position
or an 8 MB console.

**What doesn't affect it.** Fill colour, fill count, the commands between
the fill and the primitive (even `Sync_Full`), splitting the work into two
command lists 1 ms apart, a triangle instead of a rectangle, and whether
the VI is running make no difference. It never happens without fill or
copy mode drawing, and never in 1-cycle mode. Runs where it happens are
independent of each other and of the VI line. The rate is 1–4% on the PAL
console and 2–15% on the NTSC console, depending on the test.

Because it is random and depends on internal pipeline state, it is left
out of this change.

## Known limitation

The scheme assumes that a framebuffer row is written only by its owning
worker. If a game re-points the colour image so that differently laid-out
buffers overlap within one batch, the hand-over can still race. That race
already exists in parallel mode for any image read of such memory.


