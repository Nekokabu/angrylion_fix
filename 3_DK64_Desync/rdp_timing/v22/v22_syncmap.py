import re,sys,json
def parse(tag):
    V=[l.rstrip('\n').split('\t') for l in open(f'v22runs/{tag}.txt') if l.startswith('V\t')]
    V=[f for f in V if len(f)>=6]
    syncs=[]; cur=None
    for l in open(f'v22runs/{tag}.sync'):
        if l.startswith('SYNC '):
            cur={'cyc':int(re.search(r'cyc=(\d+)',l).group(1)),'px':int(re.search(r'px=(\d+)',l).group(1)),'spans':int(re.search(r'spans=(\d+)',l).group(1)),'prims':int(re.search(r'prims=(\d+)',l).group(1)),'rects':int(re.search(r'rects=(\d+)',l).group(1)),'load':int(re.search(r'load=(\d+)',l).group(1))}
            syncs.append(cur)
        elif l.startswith('MODEPX'): cur['modepx']={int(a):int(b) for a,b in re.findall(r'(\d+):(\d+)',l)}
        elif l.startswith('MODESP'): cur['modesp']={int(a):int(b) for a,b in re.findall(r'(\d+):(\d+)',l)}
        elif l.startswith('MODES') : cur['modes']={int(a):int(b) for a,b in re.findall(r'(\d+):(\d+)',l)}
        elif l.startswith("ZPASS"): pass
    out={}
    for i,f in enumerate(V):
        s=syncs[3+3*i+2]  # third run
        out[(f[1].replace('.bin',''),f[2],f[3])]=dict(dpc=int(f[4]),ticks=int(f[5]),**s)
    return out,syncs,V
if __name__=='__main__':
    d,syncs,V=parse(sys.argv[1])
    for L in ['bar_i2274','bar_i2279','bar_i2284','bar_i2314','bar_i2338','bar_i2404','bar_i2410','bar_2316','dk64_797']:
        s=d[(L,'full','vi_off')]; t=d[(L,'tri_only','vi_off')]
        trisp=sum(v for k,v in t['modesp'].items() if k<32); tripx=sum(v for k,v in t['modepx'].items() if k<32)
        ntri=t['prims']-t['rects']
        print(f"{L:10s} dpc {s['dpc']:8d} cyc {s['cyc']:8d}  prims {s['prims']:5d} rects {s['rects']:3d} tris {ntri:5d} tri-spans {trisp:6d} tri-px {tripx:7d}  spans/tri {trisp/ntri:5.1f} px/span {tripx/trisp:5.1f}  tri_only cyc {t['cyc']:8d}  modes {{{', '.join(f'{k}:{v}' for k,v in sorted(t['modesp'].items()) if k<32)}}}")
