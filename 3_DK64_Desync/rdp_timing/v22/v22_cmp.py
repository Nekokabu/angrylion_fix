import sys
from v22_syncmap import parse
def loadW(p):
    d={}
    for l in open(p,errors='replace'):
        f=l.rstrip('\n').split('\t')
        if f[0]=='W' and len(f)>=12: d[tuple(f[1:6])]=f
    return d
def loadV(p):
    r={}
    for l in open(p,errors='replace'):
        f=l.rstrip('\n').split('\t')
        if f[0]=='V' and len(f)>=6: r[(f[1].replace('.bin',''),f[2],f[3])]=int(f[4])
    return r
tags=sys.argv[1:]
hw=loadW('hw/v22_ntsc_002.txt'); hv=loadV('hw/v22_ntsc_002.txt')
runs={t:(loadW(f'v22runs/{t}.txt'),parse(t)[0]) for t in tags}
print('W vi_off pass clk/tri: hw then hw/model per tag',tags)
for mode in ('2cyc_tex_blend_z','1cyc_tex_blend_z'):
    for H in ('1','3','16'):
        row=[]
        for w in ('1','2','4','8','16','32','64'):
            k=('vi_off',mode,w,H,'pass')
            if k in hw: row.append(f"w{w}:{float(hw[k][11]):6.1f} "+'/'.join(f"{float(hw[k][11])/float(runs[t][0][k][11]):.2f}" for t in tags))
        print(mode,'H',H,' '.join(row))
for mode in ('2cyc_tex_blend','2cyc_tex_z','2cyc_tex','1cyc_tex_z'):
    row=[]
    for w in ('2','4','8','32'):
        k=('vi_off',mode,w,'3','pass'); row.append(f"w{w}:{float(hw[k][11]):6.1f} "+'/'.join(f"{float(hw[k][11])/float(runs[t][0][k][11]):.2f}" for t in tags))
    print(mode,'H 3',' '.join(row))
print('\nV vi_off: tri contribution hw/model, full hw/model per tag; model tri spans/px')
for L in ['bar_i2274','bar_i2279','bar_i2284','bar_i2314','bar_i2338','bar_i2404','bar_i2410','bar_2316','dk64_797']:
    h=hv[(L,'full','vi_off')]-hv[(L,'no_tri','vi_off')]; hf=hv[(L,'full','vi_off')]
    out=[]
    for t in tags:
        d=runs[t][1]; m=d[(L,'full','vi_off')]['cyc']-d[(L,'no_tri','vi_off')]['cyc']; mf=d[(L,'full','vi_off')]['cyc']
        to=d[(L,'tri_only','vi_off')]
        out.append(f"{t}: tri {h/m:.3f} full {hf/mf:.3f} (sp {sum(v for k,v in to['modesp'].items() if k<32)} px {sum(v for k,v in to['modepx'].items() if k<32)})")
    print(f"{L:10s} "+' | '.join(out))
