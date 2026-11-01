#!/bin/bash
# usage: run_v20b.sh TAG [ENV...]  -> /home/claude/out/v7/v20b_TAG.txt (per-condition RSP/RDP VI from the thread clocks, runs 1-2)
T=$1; shift
S=/home/claude/out/v7
cd /home/claude/out/task_timing_v20b && env "$@" ARES_SYNC_LOG=1 ARES_TASK_LOG=1 /home/claude/ares/build/headless/headless task_timing_v20b.z64 ${FRAMES:-13500} > $S/v20b_$T.log 2>&1
python3 - $S/v20b_$T.log > $S/v20b_$T.txt <<'PY'
import re,sys,collections
mode=None; start=None; end=None; res=collections.OrderedDict()
for l in open(sys.argv[1],errors='replace'):
    if l.startswith('bar_') and ' mode ' in l: mode=l.strip(); res.setdefault(mode,[])
    m=re.match(r'TASK start #(\d+).*clock (\d+)',l)
    if m: start=int(m.group(2))
    m=re.match(r'TASK end #(\d+) clock (\d+)',l)
    if m: end=int(m.group(2))
    m=re.match(r'SYNC (\d+) t=(\d+) .*delay=(\d+)',l)
    if m and start and mode:
        t=int(m.group(2)); res[mode].append(((end-start)/3125000,(t-start)/3125000,int(m.group(3))))
for k,v in res.items():
    v=v[1:] if len(v)>1 else v
    if not v: continue
    rsp=sorted(x[0] for x in v)[len(v)//2]; rdp=sorted(x[1] for x in v)[len(v)//2]; d=sorted(x[2] for x in v)[len(v)//2]
    print(f"{k:34s} rsp {rsp:.3f}  rdp {rdp:.3f}  delay {d/1041667:.3f} VI")
PY
cat $S/v20b_$T.txt
