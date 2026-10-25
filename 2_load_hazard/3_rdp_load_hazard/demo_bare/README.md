# rdp_load_hazard_demo (bare-metal build)

A visual test ROM for the RDP load hazard. It runs without the libdragon
runtime, so it also works in emulators that cannot run current libdragon
ROMs (mupen64plus 2.6.0, Rosalie's Mupen GUI). It boots with libdragon's
public-domain compatibility IPL3.

## What it shows

On real hardware, the last TMEM writes of a load are still pending when the
next command arrives. A command that changes the load tile or the texture
image soon after `Load_Tile`, with no sync in between, changes the end of the
load.

Each panel clears TMEM to red, loads a 32×16 RGBA16 texture with
`Load_Tile`, sends GAP no-ops, then the change. The images show texture rows
8–15, magnified.

| Row | Change | Real hardware |
|---|---|---|
| 1 | `Set_Tile` on the load tile, with the TMEM address moved by 2 KiB. Top image: the original address. Bottom image: the new address. | At gaps 0, 4 and 8, the end of the load goes to the new address: the top image ends in red, the bottom one shows the moved texels. At gap 16, nothing moves. |
| 2 | `Set_Texture_Image` with a second (orange) texture | At gaps 0 and 2 the last row is orange. At gaps 4 and 8 it is not. |

Under each panel: HAZARD or NONE, and `=HW` (green) or `DIFF` (red)
depending on whether it matches real hardware. The bottom line counts
matching panels: real hardware shows 8/8.

An emulator that completes every load at once shows NONE everywhere and
3/8. `rdp_load_hazard_demo_before_after.png` shows the original
angrylion-rdp-plus (left) and the version with the load hazard change
(right).

## Building

Needs only the libdragon toolchain (`mips64-elf-gcc`) in `$N64_INST` and
python3. Run `make`.

Made by Nekokabu.
