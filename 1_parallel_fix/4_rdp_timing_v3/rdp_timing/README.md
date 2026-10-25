# rdp_timing

Measures how long the N64's RDP takes to execute command lists on real
hardware, and writes the results to the flash cart's SD card as
tab-separated text for emulator development.

Each test case is a small RDP command list ending in `Sync_Full`. The RDP's
own cycle counters are cleared just before the list starts and read when the
`Sync_Full` interrupt arrives:

| Column | Source | Unit |
|---|---|---|
| `clock` | `DPC_CLOCK` | RCP cycles (62.5 MHz) while the RDP clock runs |
| `bufbusy` | `DPC_BUFBUSY` | cycles the command buffer was busy |
| `pipebusy` | `DPC_PIPEBUSY` | cycles the pipeline was busy |
| `tmem` | `DPC_TMEM` | cycles TMEM was busy (loads) |
| `count` | CPU `COUNT` | 46.875 MHz ticks from the `DPC_END` write until the interrupt was seen |

State setup (render modes, texture loads) runs first as a separate list that
is not measured, so a measured list contains only the commands under test.
Empty lists are measured too ("baseline"), so the cost of starting a list
and of the final `Sync_Full` can be subtracted.

The RDP is driven directly through the DP registers rather than through
rdpq, so every command is exactly as written in `main.c`.

## Changes in v3

* Fixed: the CI8 texture setup sent `Set_Texture_Image` (for the TLUT) right
  after `Load_Tile` with no sync. On hardware the end of the load then reads
  from the new address, which corrupted the last 32 bytes of the texture
  (found with the v2 probes). The setup now follows libultra's order
  (`Sync_Pipe` after a load, `Sync_Tile` before changing a tile, `Sync_Load`
  before a load). Only rendering of CI8 cases changes; ids 0-424 are the
  same cases as in v2.
* New `hazard` group (ids 425-446): that race, measured on purpose. A 1 KiB
  `Load_Tile` / `Load_Block` is followed by a gap (0-64 no-ops, or one sync
  command) and then a command that changes the load (`Set_Texture_Image`, or
  `Set_Tile` on the load tile or on another tile), with no sync. TMEM is then
  read back as I8, showing which bytes the load got wrong and where they came
  from.
* New `span` group (ids 447-461): 2-cycle rectangles with framebuffer reads,
  varying the left edge, width, framebuffer width (320/336), height, and
  whether the framebuffer starts zeroed or with a per-pixel pattern, plus
  1-cycle and no-blend controls. angrylion-rdp-plus draws a wrong first pixel
  (x=0) on full-width spans in this mode; these cases narrow down when.
  Their dumps contain whole framebuffer rows.
* Framebuffer dump writes are retried once if they fail.

## Changes in v2

* Z cases are split into `zpass` (depth decreases per primitive and the Z
  buffer starts at "far": every pixel passes and is written) and `zfail` (Z
  buffer starts at "near": every pixel fails). In v1 repeated primitives at
  the same depth mixed both, so the per-pixel Z cost depended on `k`.
* In the `vi_on` pass every run starts on the same VI line (the first active
  line), so the amount of video DMA a run competes with is repeatable. In v1
  the VI phase was arbitrary, which made `vi_on` numbers vary by up to ~7%
  between runs on different consoles.
* Framebuffer dumps: after the timing passes, the cases that rendered
  differently from angrylion-rdp-plus in v1 (CI8 textures, 2-cycle full
  screen blend/AA) and their 1-cycle counterparts are drawn once more and
  their pixels written to the SD card, for pixel-by-pixel comparison.
* Four CI8/TLUT probe cases that show the palette indices stored in TMEM and
  the palette contents directly (see "Framebuffer dumps").

Case ids differ from v1; compare v2 files only with v2 files.

## Test matrix

462 cases, each run 5 times, in two passes:

* `vi_off`: VI switched off, so no video DMA competes for RDRAM (the screen
  is blank during this pass)
* `vi_on`: VI scanning out a 320x240x16 frame, as in a game; every run
  starts when the VI reaches its first active line

| Group | What | Variations |
|---|---|---|
| baseline | empty list | from RDRAM, from RSP DMEM (XBUS) |
| fill | `Fill_Rectangle` in fill mode | 16/32-bit framebuffer x 7 sizes |
| copy | `Texture_Rectangle` in copy mode | RGBA16, CI8+TLUT x 7 sizes |
| rect | rectangles in 1-cycle and 2-cycle mode | 16/32-bit framebuffer; untextured, RGBA16 point/bilinear, TEX*PRIM, RGBA32, CI8+TLUT, I4, blending, Z pass, Z fail, antialiasing, textured+blend+Z pass; x 7 sizes |
| tri | right triangles, legs 8/32/128/240 px | flat, shaded, textured, shaded+textured+Z pass / Z fail (bilinear), perspective |
| load | TMEM loads, 8 per list | Load_Tile / Load_Block RGBA16, Load_Tile RGBA32 and CI8, Load_TLUT 16/256 |
| cmd | 256 copies of one command | no-op, syncs, Set_* commands; some also from DMEM |
| probe | CI8/TLUT diagnostics, 32-bit framebuffer | see "Framebuffer dumps" |
| hazard | load followed by an unsynced change | see "Changes in v3" |
| span | 2-cycle framebuffer-read rectangles | see "Changes in v3" |

Rectangle sizes: 8x8, 32x32, 64x64, 128x128, 320x1, 1x240, 320x240. Small
primitives are repeated (`k` up to 32) so every list does a measurable amount
of work. Dithering is off and the framebuffer is cleared before the first
run of each case, so rendering is deterministic.

## Building

Requires [libdragon](https://github.com/DragonMinded/libdragon) (preview
branch) and its toolchain in `$N64_INST`.

```sh
make            # builds rdp_timing.z64
```

Keep the default debug build (do not define `NDEBUG`): SD card access uses
libdragon's `debug_init_sdfs()`.

## Running

Run `rdp_timing.z64` from a flash cart with an SD card (64drive,
EverDrive-64, ED64Plus / Super 64, SummerCart64). On hardware the run should
take one to two minutes (the `vi_on` pass waits for the VI line before every
run). Results are written to:

```
sd:/rdp_timing/rdp_timing_NNN.txt        summary, one line per case and pass
sd:/rdp_timing/rdp_timing_NNN_raw.txt    every individual run
sd:/rdp_timing/rdp_timing_NNN_fb/        framebuffer dumps + index.txt
```

`NNN` counts up (001, 002, ...), so runs on several consoles or several runs
on one console never overwrite each other. If no SD card can be used, the
summary is sent to the debug log instead (USB via UNFLoader on 64drive /
SummerCart64, or IS-Viewer in emulators).

## File format

Plain text, tab-separated, UTF-8. Lines starting with `#` are comments; the
first ones are `key<TAB>value` metadata (`mi_version` identifies the RCP
revision, plus TV type, memory size and runs per case). The first
non-comment line is the column header, so the files load directly into a
spreadsheet, pandas (`pd.read_csv(f, sep="\t", comment="#")`) or any CSV
reader.

Summary columns:

| Column | Meaning |
|---|---|
| `id` | case number (same case in both passes and in every file of the same version) |
| `pass` | `vi_off` or `vi_on` |
| `source` | `rdram` or `xbus` (list read from RSP DMEM) |
| `group`, `prim`, `cycle`, `fb`, `tex`, `filter`, `flags` | what was drawn and how |
| `w`, `h` | primitive or texture size |
| `k` | primitives (or commands) in the list |
| `pixels` | pixels per primitive (triangles: s*(s+1)/2) |
| `clock_med/min/max` | `DPC_CLOCK` median / min / max over the runs |
| `bufbusy_med`, `pipebusy_med`, `tmem_med` | medians of the other counters |
| `count_med/min/max` | CPU `COUNT` ticks |
| `drawn` | non-zero framebuffer pixels after all runs; should equal `pixels` (0 for `zfail`; not meaningful for `prefill` span cases) |
| `crc32` | CRC32 of the framebuffer after all runs (cleared before the first); compares rendering between consoles and emulators |

Raw file columns: `id pass run clock bufbusy pipebusy tmem count`.

The summary ends with `# vi_sync_line` (the VI half-line every `vi_on` run
started on) and `# vi_sync_misses` (runs where the wait timed out; should be
0).

## Framebuffer dumps

`rdp_timing_NNN_fb/` holds one file per dumped case, `case_III.bin`: the drawn
area (w x h pixels from the top left), big-endian, row-major; 16-bit pixels
are RGBA5551, 32-bit pixels RGBA8888. `index.txt` lists the cases in the same
tab-separated format as the summary.

```sh
python3 tools/rdp_fb_diff.py rdp_timing_001_fb                 # analyse one set
python3 tools/rdp_fb_diff.py console_fb emulator_fb --png out  # compare two sets
```

Analysing one set checks every copy / 1-cycle CI8 dump against the texture
and palette the ROM uses (reporting which texture rows show wrong palette
entries), reports for each hazard case which TMEM bytes are wrong and whether
they came from the new source address or never got written, reports for each
span case the drawn extent and any rows whose first pixel differs from its
neighbour, and decodes the probes:

| Probe | What it shows |
|---|---|
| `ci8_as_i8` | the CI8 texture read back as I8 after the TLUT load: the palette indices actually stored in TMEM |
| `ci8_as_i8_notlut` | the same without a TLUT load (control) |
| `tlut_view` | a 16x16 texture with indices 0..255: pixel i shows palette entry i |
| `tlut_view_line32` | the same, with the TLUT load tile's line set to 32 instead of 0 |

Comparing two sets lists the differing pixels of each case and, with
`--png`, writes `case_III_A.png`, `case_III_B.png` and a red-on-grey
`case_III_diff.png`.

## Analysing

```sh
python3 tools/rdp_timing_report.py rdp_timing_001.txt
python3 tools/rdp_timing_report.py rdp_timing_001.txt --pass vi_off --group rect
python3 tools/rdp_timing_report.py rdp_timing_001.txt --csv derived.csv
python3 tools/rdp_timing_report.py console.txt --compare emulator.txt
```

The report subtracts the baseline of the same pass and source and shows the
cost per primitive and per pixel. `--metric` selects `clock` (default),
`pipebusy`, `bufbusy`, `tmem` or `count`. `--compare` lines two result files
up case by case: the timing ratio, and whether the rendering (CRC) matches.
Standard-library Python only.

## Notes

* v1 on real NTSC and PAL consoles confirmed that `DPC_CLOCK` counts every
  RCP cycle: `count` x 4/3 matched it to within 0.01%.
* The counters are 24-bit; the largest case stays far below that limit.
* Run it on more than one console if you can: comparing `mi_version` and
  results between revisions is part of the point.
* v2 was tested in mupen64plus with angrylion-rdp-plus for control flow,
  rendering and file output (that emulator does not model RDP timing, so all
  its counters read 0); the texture setups render the exact expected texels
  in the same RDP core. In that emulator the final status message can hang
  in libdragon's vblank handler (lazy FPU save in a nested exception, which
  mupen64plus with the test patches mishandles) after all files are written;
  this is not a problem on hardware, where v1 ran fine.

`examples/` holds the output of one emulator run (mupen64plus +
angrylion-rdp-plus): `angrylion_summary.txt`, `angrylion_raw.txt` and the
framebuffer dumps in `angrylion_fb/`. The timing columns are all 0, but the
`crc32` column and the dumps are a reference for how angrylion renders each
case: compare a console's dumps against `angrylion_fb/` with
`rdp_fb_diff.py`. Note that angrylion's first pixel in 2-cycle
framebuffer-read cases depends on what was rendered before (its cycle-0
blender uses the previous pixel's memory colour, carried over between
spans and primitives), so those pixels can differ between runs of the
reference; hardware is consistent.
