# Root-Cause Analysis of the Donkey Kong 64 Title Demo Desync and the Fix in ares

*English translation of REPORT.md (the Japanese file is the original).*

Target ROM: Donkey Kong 64 (J) ― NDOJ, CRC 053C89A7-A5064302, big-endian 32MB
Target emulator: ares (developed on master 9408cb43; also applies to master as of a774671d, 2026-10-26)
Updated: 2026-10-31 (reflects results of hardware test ROMs v9 to v16. VI mode factors, dynamic bus contention, CPU instruction timing and SP DMA are now based on measured values)

## Conclusion (Summary)

- The cause of the desync is **not rendering timing but a difference in the series of frame intervals the game measures (number of VI retraces = frame delta)**.
- The DK64 DKTV demo replays "inputs recorded per game frame (stick X/Y, buttons, camera angle)", and the character's movement is **scaled by the delta (2 = 30fps, 3 = 20fps, ...)**.
- On the console, this demo in Jungle Japes runs with **a constant delta of 3 (one frame reliably exceeds 2 VI = 33.3ms)**.
  This is also supported by the fact that the DK64 Recompiled project fixes the delta to 3 in the same scene
  (`patches/patches_main.c`: `if (gameIsInDKTVMode() && current_map == MAP_JAPES) D_global_asm_80744478 = 3;`).
- In ares, the RDP execution time is **0**, so the frame time is determined by CPU time alone and fluctuates right around 33ms,
  which makes the **delta flip between 2 and 3**. Since the inputs are per frame, in a delta-2 frame DK's movement is only 2/3 as large,
  so he passes on the wrong side of the palm tree or falls without reaching the rope.
- As the fix, I added to the ares RDP a **model that estimates execution time from the command stream**, and made it delay the Sync_Full DP interrupt and the
  busy flag of DPC_STATUS by that amount. The model's factors are calibrated against the **frame display intervals of the console (N64Digital HDMI output
  recorded at 60fps with an EzRecorder 330)**. After the fix, a run through from power-on completes just like the console:
  climb the cliff, jump to the rope, swing across to the far side on the pendulum, and get launched from the barrel cannon.
- Updated 2026-10-27: Using the measurement results of the hardware test ROMs (v7/v8), I replaced the estimated correction factors with **measured values (placement penalty, per-mode contention, VI)**,
  and added the CPU cache transfer delay during RDP rendering (measured value) as `0003`.
- Updated 2026-10-31 (patches 0004 to 0007): With ROMs that replay the game's own display lists on the console (v9/v10/v12/v15) and instruction-timing ROMs for the
  CPU (v11/v14) and RSP (v16), I replaced all remaining degrees of freedom in the model with measured values.
  **VI mode factors** (RDP +11.7% in the game's AA+divot+dedither mode, +32% for hires, +3% in MGC's aa2 mode),
  **dynamic contention per bus master** (CPU fill 11.7 / instruction fill 8.5 / writeback 5 / SP DMA 1.22 clk/8B),
  placement penalty 15, and on the CPU side FPU (pipelined, div/sqrt 29 to 30, cvt 5), load/store hazards,
  a fix for the double charge in the recompiler, uncached read 32 / RCP register read 23, and SP DMA 38 cycles + 0.155/B.
  The DK64 demo is the best so far (score 22), the BAR crash scene matches, and the BAR intro remains on a knife edge
  at 2.0 vs 2.5 VI/frame on the console (see below).
- Added 2026-11-01 (bar_tasklog): With a patch that puts a resident logger into the game itself, I recorded every frame of the BAR intro on the console (normal run and a run with the CPU stopped),
  and decomposed the residual. CPU contention is within 8% of the model, and RSP/RDP are +2.7% (heavy) / +6.7% (light) on the console even with the CPU stopped. A per-type analysis of v15 showed that the
  −7% in the crash scene is the **Z-test pass rate** (which depends on the scene's draw order). Because a uniform factor is incompatible with DK64, this stays at series v7 (the patch is identical to v7).
- Added 2026-10-31 (series v7, patch 0011): With a ROM (v20b) that decomposes each condition of v20 into "large FIFO (RSP does not wait)" and "RDP frozen → RSP alone / unfrozen → RDP alone",
  I confirmed that in ares **RSP alone (F3DEX2) is +2% and RDP alone is −1% relative to the console** (frame 2316). What was missing was the **time the RSP's DMA
  takes away from the RDP** (0.057 VI/frame on the console, 0.080 in VI game mode, 0.049 in ares); I corrected it by raising the base cost of one DMA from 6 to 9 and applying the VI factor. From the same ROM's
  CPU chase, I re-measured the RDP loss per CPU fill from 11.7 to 10 (13.3 with VI on). The RDRAM page effect ROM (v21) was judged to need no change on the CPU side.
  The BAR intro goes from 252 to 249 frames (console: 225), and DK64 is unchanged.
- Added 2026-10-31 (series v6, patch 0010): With a ROM that replays the game's RSP graphics tasks on the console (v20), it turned out that **on the console the RSP task waits with the FIFO full until
  0.05 to 0.3 VI before the RDP** (ares finished 3 times faster). With the fix that returns `DPC_CURRENT` according to the model's progress,
  the RSP now waits as on the console, the BAR intro goes from 271 to 252 frames (console: 225), and the shape of the frame count per 60 VI matches the console.
- Added 2026-10-31 (patches 0007 to 0009, series v4): `0007` was replaced with a PI timing fix that gets past the MGC 2011 demo's emulator detection.
  I added the results of the mixed-load ROM (v17), the third round of CPU tests (v18), and the SP DMA contention ROMs (v19/v19b) as `0008`/`0009`: for the FPU, with normal operands
  add 3 / mul.s 5 / mul.d 8 / div.d and sqrt.d 58 (v14's "2 / 30" was the fast path), a 4-entry write buffer (uncached write 16 cycles each,
  fills and uncached reads wait for it to drain), asynchronous dirty writeback, cache instruction 5, an RDP penalty of 4 for uncached writes,
  a VI ×1.12 for SP DMA and mutual delay with CPU line transfers, and the rule that during consecutive DMA the RDP can use only 36 clk per DMA (v19b). Load interactions are additive, with no superadditivity. The BAR intro is unchanged (see Remaining Issues).

## Course of the Analysis

### 1. Game-Side Mechanism (ROM Analysis)

The J version's main code (global_asm) is the gzip at ROM 0x116F0 decompressed to RAM 0x805F8B00.

| Item | J version address | Contents |
|---|---|---|
| VI interrupt handler | 0x8060CCA4 | Increments the VI retrace count (`sched+0x284` = 0x80767EB4) and executes the pending buffer swap |
| RDP completion (DP interrupt) handler | 0x8060D1A4 | Compares with `D_807460D0` (VI at which the next swap is allowed) and either swaps or holds. After the swap, `osDpSetStatus(FREEZE)` |
| Frame delta computation | 0x805FE024 | Records `t0 = retrace count − previous` into an 8-element history and updates `D_80743D38` (the delta) with hysteresis. It then busy-waits until `retrace count ≥ previous + delta` |
| DKTV input replay | 0x80608EC4 / 0x80608F4C | Advances one entry per game frame (4 bytes: 15-bit camera angle + button update flag, stick X, stick Y) and writes it to the pad structure (0x807ED180) |
| Physics delta consumption | 0x8061B7E4 and many others | Uses `delta × 0.5` as the time step (1.0 at delta 2, 1.5 at delta 3) |

Hysteresis rule: if t0 of the last 2 frames is larger than the current delta, it is raised immediately; if all of the last 4 frames are smaller, it is lowered.
Therefore **exact cycle matching is unnecessary**, and the delta series is determined solely by "whether the frame time always exceeds 33ms".

### 2. Reproduction and Verification in ares (Headless Harness)

I ran the ROM with a self-made harness stripped of GUI dependencies, and recorded RAM (current map 0x8076A298, player coordinate ptr 0x807FBFBC +0x7C/0x80/0x84, delta 0x80743D38, DKTV index 0x80745C6C) every VI frame.

| Condition | Delta series (during demo) | Result |
|---|---|---|
| ares as is (RDP time 0) | Mostly 3, but 2 intrudes frequently (2×15, 2×22, 2×27 ...) | Falls into the valley at X≈1758 (as reported) |
| Delta fixed to 2 (code patched in RAM) | Always 2 | Falls via a different path |
| **Delta fixed to 3** | Always 3 | **Jumps to the rope, swings across to the far side → launched from the barrel cannon → transitions to the next demo (normal)** |
| Fixed ares (default values) | Almost 3 (a few 4s right after the start, short 2s in the second half) | **Completes normally** |

That the run becomes normal with the delta fixed to 3 is direct proof that the cause is the frame delta series.

Independently of the RDP time, in ares the CPU is busy almost 100% during the demo (PC samples of the idle thread 0x80000A04 are about 2.5%), and
each game frame is dominated by cache misses: about 30,000 dcache misses (≈13ms) plus about 10,000 writebacks (≈4ms).
On the console, CPU cache fills also become slower while the RDP occupies RDRAM, so the CPU side should be slower than ares as well.

### 3. Consistency with Known Information

- ares Issues [#1652](https://github.com/ares-emulator/ares/issues/1652) / [#1175](https://github.com/ares-emulator/ares/issues/1175): cause not identified, only "very sensitive to timing".
- The simple64 2022.07 release note "DK64 intro now grabs the rope" is due to a general review of timing.
- [DK64 Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled)'s `patches_main.c` fixes the delta to 3 in the Japes DKTV (consistent with the conclusion of this analysis).
- ares's "succeeds 2 times out of 10" (PAL) is probably because ares randomizes the RDRAM initialization timing (ccLow/ccHigh in `rdram.cpp`), which shifts the start time, and
  frames near the boundary happened to tip to the 3 side.

### 4. Calibration Against Console Recordings

From two console recordings (1080p 59.94fps), I extracted the number of consecutive identical frames (= the number of VI in the display interval)
(`analyze.py`: the picture is judged to have "changed" when the mean difference of 480×270 grayscale > 0.6). Over the whole demo section, the two recordings agree
within ±1 VI for the totals of every 25-frame block, so **the console's behavior is deterministic**.

For the Japes demo section (about 494 game frames), the VI count total per 25 game frames (console → ares):

| Block | 0 | 25 | 50 | 75 | 100 | 125 | 150 | 175 | 200 | 225 | 250 | 275 | 300 | 325 | 350 | 375 | 400 | 425 | 450 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Console | 79 | 75 | 76 | 78 | 83 | 82 | 78 | 75 | 75 | 74 | 71 | 73 | 75 | 80 | 77 | 75 | 74 | 70 | 73 |
| ares before fix | 97 | 74 | 73 | 80 | 87 | 88 | 83 | 76 | 71 | **65** | **64** | 72 | **63** | **64** | 73 | 77 | 72 | 74 | 72 |
| ares after fix | 98 | 77 | 76 | 80 | 86 | 87 | 84 | 77 | 76 | 75 | 75 | 76 | 79 | 87 | 76 | 77 | 75 | 75 | 69 |

Histogram of display intervals (2 / 3 / 4 or more): console 28/436/30, before fix 105/319/76, after fix 19/417/64.

- Before the fix, ares matched the console almost exactly to a few percent slower in the first half (the CPU-bound section), but in the second half (the section where the console's RDP time exceeds the CPU)
  2s occurred frequently (block totals 63 to 65 = average 2.6 VI), and there the game's delta dropped to 2 and desynced.
- The initially considered idea of "imposing a penalty on CPU cache fills while the RDP is busy" turned out, by comparison with the recordings, to slow the CPU-bound first half
  far more than the console (block totals 92 to 104), so I did not adopt it at that point
  (the value then was a flat 12 to 20 cycles. Later I adopted the values measured on the console, 10/4/12, as `0003`; see the 2026-10-27 section).
- With an RDRAM contention factor of 2.5 the second half is too fast and fails, with 3.5 it is too slow and fails, and 2.6 to 3.0 completes (error is smallest at 2.75 to 2.9).
  For compatibility with Beetle Adventure Racing described below, I adopted **2.9** (the value at the time of the estimate model. After the console-measured model was incorporated the meaning of the factor changed and it became **1.8**; see below).

## Beetle Adventure Racing (J) OP Demo (Calibration at the Time of the Estimate Model)

Using the same method, I compared console recordings (two, also agreeing within a few VI = deterministic) with ares.

- The OP is based on 30fps (display interval 2); in light scenes it leans toward 60fps (1 2 1 2 ...), and in heavy scenes it exceeds 33ms (2 3 2 3 ...).
  Scene cuts are time-based (the VI count of each scene matches between ares and the console), but the number of game frames within a scene
  varies with the emulator's frame time.
- Before the fix, ares (RDP time 0) ran the whole OP at 60fps. With a factor of 3.0, heavy scenes are about 25% slower than the console
  (e.g. seg1: console 306 frames vs ares 281), and 2.75 brings the per-scene frame counts closest.
- Helicopter crash scene (VI count from the segment start; console: shatter begins at +658 to 660, "crash" text shown at +668):

| Factor | Shatter | Text | Gap |
|---|---|---|---|
| Console | +658 to 660 | +668 | 8 to 10 |
| 2.6 | +644 | +666 | 22 |
| 2.75 | +652 | +664 | 12 |
| **2.9** | **+658** | **+668** | **10** |
| 3.0 | +662 | +672 | 10 |

  The relative timing of the shatter (car progress = frame-based) and the text (time-based) moves with the factor, and matches the console at 2.9.
- The symptom "the text appears first" occurs with the first patch I sent (factor 2.5 + CPU stall 12). The CPU stall term
  makes CPU-bound light scenes slower than the console, so the car's progress is delayed and the text gets ahead. With the current version (no CPU stall) it
  does not reproduce in the headless environment, and both order and gap match the console.
- Verification scripts: `seq.py` (display interval series from ares's rendering statistics) and `cmpseg.py` (scene-by-scene comparison with the console video).

## Integrating the Hardware-Measured RDP Timing Model (updated 2026-10-26)

I **replaced** the original estimated model ("ideal value × contention factor 2.9") with `rdp_timing_model` (v3 to v6, identical for NTSC/PAL, median error 0.5%), which was measured in a separate session. The main differences from the estimated model:

| Item | Old (estimate) | New (measured table) |
|---|---|---|
| Span | Pixel rate + per-stream (2.5+6+B/8)×2.9 + 12 | Table by mode × address phase × width (includes RDRAM waits) |
| 1cyc16 320px | 589 clk | 331 clk (almost entirely pixel-rate bound) |
| 1cyc16 blend+Z 32px | 235 clk | 170 clk (280 at ×1.8) |
| fill16 320px | 331 clk | 192 clk (282 at ×1.8) |
| Primitive | Fixed 14 | Depends on mode and width (−15 to +35, overlaps during continuous drawing) |
| Texture load | 8B/clk + RDRAM read×2.9 | Measured formula: 13.5 + 0.48/word + 22.7/64B, etc. |
| Z fail | Not distinguished | Separate table (about half of pass), pass rate estimated from overdraw |

Using the table **as is** (contention factor 1.0) makes DK64 too fast in its second half and it fails (on the console the CPU/RSP/VI compete for RDRAM).
I recalibrated by applying a factor only to the RDRAM-wait portion:

| Factor | DK64 error (25-frame block total difference) | DK64 2/3/4 histogram | DK64 completes | BAR seg0/seg1 frame counts (console 225/306) | BAR crash shatter/text (console +658 to 660/+668) |
|---|---|---|---|---|---|
| 1.0 | 72 | 91/333/76 | ✗ | – | – |
| 1.5 | 53 | 52/382/66 | ✗ | – | – |
| 1.7 | 39 | 36/408/56 | ○ | 278/347 | +629/+667 |
| **1.8** | **38** | 30/410/60 | ○ | 251/327 | +639/+667 |
| 1.9 | 42 to 46 | 31/412/57 | ○ | 251 to 253/313 to 331 | +638 to 644/+666 |
| 2.0 | 66 | 17/404/79 | ○ (block 13 is 95 = delta 4 mixed in) | – | – |
| Old model 2.9 | 40 | 25/419/56 | ○ | 257/302 | +658/+668 |

- At 1.8, DK64 is equal to or better than the old model (the histogram is also closest to the console's 28/436/30). It completes for 1.6 to 1.9.
- BAR remains **3 to 7% faster** than the console with the new model (heavy scenes have many 2-VI frames, and the crash shatter occurs about 20 VI earlier than on the console.
  The text is time-based, so it matches). For BAR alone, a factor of 2.3 to 2.4 is best, but DK64 fails at 2.0, so the two cannot both be satisfied.
  The symptom "the text appears before the shatter" does not occur (shatter +639 → text +667).
- I investigated what separates the two games but have not found a decisive factor:
  - CPU/RSP RDRAM traffic (ares RBus statistics) is the same for both games at about 32MB/s → cannot be explained by a variable factor.
  - The VI settings (320×240 16bit, AA+divot) are also identical.
  - I tried a model in which the RSP waits for the RDP to consume the output buffer (`DPC_CURRENT` throttling); only DK64 became much slower
    and BAR was unchanged → not adopted (my guess is that BAR waits for DP completion, while DK64 has a game loop that reacts to SP completion).
  - Buffer layouts of both games (obtained in ares): BAR has color 0x1DA800 / 0x200000 and Z 0x3DA800 (Z and one of the color buffers are exactly 2MiB apart =
    identical low addresses); DK64 has color 0x01D240 / 0x042A40 and Z 0x5D2E00 (Expansion Pak region).
  - A real-hardware test ROM for isolation, `rdp_timing_v7` (libdragon, saved to SD, runs on 4MiB consoles), is provided in `rdp_timing_v7.zip`
    (A: color/Z buffer layout, B: Set/sync commands between primitives, C: texture loads between small primitives,
    D: direct measurement of RDRAM contention by CPU/SP DMA/VI, E: scattered primitives). Results can be aggregated with `analyze_v7.py`.
  - Remaining candidates are color/Z buffer bank contention (the measurement placed both buffers on page boundaries; with BAR's buffer layout row misses may increase),
    implicit pipeline waits when `Set_Other_Modes` is used heavily without Sync_Pipe (BAR issues 704 per frame),
    and error in BAR's CPU time. These can be isolated with additional hardware measurements (span measurements with varying buffer addresses, the cost of consecutive Set commands (state)).
- It is also possible to set `zPassAuto=false` and fix `zPassFraction` (with a fixed 0.75, DK64 is slightly faster and BAR is faster still).

## Hardware Test ROM v7/v8 Results and the Final Model (updated 2026-10-27)

These are the NTSC/PAL hardware results of the test ROMs (`rdp_timing_v7` / `rdp_timing_v8`, libdragon,
saved to SD) built to isolate "the factor that separates the two" from the previous section (originals in `hw/`; NTSC and PAL agree for all series).

### RDP side (v7 A to E, v8 G/GT)

| Series | What was examined | Result |
|---|---|---|
| A | Relative placement of color and Z buffers | If Z is **within ±256KB** of the color buffer, +25 to 33% for 64×8 rectangles and +3% for a 200-row list. Both BAR (color 0x1DA800/0x200000, Z 0x3DA800) and DK64 (color 0x1D240/0x42A40, Z 0x5D2E00) have "far" layouts, so this is **not the cause** |
| B | Set/sync commands between primitives | Set commands take 1 to 6 clk with no implicit wait. Only `set_convert`/`set_key_r` act like a sync (≈400), and a slow state remains until the next `Set_Other_Modes` (because of garbage values). Game-like sequences: sync_pipe+set_other_modes ≈50 (Z mode 15 to 40), sync_tile+set_tile ≈34, sync_load+image+tile+load_block 128B ≈100, the full F3DEX2 texture switch set ≈155 — **matches the model's values** |
| C | Texture loads between small primitives | Interference between loads and primitives ≈0 — as in the model |
| E/G/GT | Primitive placement | For any placement other than stacking (drawing on the rows continuing from the previous primitive), modes that read color/Z cost **+35 to 50 clk per span** (scattered rectangles), 30 to 41 for side-by-side h≥4, 13 to 26 for triangle strips, and 28 to 44 for scattered triangles. Write-only modes (plain 1cyc) cost 0, and fill costs +6 to 11 |
| D | RDRAM contention from other masters | The RDP delay is proportional to the number of transactions from other masters. At about 2M trans/s, Z spans are ×1.19 to 1.22, write-only spans ×1.34 to 1.39, and copy ×1.3. Running SP DMA at full speed gives ×9. VI scanout adds +6.9% only in Z modes and 0% in plain |

The difference between the measured table (stacked drawing, idle console) and real games can be explained by the **"placement penalty (per span)" and the "contention factor (per mode)"**,
so the previous free parameter "RDRAM-wait portion ×1.8" is no longer needed.

### CPU side (v8 F: CPU while the RDP is drawing)

I measured the CPU loop with the COP0 counter (VI on, with the RDP drawing a long list). CPU cycles per operation:

| CPU operation | Idle console | ares | Increase while RDP draws (blend+Z / plain / fill) |
|---|---|---|---|
| dcache fill (sequential) | 38.8 | 45 (48/40 setting) | +3.8 / +0.9 / +2.0 |
| dcache fill (dependent, random) | 45.5 to 46.6 | — | Not measurable (the list finishes first) |
| dcache writeback (separated from copy) | ≈41 | 40 | +10 / +26 / +15 |
| icache fill (including 8 nops) | 54 | ≈56 | +10 / +12 / +12 |
| Uncached 32bit read | 41 | 11 | +3.6 / +0.6 / +1.3 |
| Uncached 32bit write | 19 | 9 | +10.7 / +6.1 / +13 |
| ALU | Match | Match | 0 |

- While the RDP is drawing, CPU cache transfers are stalled by **+10 cycles for icache, +4 for dcache, and +10 to 25 for writeback**.
  The earlier decision to remove the "CPU stall" was because the values then (a flat 12 to 20) were too large; the measured values were 1/3 to 1/2 of those.
- ares's uncached RDRAM accesses are faster than the console's (read 11 vs 40), but in both games uncached RDRAM accesses during OP are
  negligible (BAR 0, DK64 about 20 per frame), and MMIO accesses are only 100 to 140 per VI, so this has no effect.

### Final model (patches 0002 + 0003)

| Item | Value | Basis |
|---|---|---|
| Placement penalty for spans that read color/Z, `spanPenalty` | 40 clk/span | G/GT scattered 35 to 50 (strips 13 to 26) |
| Contention for spans that read color/Z, `contentionZ` | ×1.13 | D (game-equivalent 1 to 1.5M trans/s) |
| Contention for write-only spans and TMEM loads, `contentionPlain` | ×1.19 | D |
| During VI scanout, `viFactor` | ×1.07 for Z modes only | D |
| CPU: icache fill / dcache fill / writeback | +10 / +4 / +12 cycles while RDP draws | F |

Results (ares default recompiler, full run from power-on):

| Version | DK64 error | DK64 2/3/4 | Completes | BAR seg0/seg1 (console 225/306) | BAR shatter/text (console +658 to 660/+668) |
|---|---|---|---|---|---|
| Previous (×1.8) | 38 | 30/409/61 | ○ | 251/327 | +639/+667 |
| Measurement correction only (no CPU stall) | 37 | 33/408/59 | ○ | 276/335 | +636/+667 |
| **Final (measurement correction + CPU stall)** | 69 | 18/391/91 | ○ | 257/334 | **+651/+667** |
| Same, interpreter | 81 | 18/380/102 | ○ | 250/334 | **+659/+667** |

- BAR's helicopter crash is at +651 (+659 in the interpreter) against +658 to 660 on the console, a large improvement over the previous +639.
  The frame count of the heavy scene (seg0) also moved closer, 251→257 (console 225).
- DK64 completes, but the CPU-bound section in the first half (blocks 4 to 6) is about 10% slower than on the console (86 to 87 → 91 to 92, console 82 to 83).
  This section has many CPU cache misses while the RDP is drawing, and **the tension remains: with the measured stalls DK64 is too slow, and
  without them BAR is too fast**. Lowering ares's base icache/dcache fill costs (48/40) to
  the console's sequential-access values (46/35) brings DK64 back to 47, but BAR goes back to +647 (the `i4` experiment).
  The console's dependent/random-access fill is 45.5, close to ares's value, so I left the base costs unchanged.
- The ares recompiler makes the CPU about 4.5% faster than the interpreter (with the RDP model disabled, 325 vs 311 lists per 500 VI of BAR),
  so the same model gives slightly different results. The interpreter side is closer to the console.
- For fine tuning: `spanPenalty` in `rdp.hpp` (25 to 40 is within the measured range; smaller favors DK64) and
  `ICacheFillStall/DCacheFillStall/DCacheWriteBackStall` in `cpu.hpp` (setting them to 0 gives the equivalent of the previous version).

## Hardware Test ROM v9 to v16 and the Final Model (updated 2026-10-31)

Up to v7/v8, the measurements used synthetic primitives. From v9 on, I **replayed on the console game display lists dumped from ares (DK64 795 to 797 /
1795 to 1797, BAR 2316 to 2318 / 2556 to 2558, late BAR intro and start of the crash, MGC 2011)** and measured RDP clocks per list under the same
conditions as the game (VI mode, CPU/RSP bus load). Results are saved in `out/hw/`.

### RDP (v9/v10/v12/v15)
- **Accuracy of the table model**: For lists replayed with vi_off, the prediction from the table plus a placement penalty of 15 is DK64 ×0.96 to 0.98, BAR intro ×1.00,
  BAR crash ×1.06 to 1.08, late BAR ×0.99, and MGC ×0.95 to 0.98. The ratios for variants in which command types are replaced with NOP (triangles only /
  no loads / no rectangles / state only / fetch only) **agree with the console within ±3% for all lists**, and the residual is a uniform
  per-list scale (the vi_off values of the two consoles agree within 0.1%).
- **VI mode factors** (NTSC unit, 320×240 16bit, ratio to vi_off): the ROM's aa3 (replicate) ×1.004, aa2 (resample) ×1.03 (the same at 1280 px width),
  **the game's AA0+divot+dedither ×1.117**, and the same at 640 px width ×1.324. The PAL unit in the same modes is ×1.020 / ×1.148 / ×1.179 / ×1.496 respectively,
  clearly heavier, so this depends on the unit (region). The model defaults to the NTSC values.
- **Contention per bus master** (clocks the RDP loses, in the game's VI mode): CPU D cache fill 14.6, instruction fill 12, writeback 6.2,
  uncached write 5.5, SP DMA 64B 13.3 / 512B 91.5. The values in the ROM's VI mode (11.7 / 8.5 / 5 / 3.7, DMA 1.22 clk/8B)
  are multiplied by ×1.25 (`viContendScale`) via the VI factor. The fixed factors 1.13/1.19/1.07 were abolished, and each transfer is now charged through `timingDelay`.
- **Command fetch**: for a 352 KB list, 171k clk (11% of the frame, 2.8 to 3.9 clk/word). Overall it overlaps with drawing.
- **Meaning of the DPC registers (v13)**: `DPC_CURRENT` is the fetch pointer, and during a long primitive it stops at START+240 (240 B prefetch). Extending `DPC_END` during execution is accepted immediately.
  Writing a new `DPC_START` during execution switches at the next END but the RDP then hangs
  (an operation the game avoids). Lists with the syncs removed also hang the RDP (the known behavior where Set_Tile/Load hits a tile in use).

### CPU (v11/v14, same for NTSC/PAL)
- Integer: load → use in the next instruction +1, store → next is a memory instruction +1, a D cache hit itself +0 (ares had +1), an untaken likely branch
  also costs 1 cycle for the slot, mfc0 Count 2, mtc0 2, and jr/beq do not interlock on a load result.
- The FPU is pipelined: add/sub/mul (single and double) issue in 2 cycles, a dependent result comes 3 cycles later, cvt/trunc 5, div.s = sqrt.s = 28 to 29, sqrt.d 30,
  compare 1. add/sub have a value dependency, taking 3 on cancellation. A div with dividend 0.0 terminates early (that is the 15.5 in v11).
- Bus: uncached RDRAM read 33 cycles (ares 1 to 2), RCP register read 23 (MI is 11, DMEM/IMEM 29, RDRAM registers 32), PIF RAM 2981,
  cartridge ROM 219. Writes go through a 4-entry write buffer: 1 cycle each for up to 4, then ≈16 each (≈9 for the RCP), and a later read waits for the drain (not implemented).
- Cache: clean fill 41.4, with a dirty victim 66 (= fill + 25 writeback: ares is 80), two consecutive fills 43 each.
  ares's stalls while the RDP draws are measured at fill +5 / instruction +6 / wb +11, and in the game's VI mode a further +6 / +6 / +2.
- Recompiler bug: 1 cycle was double-charged for instructions that go through the slow path (miss, uncached) (wScottSh's "+1 compared with the interpreter").

### RSP (v16)
- **Instruction timing matches ares exactly** (dual issue, dependent vector instruction 4 cycles, lqv → use +1, branch bubble, mfc0 2).
- SP DMA on the console is 38 RSP cycles + 0.155/B (out is 32 + 0.145/B), and ares used 1 system clock per 8B → changed to 114 system clocks + 4/8B.

### Final model (defaults after applying patches 0002 to 0007)

| Item | Value | Basis |
|---|---|---|
| Placement penalty `spanPenalty` | 15 clk/span | v9/v15 list replay |
| VI factors `viFactorAA0/AA2/AA3` | 1.117 / 1.03 / 1.004; for AA0/1, the portion beyond 640 B/line grows by ×1.77 | v15 NTSC |
| Contention (clk the RDP loses) `contendCpu/I/W/Dma` | 11.7 / 8.5 / 5.0 / 1.22 per 8B, × (1 + 0.117×1.6) | v10/v12 |
| CPU stall (while RDP draws / VI AA0 mode) | fill +5+6, instruction fill +6+6, wb +11+2 | v10/v12 |
| CPU fill base cost | I 48, D 40, writeback 25 (fill first, asynchronous) | v14 |
| FPU issue / dependent latency | add·sub·mul 2 / 3, cvt 5, div.s·sqrt.s 29, .d 30, compare 1 | v11/v14 |
| Hazards | load-use +1, store→mem +1, mfc0 Count +1, mtc0 +1, untaken likely +1 | v11/v14 |
| Uncached read / RCP register read | 32 / 23 cycles | v14 |
| SP DMA | 114 system clocks + 4/8B (≈38 RSP cycles + 0.155/B) | v16 |

Results (ares default recompiler, full run from power-on):

| Version | DK64 error | DK64 2/3/4 | Completes | BAR seg0/seg1 (console 225/306) | BAR shatter/text (console +658 to 660/+668) |
|---|---|---|---|---|---|
| 0002+0003 (10-27) | 69 | 18/391/91 | ○ | 257/334 | +651/+667 |
| **0004 to 0007 (10-31)** | **22** | 35/438/27 | ○ | 278/339 | +627/+667 |
| 0004 to 0009 (10-31, series v4) | 28 | 33/418/49 | ○ | 271/338 | +635/+667 (measured with the harness at the same settings) |
| 0004 to 0009 + v19b rule (series v5) | 29 | 32/420/48 | ○ | 258/337 | +637/+667 (measured with the harness at the same settings) |
| **0004 to 0010: DPC_CURRENT model (series v6)** | 28 | 33/418/49 | ○ | **252/335** | +639/+667 (harness) |
| **0004 to 0011: re-measured DMA/CPU contention (series v7)** | 29 | 26/422/52 | ○ | **249/331** | +641/+667 (harness) |
| **0004 to 0012: Z whole-span fail only for spans of 16 px or less, ATOMIC_PRIM, 1-cycle AA (series v8)** | 29 | 24/423/53 | ○ | **249/331** | +641/+667 (harness) |

- The DK64 demo is now the series closest to the console (per-block VI counts are within 1 to 2).
- BAR's crash scene (RDP-bound) matches the console (68/74/66 vs 70/76/69 VI/30 frames).
- BAR's intro runs at 2.0 VI/frame in ares and 2.5 on the console (a 50/50 mix of 2 and 3). The RDP time of the intro lists matches the console replay (×1.00, 1.5 VI including
  the VI factor) and is ≈1.95 VI including contention. The CPU is 87% busy in ares. In other words, **both the RDP and the CPU are within a few % of the 2 VI boundary**, with the console
  above it and ares below it. Slowing the CPU uniformly by 25% brings BAR closer to the console but breaks DK64, and slowing the RSP by 25% changes nothing,
  so the remaining difference is thought to be a few % on the CPU side (value-dependent FPU costs, the write buffer, super-additivity under combined loads that were not measured).
  The frame structure is the same in both the 8MB and 4MB console recordings, so the presence of the Expansion Pak is irrelevant.
- MGC 2011 (Retroactive): in comparison with the console recording, from demo start to the RETRO logo it is 8954 VI on the console and 8547 in ares (−4.5%). Per part, −12% to +3%.
  This demo has a different kind of load in which PI loads, the RSP, and the CPU are intermixed, and factors outside the scope of the RDP model remain.
## Hardware test ROMs v17 to v19 and the v4 model (added 2026-10-31)

Up to v16, loads were measured one at a time. v17 to v19 measured **the interactions when loads are applied simultaneously**, plus the CPU
value-dependent costs and the write buffer that v14 left unmeasured (one NTSC and one PAL console each; results in `hw/v17_*`, `v18_*`, `v19_*`).

### v17: game-list playback x combined CPU/RSP load (8MB)
- **Loads add up.** The ratio between the prediction (the "clocks the RDP loses per operation" derived from each load alone, multiplied by the count) and the measurement under the combined condition is
  0.99 to 1.03 (only ucw+dma64 gives 1.06 to 1.07). Even with CPU+RSP+VI+RDP running at the same time, no super-additive effect occurs.
- Clocks the RDP loses per operation (hardware / ares): D-cache fill 13.8 / 14.5 (VI on), 9.7 (VI off),
  mixed load+store (2 fills + write-back) 27 to 28.5 / 34 to 37.5, **uncached write 4 to 5 (VI off 2.7) / ares 0**,
  SP DMA 64B 11 to 13 / 9 to 17, 512B 86 / 78 to 81. -> The RDP penalty for a write-back, `contendCpuW`, goes 5 -> 1, and `contendCpuU` = 4 is newly added for uncached writes.
- CPU side (while the RDP is drawing): chase 1 fill 55.5 cycles (ares 56), mixed 1 operation 130 (ares 149 -> due to the write-back being charged serially),
  **uncached write 33 (ares 8.2)**.
- Note: the `icache` load had its 2 KiB of straight-line code fit entirely in the I-cache (16 KiB), so it was effectively no load (the result can be read as a control condition meaning "the CPU is just
  running inside the cache"). Also, the VI effect disappears from the 5th list onward on NTSC and from the very start on PAL (game and vi_off have the same value), which we take to be the same "VI stops" symptom as in v15. Rows that include the VI are valid only for the first 4 lists on NTSC.

### v18: FPU value dependence, write buffer, write-back drain, cache instruction
- **FPU (issue cost, normal operands)**: add/sub (single and double) 3, mul.s 5, mul.d 8, div.s 29, **div.d 58**, sqrt.s 29, **sqrt.d 58**,
  cvt 5 (cvt.d.s 1, cvt.s.d 2), compare 1. A dependent next instruction costs an additional +1. Fast paths: adding +0.0 costs 2, multiplying by 0.0 / 1.0 / 2.0 costs 2,
  dividing with a dividend of 0.0 costs 2, sqrt(0.0) costs 2. The "add/mul issue 2" and "div.d 30" seen in v11/v14 were because the operands were 1.0/0.0 and
  hit the fast path (sqrt.d 30 is the average of 2 and 58). This means the original ares values (other than add 5) were nearly correct.
- **Write buffer**: an uncached RDRAM write drains at 16.3 cycles per entry (17.4 with VI on), regardless of width (sb/sh/sw), and 18 to 19 for sd.
  Up to 4 entries the CPU does not stall; from the 5th it waits for the drain. ALU instructions proceed during the drain (even 8 stores x 14 instructions has a total time bound by the drain rate).
  A subsequent uncached read waits for the drain to complete (+13), **a line fill waits for the drain to complete + about 9 bus turnaround (+25)**,
  and a cache hit does not wait. SP/DP register writes take 10 cycles, MI register writes take 2.
- **Dirty write-back**: if a fill comes right after, +28; if there are 16 or more cycles of work in between, most of it is hidden, leaving a residual of +6. Until now ares charged 25
  serially (because the v14 test was back-to-back).
- **cache instruction**: 5 cycles per instruction (I-cache hit invalidate 6), a dirty-line write-back takes about 17 cycles per 16 B (bound by the drain rate),
  and a clean/absent line costs only 5. ares had 1.

### v19: SP DMA under RDP drawing, VI, and CPU misses (S series)
| Condition (RDP idle/drawing, VI, CPU) | dma_in 8B | 64B | 512B | 3584B |
|---|---|---|---|---|
| idle / off / idle (baseline 38 / 44 / 108 / 581 RSP cycles) | 1.00 | 1.00 | 1.00 | 1.00 |
| idle / off / **CPU chase** | 1.16 | 1.39 | 1.74 | **1.94** |
| idle / **320x240 game mode** / idle (PAL console) | 1.04 (1.12) | 1.10 (1.09) | 1.16 (1.24) | **1.18 (1.26)** |
| idle / game mode / chase | 1.28 | 1.61 | 2.09 | **2.39** |
| while drawing fill / tex / texzb / off / idle | 1.00 to 1.12 | 1.05 to 1.12 | 1.00 to 1.06 | 1.01 |
- RDP drawing barely delays SP DMA (+0 to 5%, up to 12% for small DMAs). **A CPU line fill delays DMA substantially**:
  about 18 RCP clocks per fill overlapping a 3584 B DMA, and at the same time the CPU-side fill also goes 46 -> 65 cycles (+19). VI game mode
  slows DMA by 18% (26% on the PAL console). RSP instruction execution (vertex_like 37, mfc0 DPC_CURRENT 10) is unchanged under every load.
- The D series (measuring the time of an RDP list under continuous SP DMA) produced 0 rows in v19 because of a ROM bug; it was obtained with the fixed version **v19b** (`hw/v19b_*`).

### v19b D series: the RDP during continuous SP DMA
A fixed list (fill 40 rectangles ~ 1.77M clk, tex 20 rectangles ~ 1.63M clk) was measured while the RSP kept issuing DMAs without pause (DPC_CLOCK wraps around at 24 bits,
so values were converted from CPU ticks). **The RDP can use the bus for only about 36 clocks per DMA, regardless of DMA size**:

| DMA (continuous) | Iteration (RSP cycles) | Clk available to RDP / iteration | RDP slowdown |
|---|---|---|---|
| dma_in 8 B | 43 | 37 | x1.2 |
| 64 B | 46 | 36 | x1.3 |
| 512 B | 113 | 37 | x3.1 |
| 3584 B | 585 | 36 | **x16** |
| dma_out 512 / 3584 B | 103 / 555 | 36 / 36 | x2.8 / x15 |
The same holds for fill/tex and for NTSC/PAL. In VI game mode it is 33 to 39, and when the CPU chase overlaps it drops to about 30 (the chase also stretches the DMA itself, so
its effect on the RDP goes up or down depending on the condition). In other words, what the RDP loses is "DMA duration - about 32" = 6 per transfer + 1.2 clocks per 8 B
(8 B: 6, 64 B: 10, 512 B: 76, 3584 B: 549). ares v4 had only 1.22 per 8 B with no per-transfer component, and the DMA itself was also on the long side at 0.167/B,
so at 3584 B, with 55 to 109 clk/iteration left for the RDP, the slowdown was only x6. -> 0009 is updated to the v19b rule (DMA 0.15/B,
RDP penalty 6 + 1.2/8B). Game DMA is sparse (BAR intro: 682 per VI, 160 KB total per VI, about 3% of RDP time), so BAR does not change.

### Reflection in the v4 model (patches 0008 / 0009)
| Item | v3 | v4 | Basis |
|---|---|---|---|
| FPU issue add·sub / mul.s / mul.d / div·sqrt .s / .d / cvt | 2 / 2 / 2 / 29 / 30 / 5 | **3 / 5 / 8 / 29 / 58 / 5** (dependent +1) | v18 |
| Uncached write | 1 cycle, no RDP effect | 4-entry write buffer, 16 cycles each (+15 while the RDP is drawing), RCP registers 10, MI 2; reads and fills wait for the drain, fills get +9 | v18/v17 |
| Dirty write-back | CPU 4 + bus 25 (asynchronous) | CPU 6 + bus 17, the next fill gets +9 turnaround | v18 |
| cache instruction | 1 | 5 (write-back asynchronous 17) | v18 |
| Clk the RDP loses: write-back / uncached write | 5 / 0 | **1 / 4** | v17 |
| SP DMA | 38 RSP cycles + 0.167/B | 38 + 0.15/B, x1.12 (VI AA0/1 mode, aa3 is x1.04); +18 clk per overlapping CPU line transfer, and that CPU transfer gets +19 cycles | v16/v19 |
| Clk the RDP loses: SP DMA | 1.22 / 8 B | **6 + 1.2 / 8 B** (the RDP can use only about 36 clk between DMAs) | v19b |

Result (series v4 row of the results table above: DK64 error 28, BAR 271/338): **the BAR intro does not run correctly** (1.7 VI/frame, same as v3,
vs 2.06 on the console). Refining the CPU model with measured values does not help as much as making the CPU uniformly 25% slower did.
Since v17 ruled out super-additivity, the remaining candidates are (a) a missed "CPU fill x RDP interaction with VI on", which could not be measured because the VI stopped in v17,
(b) the RSP side of the RDP list (F3DEX2's DMA becomes 1.4 to 2 times slower under CPU fills, as in v19, which interrupts the supply to the RDP:
the ares RSP is modelled so that RDP command supply does not stop even during DMA), and (c) the game's frame loop side (where it waits for RSP task completion).
For (b), the v19b D series showed that "the RDP almost stops during continuous DMA", but game DMA is sparse and amounts to only about 3% of BAR's RDP time, so
  the BAR intro does not run correctly. What remains is (a) and (c), and the RSP-to-RDP supply timing itself during F3DEX2 execution in ares (waiting on a full command FIFO via XBUS).

## Hardware test ROMs v19b/v20 and the v6 model: the RSP waits for the RDP (added 2026-10-31)

### v20: replaying the game's RSP graphics task as-is on the console
At the moment ares starts a BAR graphics task (HALT clear in SP_STATUS), it dumps 4 MB of RDRAM, DMEM, IMEM, and the VI/DPC registers. A ROM linked at
0x80600000 restores them on the console and starts the RSP from the same state as the game, **measuring the time of RSP halt (task completion) and of the RDP's DP interrupt
(Sync_Full) simultaneously** (`hw/v20_*`, NTSC/PAL, 3 runs each, min).

| Frame / condition | Console RSP done [VI] | Console RDP done | ares v5 RSP | ares v5 RDP |
|---|---|---|---|---|
| bar_2316 game idle | **1.538** | 1.587 | **0.504** | 1.484 |
| bar_2316 game chase | 2.285 | 2.378 | 0.532 | 2.421 |
| bar_2316 vi_off idle | 1.376 | 1.418 | 0.498 | 1.399 |
| bar_2579 game idle | 1.449 | 1.652 | 0.687 | 1.696 |
| bar_2520 game idle | 0.428 | 0.637 | 0.217 | 0.573 |

- **On the console, the RSP task does not finish until 0.05 to 0.3 VI before the RDP.** F3DEX2 writes RDP commands to a FIFO in RDRAM (in this OSTask,
  8 KB at 0xFDF80 to 0x100000, 5% of the frame's 175 KB of commands); if the next flush would overwrite the RDP's unread portion, it reads
  `DPC_CURRENT` and waits, and at the end of the FIFO it waits for `DPC_CURRENT == DPC_END` before wrapping around. In ares, the instant `DPC_END` is written,
  the renderer consumes all the commands and `DPC_CURRENT = DPC_END`, so the RSP never waited and finished at 0.5 VI (3 times too early).
  The game's CPU waits for the SP interrupt (task completion) before advancing to the next frame, so ares released the CPU about 1 VI too early. This is the mechanism behind
  the BAR intro's 2.0 vs 2.5 VI/frame.
- The RDP-side time agrees between ares and the console within -6% to +8% (also under the CPU chase). The RDP fed by the RSP in a real task is 6% slower than v15/v17 (1.56M clk), which
  replay the same command sequence as a continuous list (1.65M clk: waits at the moments when the FIFO empties and RSP DMA contention).
- The PAL console matches NTSC in vi_off, and is +4% in VI game mode.

### v19b/v19c: the RDP during continuous SP DMA
The RDP can use the bus for only about 36 clocks per DMA (regardless of size) -> the 0009 rule (6 + 1.2/8B per transfer) was already reflected in v5.

### Fix (patch 0010: `n64: DPC_CURRENT follows the modelled RDP progress`)
On every `DPC_END` write, it records "the range (current..end) and the amount of work it queued", and a `DPC_CURRENT` read returns **the position inside the oldest incomplete range
that the completed amount of work has reached** (linear interpolation by work amount within the range; END once everything is done). This alone makes F3DEX2's FIFO wait and
wrap-around wait occur just as on the console, and running the v20 ROM in ares makes RSP completion come 0.05 to 0.3 VI before RDP completion (same as the console).
The range ring is not included in save states (right after a load, `DPC_CURRENT = DPC_END` until the next `DPC_END` write).

Result (clean core 0001 to 0010): series v6 row of the results table below. The BAR intro seg0 frame count approaches the 250s from 271 (console 225), and the
pattern of frame counts per 60 VI block takes the same shape as the console (45 55 66 65 75 63 53 vs console 46 56 70 67 75 65 56).
We attribute the remaining ~10% to the sum of the RDP model's -6% (real task in VI game mode) and a few percent on the CPU side.

## Hardware test ROMs v20b/v21 and the v7 model: decomposing RSP, RDP, and DMA contention (added 2026-10-31)

### v20b: decomposing each v20 condition into RSP alone / RDP alone / DMA contention
The same dump as v20 (4 frames of BAR graphics tasks) was measured as normal (8 KB FIFO, the RSP waits for the RDP), and additionally

- **Large FIFO**: the OSTask's output FIFO is moved to 1 MB on the expansion memory side (from 0x500000), and both are run with the RSP never waiting for the RDP.
- **Frozen**: additionally the RDP is DPC-frozen and only the RSP is run (`t_sp` = **F3DEX2 alone**); after the RSP halts, the counter is cleared and the RDP is unfrozen (`t_dp` = **RDP alone**,
  fed from the real FIFO, no RSP DMA contention).

(`hw/v20b_*`, NTSC/PAL, 3 runs each, min. For ares, `hw/v20b_ares_v6.txt`/`v20b_ares_v7.txt`, the median of 3 runs from thread times.
The first ares run is excluded because the VI factor has not fully taken effect and it is fast.)

| Frame / VI / CPU / condition | Console NTSC RSP / RDP [VI] | Console PAL | ares v6 | ares v7 |
|---|---|---|---|---|
| bar_2316 game idle normal | **1.538 / 1.587** | 1.602 / 1.659 | 1.463 / 1.535 | **1.474 / 1.553** |
| bar_2316 game idle large FIFO | 0.498 / 1.573 | 0.498 / 1.643 | 0.504 / 1.535 | 0.504 / 1.553 |
| bar_2316 game idle RSP alone / RDP alone | **0.494 / 1.493** | 0.495 / 1.590 | 0.504 / 1.479 | 0.504 / 1.479 |
| bar_2316 game chase normal | 2.279 / 2.372 | 2.410 / 2.483 | 2.085 / 2.333 | 2.039 / 2.268 |
| bar_2316 game chase large FIFO | 0.515 / 2.392 | 0.517 / 2.508 | 0.532 / 2.360 | 0.532 / 2.291 |
| bar_2316 game chase RSP alone / RDP alone | 0.511 / 2.274 | 0.512 / 2.428 | 0.534 / 2.301 | 0.534 / 2.205 |
| bar_2316 vi_off idle normal | 1.376 / 1.418 | 1.376 / 1.418 | 1.338 / 1.399 | 1.341 / 1.403 |
| bar_2316 vi_off idle large FIFO | 0.493 / 1.398 | 0.493 / 1.397 | 0.498 / 1.399 | 0.498 / 1.403 |
| bar_2316 vi_off idle RSP alone / RDP alone | 0.490 / 1.341 | 0.490 / 1.341 | 0.498 / 1.343 | 0.498 / 1.343 |
| bar_2316 vi_off chase normal | 1.888 / 1.960 | 1.888 / 1.960 | 1.849 / 2.057 | 1.755 / 1.929 |
| bar_2316 vi_off chase RSP alone / RDP alone | 0.501 / 1.899 | 0.501 / 1.898 | 0.529 / 2.021 | 0.529 / 1.883 |
| bar_2579 game idle normal | 1.449 / 1.651 | 1.309 / 1.499 (VI stopped) | 1.452 / 1.734 | 1.468 / 1.754 |
| bar_2579 game idle RSP alone / RDP alone | 0.674 / 1.541 | 0.670 / 1.392 (VI stopped) | 0.687 / 1.626 | 0.687 / 1.626 |
| bar_2579 vi_off idle normal | 1.308 / 1.499 | 1.308 / 1.498 | 1.336 / 1.590 | 1.338 / 1.593 |
| bar_2579 vi_off chase normal | 1.734 / 1.989 | 1.733 / 1.987 | 1.818 / 2.279 | 1.730 / 2.146 |
| bar_2520 game idle normal | 0.429 / 0.636 | 0.435 / 0.662 | 0.406 / 0.575 | 0.412 / 0.583 |
| bar_2520 game idle RSP alone / RDP alone | 0.206 / 0.599 | 0.203 / 0.540 (VI stopped) | 0.217 / 0.549 | 0.217 / 0.549 |
| bar_2520 vi_off idle normal | 0.378 / 0.569 | 0.378 / 0.569 | 0.368 / 0.522 | 0.369 / 0.524 |
| bar_2520 vi_off chase normal | 0.513 / 0.774 | 0.512 / 0.774 | 0.425 / 0.768 | 0.418 / 0.721 |

(On the PAL console the VI stops partway from bar_2316 onward (the same phenomenon as v17), and the "game" columns for bar_2520/2543/2579 have the same values as vi_off.)

Findings (frame 2316, NTSC):

- **RSP alone (F3DEX2 execution)**: console 0.494 VI, ares 0.504 (+2%). 2579: 0.674 / 0.687, 2520: 0.206 / 0.217 (+5%). The ares RSP is
  2 to 5% slower, but since the direction is toward "slower", it is not the cause of the BAR intro problem.
- **RDP alone**: console 1.493 (VI off 1.341), ares 1.479 / 1.343 (-1% / 0%). The VI game-mode factor is +11.3% on the NTSC console (model +11.7%) and +18.6% on the PAL console.
  The per-frame model error is as in v15/v17: 2579 +5.5%, 2520/2543 -8%.
- **Time the RSP's DMA takes from the RDP** (large FIFO - RDP alone): console **0.080 VI** (VI off 0.057), PAL console 0.053 (VI off 0.057). ares v6 is 0.049 regardless of VI.
  F3DEX2 performs 1164 DMAs per frame (485 list fetches of 168 B, 430 output flushes of 344 B, vertices 448 B, matrices 64 B, etc.,
  262 B on average, 305 KB total), and with VI off takes **51 clk per DMA** from the RDP. The continuous-DMA rule from v19b (6 + 1.2/8B) gives 45, and an isolated DMA is
  a few clk higher. In VI game mode it splits by console: 71 on the NTSC console and 47 on the PAL console.
- **Cost of the FIFO coupling itself** (normal - large FIFO): +0.9 to 1.4% for idle, -1% for chase. This is negligible, and with the v6 (0010) DPC_CURRENT model the RSP's
  completion time already has the shape seen on the console (RSP completion console 1.538 / ares 1.474, the difference being mostly the RDP-side difference).
- **RDP loss from the CPU chase**: (RDP alone (chase) - RDP alone (idle)) / number of loads = VI off **10.4 clk/fill**, VI game mode **13.3** (v17: 9.7 / 13.8,
  v10: 11.7). ares v6 gives 11.7 x 1.19 = 13.9 (11.7 for VI off), which is 13% too high with VI off and 4% too high with VI on.
- Contention among all three (RDP + CPU chase + RSP DMA) is slightly larger than the sum (extra DMA cost during chase 0.118 VI vs 0.080 during idle).

### v21: RDRAM page/bank effects (the remaining suspect on the CPU side)
Dependent chained loads chasing randomly through 4 MB (`hw/v21_*`): one fill costs 45.5 cycles for ranges of 64K to 256K, 43.0 for 1M, **38.0 for 4M**; alternating
access between two points 1 MB or more apart is 38.0, under 1 MB is 45.5, sequential is 38.0, with dirty-line write-back included it is 71.3 regardless of range, uncached reads are 35 (4M) / 42.6 (64K), and instruction fill is
about 44 (ares 48). So the ares value of 43 lies in the middle of the console's 38 to 45.5, and **the cost of a CPU fill is not the cause of the BAR intro residual** (no change).

### Fix (patch 0011: `n64: SP DMA and CPU bus contention against the RDP refitted on a real graphics task`)
- The RDP loss per SP DMA, `contendDmaBase`, goes 6 -> **9** (48 clk per DMA with F3DEX2's DMA mix, console 51), and the loss is multiplied by the same VI factor `contendScale`
  as the CPU (`rsp/dma.cpp`).
- The RDP loss per CPU fill, `contendCpu`, goes 11.7 -> **10.0**, and the slope of the VI factor, `viContendScale`, goes 1.6 -> **2.4** (x1.28 in VI game mode: fill 12.8, DMA 61).

Result: frame 2316 (normal, VI game mode) RSP completion / RDP completion = 1.474 / 1.553 VI (console 1.538 / 1.587, v6 1.463 / 1.535), chase 2.268 (console 2.372,
v6 2.333), VI-off chase 1.929 (console 1.960, v6 2.057). The full run of clean core 0001 to 0011 is in the series v7 row of the results table: BAR intro seg0 252 -> 249
frames, seg1 335 -> 331, DK64 28 -> 29 (within the error range), crash scene +641/+667 (console +658 to 660/+668).
## Measurement with a resident logger inside the game itself (bar_tasklog) and the v15 command-class analysis (added 2026-11-01)

### bar_tasklog: recording the frame timeline of BAR (J) on the console
This is not a test ROM but a **patch to the game itself** (`bar_tasklog/`; the ROM is not included and is generated from a dump with `make_patch.py`). At entry, a 1.2 KB logger is copied from the padding at the end of the ROM
(0xFF0000) to 0x80700000 (upper 4 MB, unused by the game, Expansion Pak required), and 4 libultra functions in RAM
(`osSpTaskLoad` / `osSpTaskStartGo` / `send_mesg` in the exception handler / `osSpTaskYield`) are replaced via `j`/`jal`. With the COP0 Count attached, it records
"graphics task start, RSP stop (yield / completion), DP interrupt, VI interrupt" at 4 bytes per entry. Recording starts automatically at the first heavy frame of the intro
(a task of 0.5 VI or more); when 32 KB has accumulated (the whole intro is ≈1600 VI) it is written to the cartridge SRAM and recording stops, and the EverDrive `.srm` is then analyzed
(`decode_tasklog.py`). The same patched ROM run in ares produces a log that can be matched against the hardware log **by frame number** (the contents of seg0 correspond by frame number,
and the length of seg0 is 465 VI on both, so they are time-synchronized). The variant `hook_idle.bin` is a version in which, during recording, the CPU that started a graphics task waits until the RSP stops
in a loop that does nothing but read SP_STATUS (it does not touch the bus, and since the scheduler is stopped there is no yield either), which lets us **isolate the in-game CPU contention directly on the console**.

Hardware logs (`hw/bar_tasklog_hw_005_normal.srm`, `..._006_idle.srm`; the normal version's 2 runs agree to within ±0.005 VI on every frame) versus ares v7
(`bar_tasklog/ares_v7*.srm`), first 165 frames of seg0 (`hw/bar_tasklog_compare_seg0.txt`):

| | Console | ares v7 | Difference |
|---|---|---|---|
| Frame period | 2 VI (95) / 3 VI (19) (last 114 frames of seg0) | mix of 1 / 2 / 3 VI | — |
| Number of frames in seg0 (465 VI) | **230** | 250 | +8.7% |
| RDP completion (CPU idle), heavy frames / light frames | 1.553 / 0.782 | 1.512 / 0.735 | **+2.7% / +6.7%** |
| RSP completion (CPU idle), heavy / light | 1.460 / 0.702 | 1.392 / 0.648 | +4.9% / +8.6% |
| In-game CPU contention (normal − idle, RDP completion), heavy / light | +0.309 / +0.134 | +0.287 / +0.118 | **+8% / +14%** |
| RDP completion, normal version, heavy / light | 1.862 / 0.920 | 1.799 / 0.853 | +3.5% / +7.9% |

- BAR's frame period is decided as 2 or 3 VI by "whether RSP completion plus roughly 0.22 to 0.25 VI of CPU post-processing fits within 2 VI" (the threshold on the console is ≈ RSP completion of 1.75 VI),
  and the heavy frames sit exactly on that boundary. In ares the light frames (RSP completion 0.62 to 0.75) fit within 1 VI, whereas on the console (0.78 to 0.85) they do not fit and
  become 2 VI — this accounts for most of the 20-frame difference in seg0.
- **The CPU contention model (10 clk/fill × VI 1.28) is within 8% of the console.** Even with the CPU stopped, the console's RDP completion is later by an average of **+0.039 VI/frame (σ 0.03)**, and
  this difference does not correlate with frame content (number of pixels, spans, primitives; memory-bound ratio) (|r| ≤ 0.3). PI DMA (1 KB/frame or less during the intro),
  audio DMA, and fills (model 43.7K clk/screen = v19b measurement 44.2K) are all ruled out as the cause.
- Fitting BAR with a uniform RDP factor breaks DK64 (harness: ×1.04 → seg0 251 / DK64 29, ×1.08 → seg0 230 / DK64 37, CPU contention 11 + ×1.05 →
  235 / 39). CPU contention of 10.8 alone gives DK64 27 to 30 and BAR 249 to 254, within error. **The residual is a content-dependent ±5%, with DK64 and BAR on opposite sides.**

### Per-class comparison using v15's "replay with a command class replaced by NOPs"
For each list in v15/v15b (hardware, VI-off equivalent), I split the difference from ares (v7, with the harness aggregating clk per class) by command class
(`hw/v15_class_fit.txt`).

| Class (contribution = full − no_X) | BAR 2316 series | BAR 2556 / c2520 (crash) | BAR t2500 (light) | DK64 797/1795 to 7 | MGC |
|---|---|---|---|---|---|
| Overall, console/ares | 0.99 to 1.01 | **0.93 to 0.95** | 1.01 | **1.02 to 1.04** | 1.00 to 1.03 |
| Triangles | 0.99 to 1.01 | 0.90 to 0.93 | 1.03 | 1.02 to 1.06 | 1.00 to 1.07 |
| Rectangles | 1.03 | 1.01 to 1.09 | 1.01 | 0.98 to 1.02 | — |
| Texture load | 1.01 to 1.02 | 1.01 to 1.07 | 1.02 | **1.06 to 1.07** | 0.99 |
| Set commands (state) only | 0.95 to 0.96 | 0.96 | 0.96 | 0.95 | 0.93 to 0.99 |

- BAR's triangles have only **5 to 11 pixels per span** (2316: 7.8K spans / 76K px), and most of the cost is the fixed per-span part (the table's
  short-span value + spanPenalty 15). DK64 is similar (1CYC16_BLEND_ZPASS, 2 to 3 px/span, 88 to 93 clk/span).
- Only the crash lists (2556/c2520) have triangles that are 7 to 10% too large. This comes from a difference in how **pixels that fail the Z test** are handled: ares's estimator
  (H(n)/n from the number of Z pixels ÷ scissor area) returned a pass rate of 1.0 with n≈1, because the Z drawing covers only half of the scissor. With an estimate that
  counts the pixels actually covered (bitmask, `T_ZCOVER`), BAR's Z drawing turns out to cover **40K pixels 2.2 to 2.5 times**, and DK64 1.5 to 1.66 times, and the
  pass rates become 0.68 to 0.73 / 0.83 to 0.88. This improves the crash lists from 0.93 to 0.97 to 0.98, but **makes the 2316 series worse (0.99 → 1.04 to 1.06) and DK64 worse
  (1.02 to 1.04 → 1.05 to 1.07)**. In other words, the 2316 and DK64 scenes are drawn back to front so nearly all Z passes, and only the crash scenes have failures.
  Since this depends on draw order it cannot be settled by estimation; **the correct approach is to return the renderer's (software RDP's) actual per-primitive Z test results to the timing model**
  (this cannot be obtained from parallel-rdp, in which case we would fall back to the current estimate). This is not included in the present series.
- DK64's texture load is 6 to 7% low and Set commands (state) are 4 to 5% high in all games (both are under 1% of the total).

### Other things confirmed
- The VI game-mode factor (model: uniform 1.117) is, by v19b measurement, ×1.140 for the fill list (PAL console ×1.202) and ×1.101 for the textured-rectangle list (×1.166).
- The cost of coupling in which the RSP waits for the RDP is +1.2 to 1.4% on the console (v20b: normal − large FIFO), but it is 0 in ares because the RDP never runs dry. Trying the granularity of `DPC_CURRENT`
  (rounding to 64 / 256 B) does not reproduce it.
- On the console the intro starts 1114 to 1134 VI after power-on, whereas in ares it is 2412 VI (a difference in the length of the boot sequence; not yet investigated).

### Conclusion
The residual of the BAR intro (seg0: 250 vs 230 frames) is the sum of (1) in-game CPU contention (+8%), (2) coupling of the RSP supply (+1.2%), (3) scene-dependent Z pass rate
(−7% in the crash scenes), and (4) content-dependent error in light frames (+6.7%), each of which is a term of a few percent at most. (1) and (2) can be put in as factors, but
they are in a range that worsens DK64 by 1 to 2 VI/block, and (3) requires coupling with the renderer, so I am leaving it as series v7.

## Hardware test ROM v22: per-class breakdown of in-game lists, DPC_CURRENT granularity, and cost of thin triangles (added 2026-11-01)

`rdp_timing_v22` (NTSC `hw/v22_ntsc_002.txt`, PAL `hw/v22_pal_001.txt`, ares v7 reference `rdp_timing_v22/ares_reference.txt`).
The VI-off values agree to within 0.1% on the 2 consoles.

### V: the actual in-game lists extracted from bar_tasklog (VI off, console/ares v7)
| List | Overall | Triangles (full − no_tri) | Texture load | Rectangles | Set commands (state) only | fetch only | Triangle px/span, spans/triangle |
|---|---|---|---|---|---|---|---|
| bar_i2274 / 2279 / 2284 (light, frames 20/25/30) | **1.035 / 1.032 / 1.032** | **1.05 / 1.06 / 1.06** | 1.02 | 1.01 | 0.94 to 0.97 | 1.00 | 37 px, 8 spans (282 to 332 triangles) |
| bar_i2314 / 2338 (heavy, frames 60/84) | 0.973 / 0.976 | 0.97 / 0.97 | 1.01 | 1.02 | 0.96 / 0.94 | 1.00 | 14 to 16 px, 13 spans (875/788 triangles) |
| bar_i2404 / 2410 (heavy, frames 150/156) | 0.963 / 0.956 | 0.94 / 0.93 | 1.15 | — | 0.95 | 1.01 | 16 px, 8 spans (1156/1071 triangles) |
| bar_2316 (v15 list) | 1.011 | 1.02 | — | — | 0.98 | 1.00 | 15 px, 13 spans (899 triangles) |
| dk64_797 | 1.058 | 1.08 | — | — | 0.95 | 0.99 | 15 px, 4.9 spans (1891 triangles, 1 cycle) |

- The +6.7% for light frames (bar_tasklog) comes from **+5 to 6% in triangles**, not from texture load, rectangles, Set commands (state), or fetch. Heavy frames are conversely −3 to 4%.
- The "texture load" and "rectangles" columns for bar_i2404/2410 and bar_2316/dk64 are omitted, because ares's reference values were order-dependent due to the automatic
  estimate of the Z pass rate (overdraw from the previous frame) (the v23 reference is taken with `T_ZPASS=1` fixed).
- Game VI-mode factors on the PAL console: overall ×1.16 to 1.17, triangles ×1.13 to 1.17, Set commands (state) only ×1.07 to 1.12, **fetch only ×1.01** (command reads are not
  disturbed by the VI). On the NTSC console, after the first case (bar_i2274 full: ×1.104, ares ×1.106) the VI stops fetching (VI probe 93K = off,
  VI_CTRL stays at 0x3012), and from then on the game-mode values are the same as VI off. On PAL the VI stayed alive during V and stopped at W.
  This is thought to be an interaction between libdragon's VI interrupt handler and this ROM's direct register writes, and is unresolved (the same phenomenon as "the VI effect disappears partway" in v15/v17).

### Q: how DPC_CURRENT advances
When the CPU keeps reading DPC_CURRENT while bar_2316 runs (48.5K reads, 1 loop = 22 ticks = 2 reads, DPC_CURRENT + MI_INTR),
the value changes 2210 times, and **per change the advance is 128 B in 72% of cases (1586 times), 64 B 13%, 256 B 7%, ≤32 B 8%** (maximum 320 B; same on NTSC/PAL; same with and without VI).
The RDP reads commands in 128 B units, and DPC_CURRENT advances at that granularity. ares uses 8 B units (even when 64/256 is tried with the harness's `T_CURQ`,
the RDP time and BAR's frame count do not change). A by-product: the same loop takes 31 ticks/iteration in ares, so **CPU reads of RCP registers are 40% slower in ares**
(ares takes 22 cyc for both MI/DPC; on the console, per v14, DPC is 23 and MI is 11).

### W: thin triangles with vertical edges (shade+texture+z, 22 words, random placement, 4096 spans)
`T(w, H) = triangle fixed part + H × per-span cost`, separated from H = 1,2,3,4,16 (VI off, Z all pass, clk):

| | Triangle fixed part | Span w=1 | w=2 | w=4 | w=8 | w=16 | w=32 | w=64 |
|---|---|---|---|---|---|---|---|---|
| 2cyc_tex_blend_z, console | **36 to 51 (mean 45)** | 74 | 78 | 85 | 97 | 121 | 167 | 237 |
| same, ares v7 (table + 15 + prim_overhead) | −9 to +30 | 76 | 92 | 98 | 110 | 138 | 185 | 271 |
| 1cyc_tex_blend_z, console | **25 to 49 (mean 40)** | 74 | 81 | 85 | 97 | 118 | 154 | 223 |
| same, ares v7 | −9 to +2 | 76 | 93 | 96 | 110 | 136 | 175 | 252 |

- An isolated triangle has **a fixed cost of about 45 clk** (including ≈ 22 clk for reading 22 words = 176 B), which ares's rectangle-derived prim_overhead (−12 to +31) does not have.
  On the other hand, the per-span cost is **the table value itself (the spanPenalty of +15 is unnecessary)**, and for widths of 32 and above it is 10 to 20% cheaper than the table.
  A 1-span, 1-px triangle is 125.6 clk on the console versus 75.4 in ares (ares is 40% cheaper); they agree for w ≥ 16.
- Z: when everything fails, each span is **29 to 41 clk cheaper in 2cyc and 18 to 49 clk cheaper in 1cyc**, and half-fail (half) lands exactly in between (e.g. 2cyc H=3 w=8: pass 110.9 / half 93.3 / fail 75.3).
  ares's approach of "mixing the pass and fail tables linearly by the pass rate" is correct; the problem is only the estimate of the pass rate (as concluded in v15).
- Other modes (H=3): 2cyc_tex (blend, no Z) is 91 on the console versus 57 in ares at w=2 (missing fixed cost) and agrees at w=32. For 2cyc_tex_blend, ares is 10 to 15% higher.

### Applying W's per-span costs directly to the game lists does not fit
When the harness is given "triangle +45, triangle spans +3, −0.8 per pixel for widths above 16" (`T_TRIOV=45 T_TRISP=3 T_SPWIDE=0.8`), the W series fits to within ±5%, but for the V series the triangles come out at
**console/model = light 1.19 to 1.21, heavy 1.05 to 1.10, bar_2316 1.13, DK64 1.06**, i.e. the game's triangles cost 10 to 20% more than synthetic triangles
of the same width and height. The spanPenalty of 15 introduced in v9 had been absorbing this difference empirically.

Going through the differences between the game's triangles and W's triangles, the first thing found was **how the rasterizer counts pixels**. ares's timing model evaluates edges at the row center
and takes `x1 − x0` pixels, whereas the console (angrylion's edge walker) processes from the floor of the left end to the floor of the right end of the 4 sub-scanlines, **both ends inclusive**,
and draws a row if even one sub-scanline falls within it. Putting this in the harness (`T_RASTER=1`) increases the span counts of the game lists by +6 to 11% for BAR and
+17% for DK64, and the pixel counts by +6 to 14%; combined with W's per-span costs the results are 1.01 to 1.02 for heavy frames, 1.04 for bar_2316, 1.06 to 1.10 for light frames,
0.94 for bar_i2404/2410, and 0.85 for DK64. The remainder is either the Z pass rate (bar_i2404/2410 and DK64 may be failing more than estimated) or mode bits
(AA, perspective, LOD, dither, TLUT, etc., which W's triangles lack), and v23 is meant to separate these.

## Hardware test ROMs v23 / v24: what makes the game's triangles expensive (added 2026-11-01)

To find the cause of v22's "applying the per-span costs of isolated synthetic triangles to the game lists comes out 10 to 20% cheaper", I separated it in two directions: (X) replaying the game lists with their mode bits /
triangle commands / Z buffer / buffer placement rewritten, and (Y) adding properties to the synthetic triangles one at a time
(`hw/v23_*.txt`, `hw/v24_*.txt`; the 2 consoles, NTSC/PAL, agree to within 0.1%; VI off).

### X: game lists (difference from full, console)
| Change | bar_i2274 (light) | bar_i2314 | bar_i2404 | bar_2316 | dk64_797 |
|---|---|---|---|---|---|
| AA_EN off | −0.1% | +0.5% | −0.6% | −0.2% | **−3.2%** |
| TEX_PERSP / TEX_LOD / dither / point sampling / EN_TLUT off | all ±0.05% | | | | |
| ATOMIC_PRIM off (set only by DK64) | 0 | 0 | 0 | 0 | **−3.3%** (20 clk/triangle) |
| 2 cycles → 1 cycle | −1.3% | −1.3% | −2.8% | −1.8% | — |
| Coverage / alpha compare off | +0.7% | +2.3% | +0.5% | +1.7% | 0 |
| IMAGE_READ off | −12.7% | −23.1% | −19.3% | −22.7% | −19.3% |
| Z_COMPARE off | −14.7% | −19.3% | −14.2% | −18.8% | −19.6% |
| Z_UPDATE off | −7.4% | −5.9% | −2.5% | −5.9% | −5.9% |
| Z clear removed + initialized to farthest (z_far: the first draw all passes; only overdraw within the frame fails) | −4.7% | −1.9% | −2.1% | −1.8% | −3.7% |
| Z clear removed + initialized to nearest (z_near: everything fails) | −19.0% | −20.7% | −14.8% | −20.5% | −20.0% |
| Triangle slopes (shade / texture / Z) set to 0 | −0.2% | −0.6% | −0.4% | −0.6% | −1.6% |
| 0F triangle → 09 (edge + Z, remaining 16 words are NOP) | +0.3% | +0.3% | +0.9% | +0.3% | +0.7% |
| Color buffer to the first 1 MB of RDRAM / Z 1 MB higher | −0.1% / 0 | −0.1% / 0 | −0.2% / 0 | −0.1% / 0 | 0 / — |

- **The mode bits are not the cause.** Neither the number of words in the triangle command nor the slopes of the attributes matter (reading the command words overlaps with drawing, and
  ares's "1.73 clk per NOP word" is an overestimate for triangle words).
- **Z: the z_far difference equals only the Z-clear Fill** (BAR 25.6K ≈ model 24.8K, DK64 44K). That is, pixels that fail due to overdraw within the frame
  save nothing (with z_near, where everything fails, it is −15 to 21% = 15 to 31 clk cheaper per span). If even one pixel in a span passes,
  all the reads and writes for that span are performed, so **the Z saving occurs only when a whole span (block) fails**. For ares's estimate of "mixing the pass/fail
  tables by the pass rate", unless there is a primitive that fails entirely, **a pass rate of 1.0** is the correct answer (the reason f=1 fit in v15).
- Only DK64 has AA (1 cycle) and ATOMIC_PRIM at −3.2% each (equivalent to 20 clk/triangle or 3.5 clk/span). For BAR (2 cycles) the AA effect is 0.
- Going from 2 cycles to 1 cycle changes things by only −1 to 3%: BAR's triangles are **memory-bound**, not pipe-bound.

### Y: synthetic triangles (2cyc_tex_blend_z, H=8, w=4/12/32, console clk/triangle)
| Case | w=4 | w=12 | w=32 |
|---|---|---|---|
| base (v22 W: vertical edges, random placement) | 709.7 (v23: 719.0) | 927.1 | 1350.4 |
| AA / TEX_PERSP / LOD / dither / point / cvg / BAR's mode words / slopes / x+0.5 | all base ±0.5% | | |
| Tilt all edges by 0.5 / 2 px/row | −1.3% / +3.5% | +1.1% / +1.8% | +0.4% / +0.9% |
| Real triangles (apex at top, mean width w) | −3.7% | −3.6% | −1.5% |
| Adjacent along the row (strip) | −4.8% | −1.9% | −1.4% |
| **All in the same place** | **−43.5%** | −39% | −31% |
| Set_Other_Modes + Set_Combine every time / every 8 | +0.5% / 0 | +0.4% / 0 | +0.1% / 0 |
| Sync_Pipe every time / Set_Tile+Size+Sync_Tile every time / Load_Block 64 B every 8 | +0.5% / −0.8% / +1.3% | +1.0% / +0.1% / +1.1% | +1.6% / +0.7% / +0.8% |
| Z all fail / Z_COMPARE off / Z_UPDATE off / IMAGE_READ off | −36% / −23% / −16% / −28% | −32% / −21% / −13% / −25% | −18% / −13% / −3% / −26% |
| **Color and Z in the same 1 MB** (adjacent, +300K) | 758.3 (**+10%**) | 1033.4 (**+17%**) | 1503.6 (**+17%**) |
| Color and Z in different 1 MB (+1M, as in BAR 0x200000/0x3DA800, +2M, +4M) | 688.5 | 885.6 | 1290.2 |

- RDRAM holds pages per 1 MB (bank), and **when the color and Z buffers are in the same 1 MB it is 10 to 17% slower**. Both games
  place them in different MBs (BAR 0x200000 / 0x3DA800, DK64 0x1D240 / 0x5D2E00), so there is no actual impact, which is also why `color_low` / `z_up1m` in X are 0.
  The base of v22/v23's W/Y had the arena straddling a 1 MB boundary, and so fell between these two values (709 to 719).
- The reason the game lists' triangles are more expensive than synthetic triangles is none of mode bits, commands, or placement; what remains is **the way the triangles are ordered itself**
  (strips sharing edges, where the next triangle immediately reads pixels written by the previous one = the read-after-write mentioned in the comments of ares's model).
  A synthetic strip (adjacent horizontally, non-overlapping) is conversely cheaper, so the overlap of shared edges needs to be reproduced synthetically, which could not be confirmed in the present series.
- In v23, on both consoles the RDP stopped responding at Y's `ci4_tlut` (Load_TLUT + CI4 tile), the timeout handling did not work, and execution halted (v24 also
  halts at the same place). The effect of CI4/TLUT is already known to be 0 from the game-list side (`om_no_tlut` ±0), so this case is dropped.

### Candidates applied to the game itself in the harness (`hw/pair_v24_candidates.txt`)
| Candidate | DK64 error / 2-3-4 | BAR seg0/seg1 (console 225/306) | Crash shatter/text | tasklog seg0 (console 230) |
|---|---|---|---|---|
| A: v7 | 26 / 29-425-46 | 251/333 | +641/+667 | 250 |
| B: + Z pass rate fixed at 1.0 | 31 / 26-423-51 | 250/332 | +641/+667 | 250 |
| C: B + ATOMIC 20 + 1cyc AA 3.5/span | 35 / 24-424-52 | 250/332 | +641/+667 | 250 |
| D: C + console edge walker | **59** / 9-400-76 ✗ | **229/301** | **+647**/+667 | **226** |
| E: D + v22 W's per-span costs | 25 / 35-418-47 | 250/332 | +638/+666 | 248 |

The console's edge walker (D) puts BAR almost exactly on the console, but it breaks DK64 because the span count of its small triangles goes up by +17%, and on the standalone lists
both BAR and DK64 are 8 to 12% too large, so it is merely covering for some other missing part in the game (rejected). Fixing the Z pass rate at 1.0 (B) increases
DK64's 4 VI (DK64's scenes contain small triangles that are entirely hidden, confirmed in v25/v28). What was finally put into 0012 is
"for whole-span Z failure, apply the estimated pass rate only to spans of 16 px or less, and always use the pass cost for wider spans" + ATOMIC_PRIM 20 + 1-cycle AA 3.5, giving
DK64 26 (26-426-48) / BAR 251 in the harness, and DK64 29 (24-423-53) / BAR 249/331 on the clean core (equivalent to v7; `hw/pair_znarrow.txt`, `hw/pair_v3_v8b.txt`).

## The DK64 in-game timeline (dk64_tasklog), replaying the demo's frames (v25), and replaying triangles by width (v26 to v28) (added 2026-11-01)

### dk64_tasklog: a resident logger on the DK64 (J) console
I put the same logger as bar_tasklog into DK64 (J) (`dk64_tasklog/`). Because the libultra version is different, I swapped in `osSpTaskLoad` 0x800085FC / `send_mesg` 0x80009984 /
the `osSpTaskStartGo` jal 0x800087B4 / the `osSpTaskYield` jal 0x800087D8, and in v2 I also hooked `osEPiRawStartDma` 0x8000C090 / `__osPiRawStartDma` 0x800048B0
to record PI DMA starts, sizes and interrupts. The logger resides at RAM 0x80160000, in ROM tail padding at 0x1FF0000, with the CIC-6105 checksum recomputed.
Recording starts at the attract demo start right after the title's still frames (60 VI or more with no graphics task). It is at console VI 12658 to 12662 versus ares 12644, so the two differ by
only 14 to 18 VI and can be compared directly by frame number (`hw/dk64_tasklog_hw_001` (v1), `_002` (v2, with PI)).

| Frames 423 to 520 from demo start | Console | ares v7 |
|---|---|---|
| Frame period 2 / 3 / 4+ VI | 30 / 462 / 25 (v1) | 32 / 450 / 36 |
| RSP completion (from start) | 0.656 to 0.667 VI | 0.679 to 0.699 |
| DP interrupt (from start) mean / median | 2.667 / 2.80 | 2.604 / 2.76 |
| DP completion of RDP-bound frames (185) | 2.903 | 2.877 (−1%) |
| Period of CPU-bound frames (136) | 3.225 | 3.294 (+2%) |
| PI DMA (per-frame mean) | 4.3 transfers / 7.8 KB / 0.100 VI (4.67 MB/s) | 4.3 transfers / 7.8 KB / 0.099 VI (4.73 MB/s) |
| Total VI (423 frames) | 1302 | 1312 (+0.8%) |

- The DK64 demo is RDP-bound ("wait for DP completion, then start the next one 0.13 VI later"), and **ares v7 has an RDP that is 1% faster and a CPU that is 2% slower, for a total of +0.8%**.
  The difference in the number of 4 VI frames is a matter of which way frames that sit on the 3 VI boundary tip, so **for DK64, v7 as it stands is equivalent to the console**.
- The EverDrive's PI DMA speed matches ares's model (the PI timing in 0007) exactly.
- The demo's frames are **400 thousand pixels / 14 thousand spans**, twice the size of dk64_797 (an intro frame, 210 thousand pixels) that I had been measuring in v15 and v22 to v24.

### v25: replaying the demo's frames themselves (VI off, console / ares v7)
| | d6052 | d6110 | d6253 | d6373 | d6513 | bar_2316 |
|---|---|---|---|---|---|---|
| Overall | 1.013 | 0.975 | 0.917 | 0.943 | 1.040 | 1.001 |
| Triangles | 1.005 | 0.957 | 0.886 | 0.917 | 1.033 | 0.997 |
| Texture load | 1.06 | 1.07 | 1.07 | 1.06 | 1.07 | 1.03 |
| In-game VI / VI off (console / ares) | **1.101 / 1.101** | 1.101 / 1.101 | 1.101 / 1.101 | 1.098 / 1.099 | 1.097 / 1.100 | 1.110 / 1.101 |
| z_near / ATOMIC_PRIM off / AA off | −12% / −1.5% / −0.5% | −12% / −1.5% / +0.7% | −16% / −1.5% / −0.6% | −16% / −1.9% / −0.6% | −14% / −1.9% / −0.5% | −20% / 0 / −0.2% |

On the NTSC console the VI is live, and **the game-VI-mode factor of 1.10 is as the model has it**. ATOMIC_PRIM is constant at 20 clk per triangle.

### v26 / v27: synthetic triangles with wide spans (64 to 320 px)
In BAR's light frame (bar_i2274), 43% of the pixels are in spans 128 to 320 px wide (the sky and the road surface), and the table extrapolates from measurements up to a width of 64 px
using constants for every 32 px. Measurements (clk/span, console / table):

| | w=32 | 64 | 128 | 192 | 256 | 318 | Per pixel (64→318) |
|---|---|---|---|---|---|---|---|
| 2cyc_tex_blend_z | 161/192 | 228/289 | 371/488 | 514/688 | 660/887 | 775/1037 (0.75) | 2.15 / 2.94 |
| 2cyc blend only | 108/120 | 171/181 | 299/307 | 427/432 | 555/558 | 667/668 (1.00) | 1.95 / 1.92 |
| 2cyc z only | 121/151 | 181/229 | 321/384 | 463/540 | 608/695 | 734/815 (0.90) | 2.18 / 2.31 |
| 1cyc_tex_blend_z | 147/182 | 210/294 | 334/513 | 459/731 | 585/951 | 665/1120 (0.59) | 1.79 / 3.25 |

At widths of 64 px and above, **the console's synthetic triangles are 25 to 40% cheaper than the table** (only for blend+Z modes), and this does not change with any of the following: the texture stride (1 / 4 / 1/4 texel/px),
perspective, I8/RGBA32, no texture, BAR's mode words, strips, or real triangles (v27).

### v28: replaying the game list's triangles split by maximum width (console / ares v7, triangles only, with Set commands (state) subtracted)
| | Width ≥64 px | 16 to 63 | <16 | tri_only overall |
|---|---|---|---|---|
| bar_i2274 (light) | **1.136** | 1.017 | 1.081 | 1.063 |
| bar_i2314 | 1.003 | 0.952 | 0.963 | 0.974 |
| bar_2316 | 1.049 | 0.969 | 0.981 | 0.997 |
| dk64_d6373 | 1.010 | 0.923 | **0.834** | 0.917 |

**In the game, wide triangles do not become cheap the way synthetic ones do** (1.00 to 1.14), and in the light frame they are on the contrary +14%. In DK64's heavy frames the thin triangles are −17%
(small triangles that are entirely hidden, where Z makes the whole span fail; consistent with z_near −16%). In other words, the cost of a triangle has a **scene-content-dependent ±10 to 15%**
(how things overlap, drawing order) that cannot be explained by the factors that vary across synthetic triangles (width, height, mode,
texture, placement, commands, buffer layout). In BAR's heavy frames and in DK64 it cancels out and lands inside the v7 table, whereas in BAR's light frame it stacks up in the same direction. There is
no solution that fits both at once with a factor per width or per type (the factors in the table above do not point the same way), and closing this gap requires a pixel-level RDP simulation (a memory access
model equivalent to angrylion).

### series v8
I put in three points confirmed on the console as 0012 (within error on the DK64 / BAR game bodies, no regression from v7):
1. The Z pass-rate estimate (H(n)/n from the previous frame's overdraw) is dropped in favour of a **fixed 1.0** (overdraw within a frame saves nothing).
2. **+20 clk for ATOMIC_PRIM triangles**.
3. **+3.5 clk/span for AA_EN in 1-cycle mode** (0 in 2-cycle).

## Changes (patches 0001 to 0012, harness 9999)

(`0001` is an independent bug fix: in `ares/n64/rdp/io.cpp` `RDP::readWord`, the DPC_TMEM_BUSY check was written as `if(data == 7)`, a typo that made the
TMEM busy counter unreadable; it is corrected to `if(address == 7)`.)

Files changed by `0002` (the RDP model): `ares/n64/rdp/timing.cpp` (new), `ares/n64/rdp/timing-model.hpp` (new, the console-measured table),
`rdp.hpp`, `rdp.cpp`, `render.cpp`, `io.cpp`, `serialization.cpp`, `vulkan/vulkan.cpp`, `system/serialization.cpp` (state version v154),
`n64/CMakeLists.txt`. Files changed by `0003` (CPU stalls): `ares/n64/cpu/cpu.hpp`, `cpu.cpp`, `dcache.cpp`, `recompiler.cpp`.
All of them have been confirmed to apply as-is to upstream master (a774671d, as of 2026-10-26) (0001 to 0012 with `git am`, 9999 with `git apply --check`).
`0012` is the model from the v24/v25/v28 sections (Z makes the whole span fail only for spans of 16 px or less, ATOMIC_PRIM at 20 clk/primitive, 1-cycle AA at 3.5 clk/span,
and `zFailWidth` / `atomicCost` / `aaSpanCost` in `rdp.hpp`), and `9999` is the verification harness (a fixed number, so it does not change from series to series).
`0007` is the PI timing fix that gets past the MGC 2011 demo's emulator detection (`n64: make cartridge ROM ignore PI accesses faster than a mask ROM can serve`, in `cartridge.hpp` and `pi/device.hpp`), `0008`/`0009` are the models from the v17 to v19 sections above, `0010` is the v20 section's, and `0011` is the v20b section's.

1. **RDP execution time model** (`timing.cpp` + `timing-model.hpp`)
   - On both the parallel-rdp path and the software path, it parses the same command stream that is passed to the renderer.
   - The cycle count per command uses the table from the **console-measured model (rdp_timing v3 to v6, NTSC/PAL, read via `DPC_CLOCK`)**
     as is (`rdp_timing_model.h` is bundled as `timing-model.hpp`):
     - Triangles / rectangles: `prim_overhead(mode, max width)` (plus +3 for shaded/textured triangles) and, per scanline,
       `span(mode, RDRAM address of the first pixel, width)`. A triangle's spans are computed per row from the edge factors (with scissor applied),
       and the address is computed from the Set_Color_Image address, width and depth (the phase within a 64-byte block matters).
     - Mode: one of the table's 26 mode rows is selected from cycle type x color depth x imageRead x zCompare/zUpdate.
       For 4/8bpp color buffers and 32bpp copies there are no measurements, so the 16bpp table is reused with a byte-count conversion.
     - Z test: the table has two series, "all pixels pass" and "all pixels fail", so it is interpolated by pass rate.
       The pass rate is estimated as H(n)/n from the **previous frame's overdraw** (Z-tested pixel count / scissor area = n)
       (the sum of the probability 1/k that the k-th layer passes when n layers are drawn in random order) (1 layer = 1.0, 2 layers = 0.75, 3 layers ≈ 0.6).
       From 0012 on, this estimate is applied **only to spans of 16 px or less** (on the console a partial per-pixel Z fail saves nothing,
       and it is cheaper only when the whole span is hidden: v24/v28).
       Compare-only / update-only scale the 16bpp 1-cycle measured row by the zpass ratio at the same width.
     - Load_Block / Load_Tile (8B word count and 64B block count from each row's RDRAM address) / Load_TLUT,
       Sync_Pipe/Tile/Load 50/33/25, other Set commands 2.62 (1.73 for lists from DMEM),
       and a base list cost of 93 per Sync_Full.
   - The table is measured with "stacked drawing, CPU/RSP almost stopped", so three corrections measured on the console are applied (previous section):
     spans that read color/Z get `spanPenalty`(40) added and are multiplied by `contentionZ`(1.13), and during VI scanout are further multiplied by `viFactor`(1.07);
     write-only spans and TMEM loads are multiplied by `contentionPlain`(1.19).
2. **The RDP thread consumes work**: `RDP::main()` consumes pending work in step with the clock (stopped while FREEZE is set).
   Sync_Full is queued, and when the work up to that point has completed it raises the DP interrupt and clears the busy flag.
3. Added state to the save state and updated the version to v154.
4. **CPU stalls** (`0003`): while the RDP model has unconsumed work and FREEZE is not set, +10 cycles are added to CPU icache fills,
   +4 to dcache fills, and +12 to dcache writebacks (`CPU::rdpBusyStall`). A call to `CPU::icacheStall()` was added to the
   icache fill path that the recompiler inlines. If the RDP model is disabled, the CPU side also behaves as before.

### Applying and building

```sh
git clone https://github.com/ares-emulator/ares.git && cd ares
git am 0001-n64-fix-DPC_TMEM_BUSY-register-read.patch        # io.cpp readWord: if(data == 7) → if(address == 7)
git am 0002-n64-model-RDP-execution-time-so-frame-pacing-matches.patch
git am 0003-n64-stall-CPU-cache-line-transfers-while-the-RDP-is-.patch
git am 0004-n64-RDP-timing-VI-mode-factors-per-transfer-bus-cont.patch
git am 0005-n64-CPU-instruction-timing-from-hardware-cpu_timing-.patch
git am 0006-n64-SP-DMA-timing-from-hardware-rsp_timing-v16.patch
git am 0007-n64-make-cartridge-ROM-ignore-PI-accesses-faster-tha.patch   # MGC 2011 emulator detection (PI timing)
git am 0008-n64-CPU-write-buffer-FPU-operand-costs-and-cache-op-.patch   # v18/v17
git am 0009-n64-SP-DMA-slowed-by-the-VI-and-by-CPU-line-transfer.patch   # v19/v19b
git am 0010-n64-DPC_CURRENT-follows-the-modelled-RDP-progress-so.patch   # v20: RSP waits for the RDP on the FIFO
git am 0011-n64-SP-DMA-and-CPU-bus-contention-against-the-RDP-re.patch   # v20b: re-measurement of the time SP DMA / CPU fills take from the RDP
git am 0012-n64-depth-test-savings-only-for-short-spans-ATOMIC_P.patch   # v24/v25/v28: depth-test savings only for spans up to 16 px, ATOMIC_PRIM, 1-cycle AA
# Build as usual (cmake -B build && cmake --build build)
```

`9999` is the headless verification harness (`headless/`, plus counting, dumping and environment-variable knobs on the core side), and is not meant for upstream (apply it on top of 0012 with `git apply`).
It removes the GUI (hiro/desktop-ui) from the top-level CMake and links only the N64 core, and
`headless <rom> <number of VI frames> --watch ADDR[:fmt] --save/--load/--poke ...` lets you observe RAM
(it is for analysis in a Linux environment without a GUI).

## Caveats and remaining issues

- (added 2026-11-01, overall status) **The DK64 demo is within 1 to 2% of the console with series v7/v8** (dk64_tasklog: RDP-bound frames −1%, CPU-bound frames +2%,
  total +0.8%, and the PI DMA speed is the same). **The BAR intro is still at seg0 249 vs 230 frames (ares is 8% faster)**, and the remainder is
  (1) the part that only appears in-game (RSP DMA contention, FIFO coupling, CPU contention, together +2 to +4%), and (2) **the ±10 to 15% "scene content" dependence of the triangle cost**
  (v28: determined by overlap and drawing order, and not explained by width, mode, texture, placement or commands. It cancels out in BAR's heavy frames and in DK64, and
  stacks up in the same direction in BAR's light frame). (2) cannot be closed with the table, and requires a pixel-level RDP simulation (a memory access model equivalent to
  angrylion). Any change that fits BAR, such as a uniform factor or the console's edge walker, breaks DK64 (the v24 candidate table).
- (added 2026-10-31) BAR intro: the mechanism was established in v20 (the RSP waits for the RDP on the FIFO → patch 0010). With v20b, the RSP+RDP of frame 2316 got as close as
  ares 1.553 against the console's 1.587 VI (−2%, versus −3.4% for v6), but the intro as a whole is still 249 vs 225 frames (+11%). That is more than the per-frame
  RSP/RDP time difference (0.03 to 0.08 VI) can explain, and the remainder lies **on the CPU side (game logic), or in the ordering of the per-frame RSP tasks (including yields
  caused by audio tasks)**. What is needed next is a ROM that measures on the console "how many VI the CPU uses per frame" (the time from the SP interrupt to the next task start),
  or a ROM that dumps and replays BAR's CPU processing itself.
- v20b revealed a difference between the two consoles: under VI game mode, the time the RSP's DMA takes from the RDP is 71 clk/DMA on the NTSC console and 47 on the PAL console (51 on both with VI off),
  and the RDP-alone VI factor is also NTSC +11.3% / PAL +18.6%. The model leans towards the NTSC console (factor x1.28, +11.7%).
- `tex` (lists containing texture loads) + VI game mode + CPU misses + continuous 3.5 KB SP DMA makes the console's RDP stall (reproduced on both consoles, v19c).
- The VI factor is the NTSC console's value. The PAL console was 5 to 15% heavier in the same modes, so fitting a PAL ROM on a PAL console would need PAL values for `viFactor*`.
- RCP register reads are uniformly 23 (only MI is 11), and uncached RDRAM reads are 32. The FPU fast paths (0.0 / 1.0 / 2^n operands) are not modelled (the normal value is charged).
- The VI factor is the NTSC console's value (on the PAL console the VI's effect is 5 to 26% larger: v15/v19).

- The calibration is based on one scene (Jungle Japes) recorded from the console in this demo. The same factors are not necessarily optimal for other scenes or other titles.
  If a similar 60fps recording is available, the same procedure can verify it with `analyze.py` (recording → display interval series) and `score.py` (block comparison with the ares log).
- Because RDP time goes from 0 to realistic values, **there may be an effect on the frame rate of other titles**
  (especially 60fps titles with a high RDP load). I only have this ROM on hand, so other titles are unverified.
  If problems appear, adjust `spanPenalty`/`contentionZ`/`contentionPlain` in the `Timing` struct in `rdp.hpp` and the
  `*Stall` constants in `cpu.hpp` (all of them are console-measured values, and no free parameters are left).
- In this environment Vulkan (parallel-rdp) could not be initialized (SwiftShader does not support 16-bit storage),
  so I could not check the screen visually. Verification was done with the coordinates and state transitions in RAM (Y oscillation in the rope section, cannon launch, map transition to the next demo).
- The same mechanism should apply to the US/PAL versions (with different addresses), but this is unverified. PAL is 50Hz, so the delta threshold (20ms) differs,
  and the calibration may fit differently.

## References

- [ares #1652](https://github.com/ares-emulator/ares/issues/1652), [ares #1175](https://github.com/ares-emulator/ares/issues/1175)
- [simple64 v2022.07.7 release notes](https://github.com/m64p/m64p/releases/tag/v2022.07.7), [simple64 #173 "DK64 runs too well"](https://github.com/simple64/simple64/issues/173)
- [Donkey Kong 64: Recompiled](https://github.com/Rainchus/Donkey-Kong-64-Recompiled) (`patches/patches_main.c`, `patches/timing_fixes.c`)
- [n64brew: Reality Display Processor/Pipeline](https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline)
- [wScottSh/ares Issue #1 (research notes on a hardware-accurate timing model)](https://github.com/wScottSh/ares/issues/1)
