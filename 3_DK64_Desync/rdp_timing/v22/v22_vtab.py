def load(p):
    d={}
    for l in open(p,errors='replace'):
        f=l.rstrip('\n').split('\t')
        if f[0]=='V' and len(f)>=6: d[(f[1].replace('.bin',''),f[2],f[3])]=int(f[4])
    return d
hw=load('hw/v22_ntsc_002.txt'); pal=load('hw/v22_pal_001.txt'); ar=load('rdp_timing_v22/ares_reference.txt')
lists=['bar_i2274','bar_i2279','bar_i2284','bar_i2314','bar_i2338','bar_i2404','bar_i2410','bar_2316','dk64_797']
vars_=['full','no_tri','no_load','no_rect','state_only','tri_only','fetch_only']
print('V vi_off  hw DPC_CLOCK / ares, ratio.')
print('list        '+' '.join(f"{v:>22s}" for v in vars_))
for L in lists:
    row=[]
    for v in vars_:
        k=(L,v,'vi_off')
        if k in hw and k in ar: row.append(f"{hw[k]:8d}/{ar[k]:8d} {hw[k]/ar[k]:.3f}")
        else: row.append(' '*22)
    print(f"{L:12s}"+' '.join(row))
print('\ncontributions (full - no_X) hw / ares:')
print('list         tri                   load                  rect                  state_only            fetch_only')
for L in lists:
    out=[]
    for v in ('no_tri','no_load','no_rect'):
        h=hw[(L,'full','vi_off')]-hw[(L,v,'vi_off')]; a=ar[(L,'full','vi_off')]-ar[(L,v,'vi_off')]
        out.append(f"{h:7d}/{a:7d} {h/a if a else 0:.3f}")
    for v in ('state_only','fetch_only'):
        h=hw[(L,v,'vi_off')]; a=ar[(L,v,'vi_off')]; out.append(f"{h:7d}/{a:7d} {h/a:.3f}")
    print(f"{L:12s} "+'  '.join(out))
print('\nPAL game/vi_off (VI on) hw, and ares:')
for L in lists:
    print(f"{L:12s}", ' '.join(f"{v}:{pal[(L,v,'game')]/pal[(L,v,'vi_off')]:.3f}" for v in vars_ if (L,v,'game') in pal), ' | ares:', ' '.join(f"{v}:{ar[(L,v,'game')]/ar[(L,v,'vi_off')]:.3f}" for v in ('full','tri_only','no_tri') if (L,v,'game') in ar))
print('\nNTSC vs PAL vi_off ratio:', ' '.join(f"{L}:{hw[(L,'full','vi_off')]/pal[(L,'full','vi_off')]:.4f}" for L in lists))
