# Emulate the RDP load hazard (pending TMEM writes after a load)

## Problem

On real hardware, the last TMEM writes of `Load_Tile`, `Load_Block` and
`Load_TLUT` are not finished when the next command arrives. If a
`Set_Tile` or `Set_Texture_Image` follows the load with no sync in between,
it changes where those writes go, or which RDRAM rows they read.

angrylion completes every load immediately, so it does not show this. On my
`rdp_timing` test ROM, 6 of the 429 cases differ from real hardware for this
reason (cases 425–446). They are the only differences left in the
single-threaded renderer.

## How hardware behaves

I measured this on NTSC and PAL consoles with five versions of a test ROM,
`rdp_load_hazard`. It drives the RDP directly, reads all of TMEM back after
each case, and compares it with the same list that has a sync before the
change. The results are deterministic and identical on NTSC and PAL. They
are also identical whether the command list is in RDRAM or in DMEM (XBUS),
even though commands are fetched about twice as fast from DMEM. So the
effect depends on how many commands follow, not on time.

**The queue**
- The last 15 *slots* of a load are still pending when the next command
  arrives.
- One slot is one iteration of the loading pipeline: 8 source bytes, or one
  `Load_TLUT` entry.
- Each row of a load takes `max(words + 1, 9)` slots. The slots before the
  row's words write nothing.

**What retires the queue**

| Next command | Effect |
|---|---|
| Any command that only sets state: colours, scissor, other modes, combine, images, `Set_Tile`, `Set_Tile_Size`, `Set_Texture_Image`, no-op | Retires **one** slot, after its own state change. The write uses the tile descriptor (TMEM address, line) of that moment. |
| `Sync_Load`, `Sync_Pipe`, `Sync_Tile`, `Sync_Full` | Completes the whole queue first |
| Any drawing command (rectangles, texture rectangles, triangles) | Completes the whole queue first |
| Another load | Completes the whole queue first |
| Nothing (the RDP is idle) | Completes the whole queue about 30 µs after the RDP runs out of commands |

**Set_Texture_Image**
- Each row's source address is fixed at a certain slot. If
  `Set_Texture_Image` arrives before that slot, the row is read from the new
  address.
- The slot is "4 slots before the row's first write", adjusted by the RDRAM
  alignment of the **previous** row. A two-row test, where each row's
  alignment was varied on its own, shows that only the previous row matters.
- The adjustment is in `load_queue_latch_adj()`. The formula matches all 568
  scans (row widths 8–128 bytes, every 8-byte source offset, three stride
  layouts, 2 and 16 rows) exactly, byte for byte.

## The change

- `loading_pipeline()` computes the slot layout of the load. It performs
  every write except those in the last 15 slots, which it stores in a
  per-worker queue: texture coordinates, source offset and row.
- `rdp_cmd()` retires one slot after each state-only command, and completes
  the queue before syncs, drawing commands and loads.
- `Set_Texture_Image` moves the queued rows whose latch slot has not passed
  yet.
- The body of the pipeline loop is now `load_qword()`, unchanged except that
  the TMEM formatting is decided when the write happens.
- **Idle RDP.** The plugin has no clock, so the queue is completed whenever
  the command list runs out (the end of `n64video_process_list()`). In
  parallel mode, a marker entry in the command buffer tells the workers.
  This matches hardware whenever the CPU or RSP waits more than ~30 µs
  before sending more commands.
- **Parallel mode.** The queue is part of `rdp_state`, so every worker keeps
  its own and behaves the same.

## Testing

mupen64plus 2.6.0, `Parallel = False` and `Parallel = True`. Every result
was compared with the console's output.

| Test | Before | After |
|---|---|---|
| `rdp_timing` v3: framebuffer dumps (88) | 71 identical | **88 identical** |
| `rdp_timing` v3: CRCs of all 429 cases × 2 passes | 6 hazard cases differ | **0 differ** (single-threaded, and in parallel mode together with my parallel-mode fix) |
| `rdp_load_hazard` v1: 91 TMEM dumps | hazard cases differ | 90 identical (the remaining one: see below) |
| `rdp_load_hazard` v2: 50 gap scans × 41 gaps, RDRAM and DMEM | every hazard case differs | **all 2050 lines identical** |
| `rdp_load_hazard` v3: 40 geometries, 30 command types, idle waits | — | identical except the idle waits shorter than 30 µs (see below) |
| `rdp_load_hazard` v4/v5: 512 STI alignment scans | — | **all identical** |

In `rdp_timing`, the hazard cases only matched hardware in parallel mode
together with the parallel-mode fix in my other pull request. The two
changes do not conflict.

A small visual test ROM, `rdp_load_hazard_demo`, is attached. It runs
without the libdragon runtime, so it also works in mupen64plus 2.6.0. It
shows eight panels: four with `Set_Tile` and four with `Set_Texture_Image`,
each sent a few commands after a load, and checks each panel against the
hardware behaviour.

| Renderer | Panels matching hardware |
|---|---|
| Real hardware | 8/8 |
| angrylion before this change | 3/8 (no effect in any panel) |
| angrylion after, `Parallel = False` and `True` | **8/8** |

See `rdp_load_hazard_demo_before_after.png` for the two screens side by
side.

## Known limitations

- **Short idle gaps.** When a list stops and more commands follow less than
  ~30 µs later, hardware still holds the queue. Here it has already been
  completed. In the test ROM this only happens when the CPU writes `DPC_END`
  twice in quick succession on purpose.
- **Changing the texel size with `Set_Texture_Image` inside the window.**
  Hardware then re-reads the row with the new size: new address, and twice
  as many bytes for 8 → 16 bit. This is one case in `rdp_load_hazard` v1 and
  is not modelled. Changing only the image width has no effect on hardware,
  and none here.

None of this happens with the usual libultra or libdragon command
sequences, which put a sync or a draw right after each load. The change
should only matter for code that sets state between a load and the next
sync.

