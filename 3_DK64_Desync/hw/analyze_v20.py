import sys, collections
VI = 781250.0
def load(p):
    d = collections.defaultdict(list)
    for l in open(p, encoding='utf-8', errors='replace'):
        f = l.rstrip('\n').split('\t')
        if f[0] != 'T': continue
        d[(f[1], f[2], f[3])].append(tuple(int(x) for x in f[5:11]))
    return d
files = sys.argv[1:]
data = [load(p) for p in files]
keys = sorted(set().union(*[set(d) for d in data]))
print(f"{'dump':10s}{'vi':8s}{'cpu':6s} | " + ' | '.join(f"{p.split('/')[-1][:22]:>34s}" for p in files))
print(' ' * 26 + '| ' + ' | '.join(f"{'t_sp VI':>8s} {'t_dp VI':>8s} {'clk M':>7s} {'loads':>7s}" for _ in files))
for k in keys:
    row = []
    for d in data:
        rs = d.get(k)
        if not rs: row.append(' ' * 34); continue
        tsp = min(r[0] for r in rs) / VI; tdp = min(r[1] for r in rs) / VI
        clk = min(r[4] for r in rs) / 1e6; loads = max(r[5] for r in rs)
        row.append(f"{tsp:8.3f} {tdp:8.3f} {clk:7.3f} {loads:7d}")
    print(f"{k[0]:10s}{k[1]:8s}{k[2]:6s} | " + ' | '.join(row))
