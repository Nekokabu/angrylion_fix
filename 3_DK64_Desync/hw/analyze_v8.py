#!/usr/bin/env python3
"""Summarise an rdp_timing v8 result file: python3 analyze_v8.py rdp_timing_v8_001.txt"""
import sys, collections
rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    line = line.rstrip('\n')
    if not line or line.startswith('#'): continue
    f = line.split('\t'); rows[f[0]].append(f[1:])

if rows.get('G'):
    print('== G: extra cycles per primitive (scattered - stacked) / 64, and per span, by height ==')
    d = {}
    for mode, w, h, layout, k, clk, ticks in (r[:7] for r in rows['G'] if len(r) >= 7):
        d[(mode, int(w), int(h), layout)] = int(clk)
    for mode in dict.fromkeys(k[0] for k in d):
        for w in (16, 64):
            line = []
            for h in (1, 2, 4, 8, 16, 32):
                st, sc, co = d.get((mode, w, h, 'stacked')), d.get((mode, w, h, 'scattered')), d.get((mode, w, h, 'columns'))
                if st and sc:
                    line.append(f'h{h:2d}: +{(sc-st)/64:5.0f}/prim ({(sc-st)/64/h:4.0f}/span) cols +{(co-st)/64:5.0f}')
            if line: print(f'{mode:24s} w={w:3d}  ' + ' | '.join(line))
if rows.get('GT'):
    print('\n== GT: triangles, extra cycles per primitive vs stacked ==')
    d = {}
    for mode, legs, layout, k, clk, ticks in (r[:6] for r in rows['GT'] if len(r) >= 6):
        d[(mode, int(legs), layout)] = int(clk)
    for (mode, legs, layout), v in d.items():
        if layout == 'stacked': continue
        st = d.get((mode, legs, 'stacked'))
        print(f'{mode:24s} legs {legs:2d} {layout:9s}: stacked {st/64:7.1f}/prim, {layout} +{(v-st)/64:6.1f}/prim ({v/st:.3f}x)')
if rows.get('F'):
    print('\n== F: CPU test time relative to idle (VI on), by RDP load ==')
    d = collections.defaultdict(dict)
    for vi, load, test, ticks, busy in (r[:5] for r in rows['F'] if len(r) >= 5):
        d[test][(vi, load)] = (int(ticks), int(busy))
    loads = []
    for r in rows['F']:
        if (r[0], r[1]) not in loads: loads.append((r[0], r[1]))
    print(f'{"test":44s}' + ''.join(f'{(vi+" "+load)[:22]:>24s}' for vi, load in loads))
    for test, vals in d.items():
        ref = vals.get(('on', 'idle'), (0, 1))[0]
        cells = []
        for key in loads:
            if key in vals:
                t, busy = vals[key]
                cells.append(f'{t:10d} ({t/ref:5.3f}){"" if busy else "!"}'.rjust(24))
            else: cells.append(' ' * 24)
        print(f'{test:44s}' + ''.join(cells))
    print('  (! = the RDP list finished before the CPU test did: ratio not valid)')
