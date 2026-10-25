import sys, collections
def rows(path, keep):
    out = collections.OrderedDict()
    for l in open(path):
        p = l.rstrip('\n').split('\t')
        if not p or l.startswith('#') or not keep(p): continue
        out['\t'.join(p[:-1] if p[0] in ('A','B','C') else p)] = 1   # A/B/C: drop ref_nonzero? keep key incl. all
    return list(out)
def cmp(hw, emu, keep, label):
    h = [l.rstrip('\n') for l in open(hw) if not l.startswith('#') and keep(l.split('\t'))]
    e = [l.rstrip('\n') for l in open(emu) if not l.startswith('#') and keep(l.split('\t'))]
    n = min(len(h), len(e)); bad = [(a, b) for a, b in zip(h, e) if a != b]
    groups = collections.Counter(a.split('\t')[0] + ' ' + a.split('\t')[1] for a, b in bad)
    print(f"{label}: {len(h)} hw lines, {len(e)} emu lines, {len(bad)} differ", dict(groups))
    for a, b in bad[:8]: print('   hw ', a, '\n   emu', b)
    return bad
v = sys.argv[1]; hw = sys.argv[2]; emu = sys.argv[3]
if v == '2': keep = lambda p: p[0] in ('tile_sti','tile_st7','block_st7','tlut_st7')
elif v == '3': keep = lambda p: p[0] in ('A','B','C') and len(p) > 3 and p[1] not in ('change','load')
else: keep = lambda p: p[0] == 'F' and p[1] != 'group'
cmp(hw, emu, keep, 'v' + v + ' ' + emu)
