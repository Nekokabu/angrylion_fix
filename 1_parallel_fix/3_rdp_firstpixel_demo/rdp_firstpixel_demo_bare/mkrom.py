#!/usr/bin/env python3
"""Build the ROM: libdragon compatibility IPL3 (4 KiB, public domain) + flat binary.

    mkrom.py ipl3_compat.z64 program.bin out.z64 "TITLE"
"""
import struct, sys
ipl3, binf, out, title = sys.argv[1:5]
rom = bytearray(open(ipl3, 'rb').read()[:0x1000])
prog = open(binf, 'rb').read()
rom[0x08:0x0C] = struct.pack('>I', 0x80000400)            # entry point
rom[0x10:0x14] = struct.pack('>I', (len(prog) + 15) & ~15) # bytes the IPL3 loads
rom[0x20:0x34] = title.encode('ascii')[:20].ljust(20, b' ')
rom += prog
rom += b'\0' * ((1 << 20) - len(rom) % (1 << 20) if len(rom) % (1 << 20) else 0)   # pad to 1 MiB
open(out, 'wb').write(rom)
