# angrylion-rdp-plus: RDP load hazard emulation

Everything for the pull request against
[ata4/angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus): the
patch, the pull request text, the test ROMs (source and builds), the results
from real NTSC and PAL consoles, and the model used to check them.

| Path | What it is |
|---|---|
| `1_patch/0001-…patch` | The change, against commit `9c8b9ed`. Apply with `git am`. Independent of the parallel-mode fix; the two merge without conflicts. |
| `2_PR_description.md` | Pull request text: hardware behaviour, the change, testing, limitations. |
| `3_rdp_load_hazard/v1`–`v5` | Test ROMs (libdragon, preview branch; `make` in each folder). Each README lists what that version measures. |
| `3_rdp_load_hazard/demo_bare/` | Visual demo ROM, bare-metal (also runs in mupen64plus 2.6.0 and RMG). Real hardware: 8/8; an emulator without the hazard: 3/8. |
| `4_hardware/` | Console results. `v1_PAL`, `v1_NTSC`: TMEM dumps per case (identical). `vN_<TV>.txt`: v2–v5 results. |
| `5_tools/sti_model.py` | The Set_Texture_Image model in Python. `python3 sti_model.py 4_hardware/v4_NTSC.txt 4_hardware/v5_NTSC.txt` checks it against every scan. |
| `5_tools/compare_with_hardware.py` | `compare_with_hardware.py N HW.txt EMU.txt` compares an emulator run of version N with the console's results. |

## What each version found

| Version | Finding |
|---|---|
| v1 | `Set_Tile` and `Set_Texture_Image` right after a load, with no sync, change the tail of the load. Any sync prevents it. |
| v2 | The reach is counted in commands, not time (RDRAM and DMEM lists give the same result). The 15-slot queue and the row layout `max(words + 1, 9)`. |
| v3 | State commands retire one slot each; syncs, draws and loads complete the queue. An idle RDP completes it after 10–100 µs. STI depends on RDRAM alignment. |
| v4 | STI: the previous row's alignment decides (exact rule for rows up to 64 bytes). The idle RDP completes the queue about 30 µs after the load ends. |
| v5 | STI rule for rows of 72–128 bytes. The idle edge is 30–33 µs after the load, with about ±1.5 µs of jitter. |

Made by Nekokabu.
