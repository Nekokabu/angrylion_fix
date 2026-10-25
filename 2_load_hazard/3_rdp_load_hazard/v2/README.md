# rdp_load_hazard v2

A diagnostic N64 ROM: a timing model of the RDP load hazard.

## v1 results (NTSC and PAL consoles, identical and deterministic)

**Set_Texture_Image shortly after Load_Tile.** Rows whose DRAM read has not
started yet are read from the new address:

- With 32-byte rows, the last row is affected up to 7 no-ops after the load.
- With 16-byte rows, two rows are affected; with 128-byte rows, none.

**Set_Tile on the load tile shortly after Load_Tile, Load_Block or
Load_TLUT.** The remaining 8-byte TMEM writes go to the new TMEM address. A
32-byte-row Load_Tile shows this up to 12 no-ops after the load, and the
number of moved bytes drops by 8 per no-op.

## Method

Each scan runs 0 to 40 no-ops between the load and the change:

| Load | Change | Sizes |
|---|---|---|
| Load_Tile | Set_Texture_Image (new address) | 16 rows of 8, 16, 24, 32, 48, 64, 96, 128 bytes |
| Load_Tile | Set_Tile on the load tile (TMEM +1 KiB) | the same sizes |
| Load_Block | Set_Tile on the load tile | 128 B to 4 KiB |
| Load_TLUT | Set_Tile on the load tile | 16, 64, 256 entries |

- **Command source.** Every scan runs twice: once with the command list in
  RDRAM, and once in RSP DMEM (XBUS), where commands are fetched faster. If
  the reach (the largest gap that still shows the effect) changes between
  the two, the effect depends on time, not on the number of commands.
- **Cycle counts.** The ROM measures each load on its own and the cost of a
  no-op from each source, so command counts can be turned into RDP cycles.
- **Comparison.** For every run, the console compares the TMEM read-back
  with the same load done with a sync before the change.

## Output

- **Screen:** one line per load, with the reach from RDRAM (R) and from
  DMEM (D). -1 means no effect.
- **SD card:** `sd:/rdp_load_hazard/v2_NNN.txt` holds the no-op and load
  cycle counts, and one line per gap scan step: differing bytes and the
  first and last differing TMEM address.

## Building

The ROM needs libdragon (preview branch) and its toolchain in `$N64_INST`.
Run `make`.
