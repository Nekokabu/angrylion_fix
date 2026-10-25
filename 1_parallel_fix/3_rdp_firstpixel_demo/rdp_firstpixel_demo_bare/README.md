# rdp_firstpixel_demo (bare-metal build)

This is the same test as `rdp_firstpixel_demo`, built **without the
libdragon runtime**.

Current libdragon ROMs do not run in mupen64plus 2.6.0 or in Rosalie's Mupen
GUI. libdragon uses the LLD/SCD instructions and 64-bit (XKPHYS) addresses
for cartridge access, which mupen64plus does not emulate, so the ROM crashes
at start-up. This build avoids both, so it runs there, for example with
angrylion-rdp-plus, which is where the parallel-mode bug shows.

## How it differs from the libdragon build

- It boots with libdragon's **compatibility IPL3** (`ipl3_compat.z64`,
  public domain, signed for CIC 6102). The IPL3 loads the flat binary to
  0x80000400.
- Start-up, VI set-up, RDP command lists and the 8x8 text font (taken from
  libdragon, public domain) are all in this folder. Nothing else is linked.
- The screen layout, the bands, the zooms and the counters are identical to
  the libdragon build. The title line includes "(BARE)" and the TV type.
- The VI uses libdragon's register values, with X_SCALE 0x201.
  - With interpolation off (AA mode 3) on a 16-bit framebuffer, the real
    VI glitches when h_start is below 128 and X_SCALE is 0x200 or less.
  - NTSC's h_start is 108, so an earlier version with X_SCALE 0x200 showed
    a broken picture on NTSC consoles. PAL (h_start 128) was fine.
  - angrylion-rdp-plus models this glitch in current master, but not in the
    1.6 release, which only shows a warning.
- The ROM tells the PIF that boot has finished, as commercial games do.

## Results in mupen64plus with angrylion-rdp-plus

These results are without any emulator patches, using the cached
interpreter, at frame 150.

| Renderer | Band A wrong rows | Band B wrong rows |
|---|---|---|
| `Parallel = False` | 0 | 0 |
| parallel, 2 workers (original 9c8b9ed) | 80 / 80 | 80 / 80 |
| parallel, 2 workers (with the fix) | 0 | 0 |

On real NTSC and PAL consoles, the ROM shows 0 wrong rows in both bands.

If an emulator's dynamic recompiler fails to boot it, switch the CPU core to
an interpreter.

## Building

```sh
make            # needs mips64-elf-gcc from the libdragon toolchain ($N64_INST) and python3
```
