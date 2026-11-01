import sys
def load(path):
    d = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if f[0] != 'X': continue
        lst, vi, cond = f[1].replace('.bin', ''), f[2], f[3]
        clock, ticks, lticks = int(f[4]), int(f[5]), int(f[6])
        counts = list(map(int, f[7:12])); model = int(f[12])
        d[(lst, vi, cond)] = (clock, ticks, lticks, counts, model)
    return d
hw = load(sys.argv[1]); ar = load(sys.argv[2])
names = ['chase', 'mixed', 'icache', 'ucw', 'dma']
def cost(d, lst, vi, cond):
    if (lst, vi, cond) not in d or (lst, vi, 'none') not in d: return None
    c, t, lt, cnt, m = d[(lst, vi, cond)]; c0 = d[(lst, vi, 'none')][0]
    tot = sum(cnt)
    return c - c0, cnt, lt
lists = sorted({k[0] for k in hw})
conds = ['chase', 'mixed', 'icache', 'ucw', 'dma64', 'dma512', 'chase+dma64', 'mixed+dma64', 'mixed+dma512', 'chase+icache', 'mixed+icache', 'ucw+dma64', 'mixed+icache+dma64', 'chase+icache+dma64+ucw']
print('list                 cond                   hw dClk   per-op(hw)  per-op(ares)   CPU cyc/op hw  ares   counts(hw)')
for lst in lists:
    for cond in conds:
        h = cost(hw, lst, 'game', cond); a = cost(ar, lst, 'game', cond)
        if not h: continue
        dh, ch, lth = h
        main = [i for i, n in enumerate(ch) if n][0] if any(ch) else 0
        # per-op RDP cost attributed to the dominant op (others small); also CPU cycles per dominant op
        per_h = dh / max(1, ch[main]); per_a = (a[0] / max(1, a[1][main])) if a else float('nan')
        cpu_h = lth * 2 / max(1, ch[main]); cpu_a = (a[2] * 2 / max(1, a[1][main])) if a else float('nan')
        print(f'{lst:20s} {cond:22s} {dh:8d} {per_h:9.2f} {per_a:12.2f}   {cpu_h:10.1f} {cpu_a:8.1f}   {ch}')
    print()
# additivity check: predicted from single-load per-op costs
print('additivity (hw, game): measured dClk vs sum of single-load per-op costs x counts')
for lst in lists:
    per = {}
    for cond, idx in [('chase', 0), ('mixed', 1), ('icache', 2), ('ucw', 3), ('dma64', 4)]:
        h = cost(hw, lst, 'game', cond)
        if h: per[idx] = h[0] / max(1, h[1][idx])
    for cond in ['chase+dma64', 'mixed+dma64', 'ucw+dma64', 'chase+icache', 'mixed+icache', 'mixed+icache+dma64', 'chase+icache+dma64+ucw']:
        h = cost(hw, lst, 'game', cond)
        if not h: continue
        pred = sum(per.get(i, 0) * h[1][i] for i in range(5))
        print(f'{lst:20s} {cond:24s} measured {h[0]:8d} predicted {pred:9.0f}  ratio {h[0]/pred if pred else 0:.3f}')
    print()
