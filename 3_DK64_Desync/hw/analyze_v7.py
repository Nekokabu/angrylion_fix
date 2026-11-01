#!/usr/bin/env python3
"""Summarise an rdp_timing v7 result file.

    python3 analyze_v7.py rdp_timing_v7_001.txt

A: clock relative to the default layout (zb = fb + 0x4B000) per offset and mode
B: extra RDP cycles per command issued between primitives
C: cost of a load sequence between primitives beyond load + primitive alone
D: span cost relative to the quiet run, against the generator's bandwidth
E: scattered / stacked ratio
"""
import sys, collections

rows = collections.defaultdict(list)
header = []
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if not line:
        continue
    if line.startswith('#'):
        header.append(line)
        continue
    f = line.split('\t')
    rows[f[0]].append(f[1:])

print('\n'.join(header[:6]))
base = int(rows['B0'][0][0]) if rows.get('B0') else 0
print(f'\nlist base (start + Sync_Full): {base} cycles')

# ---- A ----
if rows.get('A'):
    print('\n== A: depth buffer placement (clock / clock at the default offset 0x4B000, same base) ==')
    data = {}
    for fb_off, off, mode, test, clk in (r[:5] for r in rows['A'] if len(r) >= 5):
        data[(fb_off, int(off), mode, test)] = int(clk)
    bases = sorted({k[0] for k in data})
    modes = []
    for k in rows['A']:
        if k[2] not in modes: modes.append(k[2])
    offs = sorted({k[1] for k in data})
    for test in 'SLR':
        print(f'-- test {test}')
        print('fb_base  offset    ' + ' '.join(f'{m:>22s}' for m in modes))
        for fb in bases:
            for off in offs:
                vals = []
                for m in modes:
                    v = data.get((fb, off, m, test)); ref = data.get((fb, 0x4B000, m, test))
                    vals.append(f'{v:8d} ({v/ref:5.3f})' if v and ref else ' ' * 16)
                if any(x.strip() for x in vals):
                    print(f'{fb:8s} {off:+9x}  ' + ' '.join(f'{x:>22s}' for x in vals))

# ---- B ----
if rows.get('B'):
    print('\n== B: extra cycles per command between primitives (26 primitives) ==')
    data = collections.defaultdict(dict)
    for mode, w, cmd, clk in (r[:4] for r in rows['B'] if len(r) >= 4):
        data[(mode, int(w))][cmd] = int(clk)
    cmds = []
    for r in rows['B']:
        if r[2] not in cmds: cmds.append(r[2])
    keys = sorted(data)
    print(f'{"command":28s}' + ' '.join(f'{m[:12]+"/"+str(w):>16s}' for m, w in keys))
    for cmd in cmds:
        print(f'{cmd:28s}' + ' '.join(f'{(data[k][cmd]-data[k]["none"])/26:16.1f}' if cmd in data[k] and 'none' in data[k] else ' '*16 for k in keys))
if rows.get('BT'):
    print('\n== BT: the same with triangles (legs 16x8) ==')
    data = collections.defaultdict(dict)
    for mode, cmd, clk in (r[:3] for r in rows['BT'] if len(r) >= 3):
        data[mode][cmd] = int(clk)
    for mode in data:
        print(mode + ':  ' + '  '.join(f'{cmd}={(v-data[mode]["none"])/26:+.1f}' for cmd, v in data[mode].items()))

# ---- C ----
if rows.get('C'):
    print('\n== C: load sequences between small primitives, cycles per repetition ==')
    data = collections.defaultdict(dict)
    for mode, prim, size, texels, pat, reps, clk in (r[:7] for r in rows['C'] if len(r) >= 7):
        data[(mode, prim, int(size), int(texels))][pat] = (int(clk) - base) / int(reps)
    print(f'{"mode/prim/size/texels":40s} {"prim":>8s} {"load":>8s} {"load+prim":>10s} {"extra":>8s} {"nopipe":>8s} {"nosync":>8s} {"tile":>8s} {"syncs":>8s}')
    def col(d, pat, minus, width=8):
        return f'{d[pat]-minus:{width}.1f}' if pat in d else ' ' * width
    for k in sorted(data):
        d = data[k]
        p, l = d.get('prim_only', 0), d.get('load_only', 0)
        print(f'{"/".join(map(str, k)):40s} {col(d, "prim_only", 0)} {col(d, "load_only", 0)} {col(d, "load+prim", 0, 10)} '
              f'{col(d, "load+prim", l + p)} {col(d, "load(no sync_pipe)+prim", l + p)} {col(d, "load(no syncs)+prim", l + p)} '
              f'{col(d, "load_tile+prim", l + p)} {col(d, "sync_load+sync_pipe+prim", p)}')

# ---- S ----
if rows.get('S'):
    print('\n== S: extra cycles per primitive for a sync sequence after it (DPC_CLOCK; in brackets: CPU wall time x4/3) ==')
    data = collections.defaultdict(dict)
    for mode, w, h, seq, k, clk, ticks in (r[:7] for r in rows['S'] if len(r) >= 7):
        data[(mode, int(w), int(h))][seq] = (int(clk), int(ticks) * 4 / 3, int(k))
    seqs = []
    for r in rows['S']:
        if r[3] not in seqs: seqs.append(r[3])
    print(f'{"mode/w/h":34s}' + ' '.join(f'{q[:22]:>24s}' for q in seqs if q != 'none'))
    for key in sorted(data):
        d = data[key]
        if 'none' not in d: continue
        c0, t0, k = d['none']
        cells = []
        for q in seqs:
            if q == 'none': continue
            if q in d and d[q][0]:
                cells.append(f'{(d[q][0]-c0)/k:9.1f} [{(d[q][1]-t0)/k:7.1f}]  ')
            else:
                cells.append(' ' * 24)
        print(f'{"/".join(map(str, key)):34s}' + ' '.join(cells))

# ---- E ----
if rows.get('E'):
    print('\n== E: scattered / stacked ==')
    data = collections.defaultdict(dict)
    for mode, w, layout, clk in (r[:4] for r in rows['E'] if len(r) >= 4):
        data[(mode, int(w))][layout] = int(clk)
    for k in sorted(data):
        d = data[k]
        print(f'{k[0]:28s} w={k[1]:3d}  stacked {d["stacked"]:7d}  scattered {d["scattered"]/d["stacked"]:.3f}  columns {d["columns"]/d["stacked"]:.3f}')

# ---- D ----
if rows.get('D'):
    print('\n== D: RDRAM contention (clock / quiet clock) ==')
    data = collections.defaultdict(dict)
    for vi, lst, gen, gap, clk, bytes_, acc, mbs in (r[:8] for r in rows['D'] if len(r) >= 8):
        data[(vi, lst)][(gen, int(gap))] = (int(clk), int(bytes_), int(acc), float(mbs))
    for (vi, lst), d in data.items():
        ref = d[('none', 0)][0]
        print(f'-- {lst}  VI {vi}  quiet {ref} cycles ({ref/62500:.2f} ms)')
        for (gen, gap), (clk, b, acc, mbs) in d.items():
            if gen == 'none': continue
            tps = acc * 62.5e6 / clk / 1e6 if clk else 0
            print(f'   {gen:24s} gap {gap:3d}: {mbs:6.1f} MB/s {tps:5.2f} Mtrans/s  ratio {clk/ref:.3f}')
