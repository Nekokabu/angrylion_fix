# angrylion-rdp-plus: parallel mode 2-cycle first-pixel fix

This bundle holds everything for the pull request against
[ata4/angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus): the
patch, the pull request text, the test ROMs (source and builds), and the
results from real NTSC and PAL consoles.

All ROMs drive the RDP directly through the DP registers, with no RSP
microcode. They build with libdragon (preview branch): run `make` in each
folder.

| Path | What it is | Supports |
|---|---|---|
| `1_patch/0001-…patch` | The fix, against commit `9c8b9ed`. Apply with `git am`. | — |
| `2_PR_description.md` | Pull request text: problem, cause, fix, testing, hardware findings. | — |
| `3_rdp_firstpixel_demo/` | Visual test ROM. Two blended bands with memory selected in both blender cycles; zoomed left edges; on-screen count of wrong rows. | The bug is visible, and the fix removes it. Real NTSC and PAL show 0 wrong rows. |
| `3_rdp_firstpixel_demo/rdp_firstpixel_demo_bare/` | The same demo without the libdragon runtime: compatibility IPL3 and a flat binary. Use it in mupen64plus 2.6.0, Rosalie's Mupen GUI and other emulators that cannot run current libdragon ROMs. | The same, in the emulators where the bug is. Real NTSC and PAL show 0 wrong rows. |
| `3_rdp_firstpixel_demo/rdp_firstpixel_demo/rdp_firstpixel_demo_before_after.png` | Screenshots side by side: the original parallel mode (left) and the fixed one (right). | — |
| `3_rdp_firstpixel_demo/hardware/ntsc.mp4`, `pal.mp4` | The final demo on real consoles: 0 bad frames in both bands. | Fixed renderer = hardware. |
| `3_rdp_firstpixel_demo/hardware/first_version_*.mp4` | The first demo version, without the invisible primitive after the background. Band A had one wrong row per frame because of the fill mode effect. | The fill mode finding. |
| `4_rdp_timing_v3/rdp_timing/` | RDP measurement ROM: 462 cases and 88 framebuffer dumps, plus analysis tools (`tools/rdp_fb_diff.py`, `tools/rdp_timing_report.py`) and angrylion reference output. | Before the fix, parallel mode differs from hardware in 9–12 cases. After the fix it is byte-identical to `Parallel = False` and to hardware in all 1-cycle and 2-cycle cases. |
| `4_rdp_timing_v3/hardware/` | Results from real consoles. `rdp_timing_005*` is NTSC without the Expansion Pak, `rdp_timing_006*` is PAL. Each has a summary, raw runs and framebuffer dumps (`_fb/`). The NTSC and PAL dumps are byte-identical. | The same. |
| `5_rdp_carry_probe/v1/` | Probe ROM. A blender passes the first cycle's memory input through, so every pixel shows the address code of the pixel read before it. 15 tests. | How the carried value is chosen on hardware: previous row, previous primitive, Sync_Full, mode changes, 1-cycle read, triangles both ways. It matches angrylion. |
| `5_rdp_carry_probe/v5/` | Probe ROM for the fill mode effect. It varies the probe's geometry. | After fill mode, the carried value is sometimes the pixel at (right edge − 13, last row) on 16-bit, and the constant `000C00E0` on 32-bit. |
| `5_rdp_carry_probe/v6/`, `v7/` | Probe ROMs, final versions for the fill mode effect: probe widths 1–64, triangles, 1-cycle, primitives before and after the probe, copy mode, split lists. | The first pixel of the first 2-cycle primitive after fill or copy mode gets the colour that was in the pipeline 12 pixels before the end of the list's last 1/2-cycle primitive. |
| `5_rdp_carry_probe/hardware/` | Results from all probe versions on real consoles. File names: `vN_<TV>[_<RAM>]_<run>.txt`. | See the table below. |

## What each probe version showed

| Version | Finding |
|---|---|
| v1 | Every carry-over rule matches angrylion. The only exception is after fill mode, where about 1 run in 16 carries an odd value. |
| v2 | The odd value after fill mode does not depend on fill colour, fill count or the commands in between (even Sync_Full). It is constant per framebuffer depth. |
| v3 | The 32-bit value `000C00E0` exists nowhere in RDRAM. The 16-bit value is a real memory read. |
| v4 | The 16-bit read is relative to the framebuffer, always at the same pixel. 8 MB consoles behave the same as 4 MB ones. |
| v5 | That pixel is at (right edge − 13, last row of the primitive), for every x position, width, height and y position tested. |
| v6 | Only that one pixel changes. "Right edge − 13" is really "12 pixels before the last pixel", counting back across rows. Copy mode does the same as fill mode; 1-cycle primitives never show it; it survives an idle RDP. With two 2-cycle primitives after the fill, the first one gets the second one's value. |
| v7 | The source is the end of the command list: a primitive drawn after the probe becomes the source (a 2-cycle one without image read gives its own colour; fill mode does not count). Rows of 4–7 pixels give (x0 + 3, last row − 2). Rates: 1–4% (PAL), 2–15% (NTSC), with no pattern between runs. |

The ROMs of v2–v4 are not included. Their source and builds are available
on request.

## Not covered here

`rdp_timing` v3 also found an unmodelled load hazard: `Set_Texture_Image` or
`Set_Tile` within about 4–8 commands after `Load_Tile`, with no sync, affects
the tail of the load. This is unrelated to the fix and is not part of the
pull request. The data is in the `rdp_timing` hardware dumps, cases 425–446.

Made by Nekokabu.
