#!/usr/bin/env python3
"""v24 comparison: python3 v23_cmp.py HWFILE TAG [TAG...]  (TAG = v24runs/TAG.txt, harness reference)"""
import sys, re
def loadX(p):
    d={}
    for l in open(p,errors='replace'):
        f=l.rstrip('\n').split('\t')
        if f[0]=='X' and len(f)>=6: d[(f[1].replace('.bin',''),f[2],f[3])]=int(f[4])
    return d
def loadY(p):
    d={}
    for l in open(p,errors='replace'):
        f=l.rstrip('\n').split('\t')
        if f[0]=='Y' and len(f)>=10: d[(f[1],int(f[2]))]=(int(f[6]),float(f[8]),float(f[9]))
    return d
hwp=sys.argv[1]; tags=sys.argv[2:]
hx=loadX(hwp); hy=loadY(hwp)
ax={t:loadX(f'v24runs/{t}.txt') for t in tags}; ay={t:loadY(f'v23runs/{t}.txt') for t in tags}
lists=['bar_i2274','bar_i2314','bar_i2404','bar_2316','dk64_797']
variants=['full','tri_only','z_far','z_near','om_no_zcmp','om_no_atomic','om_no_aa','om_no_cvg','color_low','z_up1m','tri_only_zfar','tri_only_znear']
print('X vi_off: hw clocks, hw - full (clk), and hw/model per tag', tags)
for L in lists:
    print('==',L)
    if (L,'full','vi_off') not in hx: print('  (no hw)'); continue
    hf=hx[(L,'full','vi_off')]
    for v in variants:
        k=(L,v,'vi_off')
        if k not in hx: continue
        h=hx[k]
        rat=' '.join(f"{t}:{h/ax[t][k]:.3f}" if k in ax[t] and ax[t][k] else f"{t}:-" for t in tags)
        dm=' '.join(f"{t}:{(ax[t][k]-ax[t][(L,'full','vi_off')]):+8d}" if k in ax[t] else '' for t in tags)
        print(f"  {v:14s} hw {h:8d} ({h-hf:+8d}, {100*(h-hf)/hf:+5.1f}%)  hw/model {rat}   model-full {dm}")
    for v in ('full','tri_only'):
        k=(L,v,'game')
        if k in hx: print(f"  {v:14s} game/vi_off hw {hx[k]/hx[(L,v,'vi_off')]:.3f}  "+' '.join(f"{t}:{ax[t][k]/ax[t][(L,v,'vi_off')]:.3f}" for t in tags if k in ax[t]))
print('\nY: hw clk/tri (clk/span) and hw/model per tag')
cases=[]
for k in hy:
    if k[0] not in cases: cases.append(k[0])
for c in cases:
    row=[]
    for w in (4,12,32):
        k=(c,w)
        if k not in hy: continue
        h=hy[k]
        row.append(f"w{w}: {h[2]:7.1f} ({h[1]:6.2f}) "+'/'.join(f"{h[0]/ay[t][k][0]:.3f}" if k in ay[t] and ay[t][k][0] else '-' for t in tags))
    base=hy.get(('base',4))
    print(f"{c:16s} "+'  '.join(row))
