#!/usr/bin/env python3
"""Compare and analyse rdp_timing framebuffer dumps (rdp_timing_NNN_fb/).

    rdp_fb_diff.py DUMP_DIR                       analyse one set of dumps
    rdp_fb_diff.py DUMP_A DUMP_B                  compare two sets pixel by pixel
    rdp_fb_diff.py DUMP_A DUMP_B --png OUT_DIR    also write A/B/diff PNG images

Typical use: A = dumps from the console, B = dumps from an emulator (the
package ships angrylion-rdp-plus reference dumps in examples/).

Analysis of one set checks every copy / 1-cycle CI8 (palette) dump against the
texture and palette the ROM uses, decodes the hazard cases (which TMEM bytes a
load got wrong and where they came from) and the span cases (rows whose first
pixel differs from its neighbour, drawn extent), and decodes the probe cases:
  ci8_as_i8         the CI8 texture read back as I8 after the TLUT load:
                    shows the palette indices actually stored in TMEM
  ci8_as_i8_notlut  the same without a TLUT load (control)
  tlut_view         16x16 indices 0..255: pixel i shows palette entry i
  tlut_view_line32  the same with the TLUT load tile's line set to 32
Standard library only.
"""

import argparse
import csv
import os
import struct
import sys
import zlib
from collections import Counter

# --- the ROM's test data (see init_textures() in main.c) ---------------------

def texci8_index(tx, ty):
    return ((ty * 32 + tx) * 37) & 0xFF

def tlut_entry(i):
    return (i * 0x0842 | 1) & 0xFFFF

TLUT_INV = {}
for _i in range(256):
    TLUT_INV.setdefault(tlut_entry(_i), _i)

# --- dump loading ------------------------------------------------------------

def load_index(d):
    path = os.path.join(d, "index.txt")
    with open(path, encoding="utf-8") as f:
        lines = [l for l in f if not l.startswith("#") and l.strip()]
    out = {}
    for r in csv.DictReader(lines, delimiter="\t"):
        for k in ("id", "fb", "w", "h", "bpp"):
            r[k] = int(r[k])
        r["x0"] = int(r.get("x0") or 0)      # v3 columns
        r["fbw"] = int(r.get("fbw") or 320)
        with open(os.path.join(d, r["file"]), "rb") as f:
            r["data"] = f.read()
        out[r["id"]] = r
    return out

def pixels(r):
    n = r["w"] * r["h"]
    if r["bpp"] == 16:
        return list(struct.unpack(f">{n}H", r["data"][:2 * n]))
    return list(struct.unpack(f">{n}I", r["data"][:4 * n]))

def to5551(v, bpp):
    """Pixel value -> RGBA5551 (a 32-bit pixel is reduced to its top bits)."""
    if bpp == 16:
        return v
    r, g, b, a = v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255
    return (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7)

def rgb8(v, bpp):
    if bpp == 16:
        r, g, b = v >> 11, (v >> 6) & 31, (v >> 1) & 31
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)
    return (v >> 24, (v >> 16) & 255, (v >> 8) & 255)

def describe(r):
    parts = [r["group"], r["prim"], r["cycle"], f"fb{r['fb']}", r["tex"]]
    if r["flags"] != "-":
        parts.append(r["flags"])
    parts.append(f"{r['w']}x{r['h']}")
    return " ".join(parts)

def ranges(vals):
    vals = sorted(set(vals))
    out, start, prev = [], None, None
    for v in vals:
        if start is None:
            start = prev = v
        elif v == prev + 1:
            prev = v
        else:
            out.append((start, prev)); start = prev = v
    if start is not None:
        out.append((start, prev))
    return ", ".join(f"{a}" if a == b else f"{a}-{b}" for a, b in out)

# --- PNG writer --------------------------------------------------------------

def write_png(path, w, h, rgb_rows):
    # small cases are scaled up (nearest neighbour) so they are readable
    k = max(1, min(8, 256 // max(w, h)))
    if k > 1:
        rgb_rows = [[c for x in range(w) for _ in range(k) for c in row[3 * x:3 * x + 3]]
                    for row in rgb_rows for _ in range(k)]
        w, h = w * k, h * k
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)

def image_rows(r, px):
    w = r["w"]
    return [[c for v in px[y * w:(y + 1) * w] for c in rgb8(v, r["bpp"])] for y in range(r["h"])]

# --- analysis of one set -------------------------------------------------------

def analyse_ci8(r, px):
    """CI8 textured rectangles: 1 texel per pixel, texture wraps at 32x32."""
    w, bpp = r["w"], r["bpp"]
    bad, deltas, rows = 0, Counter(), []
    for i, v in enumerate(px):
        x, y = i % w, i // w
        tx, ty = x % 32, y % 32
        idx = texci8_index(tx, ty)
        got = to5551(v, bpp) | 1        # low bit is coverage in 16-bit
        if got == tlut_entry(idx) | 1:
            continue
        bad += 1
        rows.append(ty)
        entry = TLUT_INV.get(got)
        if entry is not None and bad <= 4096:
            for q in range(1024):       # texels holding the index that was shown
                if ((q * 37) & 0xFF) == entry:
                    deltas[(q % 32 - tx, q // 32 - ty)] += 1
    if not bad:
        return "palette lookup correct for every pixel"
    msg = f"{bad}/{len(px)} pixels show the wrong palette entry; texture rows {ranges(rows)}"
    if deltas:
        (dx, dy), n = deltas.most_common(1)[0]
        msg += f"; most common: shown entry = index of texel at offset ({dx:+d},{dy:+d}) ({n}x)"
    return msg

def analyse_probe(r, px):
    w, bpp = r["w"], r["bpp"]
    if r["prim"].startswith("ci8_as_i8"):
        bad = []
        for i, v in enumerate(px):
            x, y = i % w, i // w
            got = v >> 24 if bpp == 32 else (v >> 11) << 3
            if got != texci8_index(x, y):
                bad.append((x, y, got, texci8_index(x, y)))
        if not bad:
            return "TMEM indices intact (all 1024 match the texture)"
        rows = ranges(b[1] for b in bad)
        ex = ", ".join(f"({x},{y}) {g} expected {e}" for x, y, g, e in bad[:6])
        return f"{len(bad)}/1024 indices differ in TMEM; texture rows {rows}; e.g. {ex}"
    if r["prim"].startswith("tlut_view"):
        bad = []
        for i, v in enumerate(px):
            got = to5551(v, bpp) | 1
            if got != tlut_entry(i) | 1:
                bad.append((i, got))
        if not bad:
            return "palette intact (all 256 entries match)"
        ex = ", ".join(f"entry {i}: {g:04X} (= entry {TLUT_INV.get(g, '?')}) expected {tlut_entry(i):04X}"
                       for i, g in bad[:6])
        return f"{len(bad)}/256 palette entries differ: {ranges(i for i, _ in bad)}; e.g. {ex}"
    return None

# --- v3: load hazards and 2-cycle first-pixel ("span") cases --------------------

def texalt_byte(i):
    return (i * 53 + 0x5A) & 0xFF

def analyse_hazard(r, px):
    """TMEM 0..0x7FF read back as I8 (32x64): expect texci8 bytes, then zeros.

    Both test patterns are bijective byte sequences (texci8[i] = 37i,
    texalt[i] = 53i + 0x5A, mod 256), so one byte matches either; the source
    is the one that explains the wrong bytes with a single constant offset."""
    wrong = []
    for i, v in enumerate(px):
        got = v >> 24
        exp = ((i * 37) & 0xFF) if i < 1024 else 0
        if got != exp:
            wrong.append((i, got))
    if not wrong:
        return "TMEM correct (load complete, nothing overwritten)"
    zero = [i for i, g in wrong if g == 0]
    data = [(i, g) for i, g in wrong if g != 0]
    msg = f"{len(wrong)} TMEM bytes wrong, bytes {ranges(i for i, _ in wrong)}"
    if zero:
        msg += f"; {len(zero)} never written (0)"
    if data:
        best = None
        for name, inv, base in (("new source (texalt)", 29, 0x5A), ("original texture", 173, 0)):
            offs = Counter((((g - base) * inv - i) & 0xFF) for i, g in data)
            o, n = offs.most_common(1)[0]
            if best is None or n > best[2]:
                best = (name, o, n)
        name, o, n = best
        msg += f"; {n}/{len(data)} bytes = {name} at TMEM offset {o if o < 128 else o - 256:+d} (mod 256)"
    return msg

def pattern_pixel(x, y, bpp):
    r, g, b = (x * 7 + y * 13) & 0xFF, (x * 3 + y * 5) & 0xFF, (x + y * 11) & 0xFF
    if bpp == 32:
        return r << 24 | g << 16 | b << 8 | 0xFF
    return (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1

def analyse_span(r, px):
    w, h, x0, bpp = r["w"], r["h"], r["x0"], r["bpp"]
    prefill = "prefill" in r["flags"]
    xs = [x for x in range(w) if any(px[y * w + x] != (pattern_pixel(x, y, bpp) if prefill else 0) for y in range(h))]
    if not xs:
        return "nothing drawn"
    left, right = min(xs), max(xs)
    msg = f"drawn x {left}-{right}"
    if not prefill:
        odd = [y for y in range(h) if px[y * w + left] != px[y * w + left + 1]]
        msg += "; first pixel differs from its neighbour in rows " + ranges(odd) if odd else "; uniform (first pixel = neighbour in every row)"
    else:
        msg += "; prefilled: compare against a reference with two dump sets"
    return msg

def analyse(dumps):
    for i in sorted(dumps):
        r = dumps[i]
        px = pixels(r)
        if r["group"] == "probe":
            msg = analyse_probe(r, px)
        elif r["group"] == "hazard":
            msg = analyse_hazard(r, px)
        elif r["group"] == "span":
            msg = analyse_span(r, px)
        elif r["tex"] == "ci8" and r["cycle"] in ("copy", "1cyc"):
            # 2-cycle cases multiply by ENV in the second cycle, so only copy
            # and 1-cycle output the palette colour directly
            msg = analyse_ci8(r, px)
        else:
            msg = None
        if msg:
            print(f"{i:>4} {describe(r)}\n     {msg}")

# --- comparison ------------------------------------------------------------------

def compare(a_set, b_set, png_dir, max_list):
    same = diff = 0
    for i in sorted(a_set):
        a, b = a_set[i], b_set.get(i)
        if b is None:
            print(f"{i:>4} only in A"); continue
        if (a["w"], a["h"], a["bpp"]) != (b["w"], b["h"], b["bpp"]):
            print(f"{i:>4} different case in B, skipped"); continue
        if a["data"] == b["data"]:
            same += 1; continue
        diff += 1
        pa, pb, w = pixels(a), pixels(b), a["w"]
        d = [k for k in range(len(pa)) if pa[k] != pb[k]]
        xs, ys = [k % w for k in d], [k // w for k in d]
        print(f"{i:>4} {describe(a)}: {len(d)}/{len(pa)} pixels differ, "
              f"x {min(xs)}-{max(xs)}, y {min(ys)}-{max(ys)}")
        print(f"     rows: {ranges(ys)[:200]}")
        for k in d[:max_list]:
            fmt = "%04X" if a["bpp"] == 16 else "%08X"
            print(f"     ({k % w},{k // w}) A {fmt % pa[k]} B {fmt % pb[k]}")
        if png_dir:
            os.makedirs(png_dir, exist_ok=True)
            write_png(os.path.join(png_dir, f"case_{i:03d}_A.png"), w, a["h"], image_rows(a, pa))
            write_png(os.path.join(png_dir, f"case_{i:03d}_B.png"), w, a["h"], image_rows(b, pb))
            dset = set(d)
            rows = [[c for x in range(w) for c in ((255, 0, 0) if y * w + x in dset else (40, 40, 40))]
                    for y in range(a["h"])]
            write_png(os.path.join(png_dir, f"case_{i:03d}_diff.png"), w, a["h"], rows)
    print(f"\nidentical: {same}, different: {diff}")

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump_a")
    ap.add_argument("dump_b", nargs="?")
    ap.add_argument("--png", metavar="OUT_DIR", help="write A/B/diff images of differing cases")
    ap.add_argument("--list", type=int, default=8, help="differing pixels to list per case")
    args = ap.parse_args()

    a = load_index(args.dump_a)
    if args.dump_b:
        compare(a, load_index(args.dump_b), args.png, args.list)
    else:
        analyse(a)

if __name__ == "__main__":
    sys.exit(main())
