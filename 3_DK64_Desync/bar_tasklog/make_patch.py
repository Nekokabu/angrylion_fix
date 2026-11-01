#!/usr/bin/env python3
"""Patch Beetle Adventure Racing (J) with the resident timing logger.

usage: make_patch.py bar_j.n64 hook.bin out.n64   (big-endian .z64/.n64 image, 16 MiB)
The user patches their own dump with this script; the ROM size stays 16 MiB (the logger sits in the padding at 0xFF0000).
"""
import struct, sys

CIC6102_SEED = 0xF8CA4DDC

def rol(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF if n else v

def checksum_6102(rom):
    t1 = t2 = t3 = t4 = t5 = t6 = CIC6102_SEED
    words = struct.unpack('>%dI' % 0x40000, rom[0x1000:0x101000])
    for i, d in enumerate(words):
        if ((t6 + d) & 0xFFFFFFFF) < t6: t4 = (t4 + 1) & 0xFFFFFFFF
        t6 = (t6 + d) & 0xFFFFFFFF
        t3 ^= d
        r = rol(d, d & 0x1F)
        t5 = (t5 + r) & 0xFFFFFFFF
        if t2 > d: t2 ^= r
        else: t2 ^= t6 ^ d
        t1 = (t1 + (t5 ^ d)) & 0xFFFFFFFF
    return (t6 ^ t4 ^ t3) & 0xFFFFFFFF, (t5 ^ t2 ^ t1) & 0xFFFFFFFF

def main():
    src, hook, dst = sys.argv[1:4]
    rom = bytearray(open(src, 'rb').read())
    if rom[:4] == b'\x37\x80\x40\x12':      # byte-swapped (.v64)
        rom[0::2], rom[1::2] = rom[1::2], rom[0::2]
    elif rom[:4] == b'\x40\x12\x37\x80':    # little-endian (.n64)
        rom = bytearray(b''.join(rom[i:i+4][::-1] for i in range(0, len(rom), 4)))
    if rom[:4] != b'\x80\x37\x12\x40':
        sys.exit('not an N64 ROM image (first bytes %s)' % bytes(rom[:4]).hex())
    if rom[0x3b:0x3f] != b'NB8J':
        sys.exit('this is not Beetle Adventure Racing (J) (game code %r)' % bytes(rom[0x3b:0x3f]))
    if len(rom) != 0x1000000:
        sys.exit('expected a 16 MiB image')
    # the libultra functions we hook, as they sit in the ROM (boot segment: ROM 0x1000 -> RAM 0x80000400)
    def romw(ram): return struct.unpack('>I', rom[ram - 0x80000400 + 0x1000:][:4])[0]
    expect = {0x80005B8C: 0x27BDFFD8, 0x80005B90: 0xAFBF0024, 0x8000629C: 0x3C0A8003, 0x800062A0: 0x254A0C70,
              0x80005D1C: 0x0C002764, 0x80005D20: 0x24040125, 0x80007868: 0x0C002764, 0x8000786C: 0x24040400,
              0x80000400: 0x3C088002, 0x8000042C: 0x01400008}
    for a, v in expect.items():
        if romw(a) != v: sys.exit('unexpected code at %08X: %08X (another revision of the game?)' % (a, romw(a)))
    # entry stub: copy the 4 KiB logger from ROM 0xFF0000 (inside the ROM's 0xFF padding) to RDRAM 0x700000 with
    # uncached CPU loads/stores (no PI DMA, no cache state involved), then jump to it
    stub = [0x3C08B0FF,  # lui t0,0xb0ff        src  0xB0FF0000
            0x3C09A070,  # lui t1,0xa070        dst  0xA0700000
            0x340A0400,  # ori t2,zero,0x400    1024 words
            0x8D0B0000,  # lw  t3,0(t0)
            0x25080004,  # addiu t0,t0,4
            0x254AFFFF,  # addiu t2,t2,-1
            0xAD2B0000,  # sw  t3,0(t1)
            0x1540FFFB,  # bnez t2,-5
            0x25290004,  # addiu t1,t1,4        (delay slot)
            0x081C0000,  # j 0x80700000
            0x00000000,
            0x00000000,
            0x00000000]
    rom[0x1000:0x1034] = struct.pack('>13I', *stub)
    blob = open(hook, 'rb').read()
    assert len(blob) <= 0x1000
    if any(b != 0xFF for b in rom[0xFF0000:0x1000000]): sys.exit('the end of the ROM is not padding')
    rom[0xFF0000:0xFF0000 + len(blob)] = blob
    c1, c2 = checksum_6102(rom)
    rom[0x10:0x18] = struct.pack('>II', c1, c2)
    rom[0x20:0x34] = b'BEETLE TASKLOG      '
    open(dst, 'wb').write(rom)
    print('wrote %s (%d bytes), checksum %08X %08X' % (dst, len(rom), c1, c2))

if __name__ == '__main__':
    main()
