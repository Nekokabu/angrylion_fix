#!/usr/bin/env python3
"""Patch Donkey Kong 64 (J) with the resident timing logger.

usage: make_patch.py dk64j.z64 hook.bin out.z64   (.z64/.n64/.v64 image, 32 MiB)
The user patches their own dump with this script; the ROM size stays 32 MiB (the logger sits in the padding at 0x1FF0000).
"""
import struct, sys

CIC6105_SEED = 0xDF26F436

def rol(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF if n else v

def checksum_6105(rom):
    t1 = t2 = t3 = t4 = t5 = t6 = CIC6105_SEED
    words = struct.unpack('>%dI' % 0x40000, rom[0x1000:0x101000])
    tab = struct.unpack('>64I', rom[0x40 + 0x710:0x40 + 0x710 + 0x100])
    for k, d in enumerate(words):
        i = 0x1000 + k * 4
        if ((t6 + d) & 0xFFFFFFFF) < t6: t4 = (t4 + 1) & 0xFFFFFFFF
        t6 = (t6 + d) & 0xFFFFFFFF
        t3 ^= d
        r = rol(d, d & 0x1F)
        t5 = (t5 + r) & 0xFFFFFFFF
        if t2 > d: t2 ^= r
        else: t2 ^= t6 ^ d
        t1 = (t1 + (tab[(i & 0xFF) // 4] ^ d)) & 0xFFFFFFFF
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
    if rom[0x3b:0x3f] != b'NDOJ':
        sys.exit('this is not Donkey Kong 64 (J) (game code %r)' % bytes(rom[0x3b:0x3f]))
    if len(rom) != 0x2000000:
        sys.exit('expected a 32 MiB image')
    c1, c2 = checksum_6105(rom)
    if rom[0x10:0x18] != struct.pack('>II', c1, c2):
        sys.exit('the CIC-6105 checksum of this image does not match its header (%s): a bad dump?' % rom[0x10:0x18].hex())
    # the libultra functions we hook, as they sit in the ROM (boot segment: ROM 0x1000 -> RAM 0x80000400)
    def romw(ram): return struct.unpack('>I', rom[ram - 0x80000400 + 0x1000:][:4])[0]
    expect = {0x800085FC: 0x27BDFFE0, 0x80008600: 0xAFBF0014,   # osSpTaskLoad
              0x80009984: 0x3C0A8001, 0x80009988: 0x254A3E30,   # send_mesg (exception handler)
              0x800087B4: 0x0C001788, 0x800087B8: 0x24040125,   # osSpTaskStartGo: jal __osSpSetStatus(0x125)
              0x800087D8: 0x0C001788, 0x800087DC: 0x24040400,   # osSpTaskYield: jal __osSpSetStatus(0x400)
              0x8000C090: 0x27BDFFE0, 0x8000C094: 0x3C03A460,   # osEPiRawStartDma (v2: PI DMA starts)
              0x800048B0: 0x27BDFFD8, 0x800048B4: 0xAFBF001C,   # __osPiRawStartDma
              0x80000400: 0x3C088001, 0x80000404: 0x25080A20, 0x8000042C: 0x01400008, 0x80000430: 0x27BD1820}
    for a, v in expect.items():
        if romw(a) != v: sys.exit('unexpected code at %08X: %08X (another revision of the game?)' % (a, romw(a)))
    # entry stub: copy the 4 KiB logger from ROM 0x1FF0000 (inside the ROM's 0xFF padding) to RDRAM 0x160000 with
    # uncached CPU loads/stores, then jump to it
    stub = [0x3C08B1FF,  # lui t0,0xb1ff        src  0xB1FF0000
            0x3C09A016,  # lui t1,0xa016        dst  0xA0160000
            0x340A0400,  # ori t2,zero,0x400    1024 words
            0x8D0B0000,  # lw  t3,0(t0)
            0x25080004,  # addiu t0,t0,4
            0x254AFFFF,  # addiu t2,t2,-1
            0xAD2B0000,  # sw  t3,0(t1)
            0x1540FFFB,  # bnez t2,-5
            0x25290004,  # addiu t1,t1,4        (delay slot)
            0x08058000,  # j 0x80160000
            0x00000000,
            0x00000000,
            0x00000000]
    rom[0x1000:0x1034] = struct.pack('>13I', *stub)
    blob = open(hook, 'rb').read()
    assert len(blob) <= 0x1000
    if any(b != 0xFF for b in rom[0x1FF0000:0x2000000]): sys.exit('the end of the ROM is not padding')
    rom[0x1FF0000:0x1FF0000 + len(blob)] = blob
    c1, c2 = checksum_6105(rom)
    rom[0x10:0x18] = struct.pack('>II', c1, c2)
    rom[0x20:0x34] = b'DK64 TASKLOG        '
    open(dst, 'wb').write(rom)
    print('wrote %s (%d bytes), checksum %08X %08X' % (dst, len(rom), c1, c2))

if __name__ == '__main__':
    main()
