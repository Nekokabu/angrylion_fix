"""STI-after-Load_Tile model (rdp_load_hazard v2..v5).

predict(L, rows, off, stride) -> differing bytes per gap 0..14 when Set_Texture_Image
(new address) follows Load_Tile after `gap` no-ops. L = row bytes, off = source
address mod 64 of row 0.
"""
import sys, collections

def adj(W, j):
    """latch offset from the previous row: W = words per row, j = previous row start word (mod 8)"""
    if W <= 8:
        if j + W > 8: return min(8 - j, 4)
        return -1 if (j % 2 and W >= 2) else 0
    t = (j + W) % 8
    return 0 if t == 0 else min(4, 8 - t)

def predict(L, rows, off, stride, gaps=15):
    W = L // 8; P = max(W + 1, 9); out = [0] * gaps
    for r in range(1, rows):                       # row 0 is always fetched before the load ends
        F = 15 - W - (rows - 1 - r) * P            # first TMEM write slot of row r
        j = ((off + (r - 1) * stride) % 64) // 8
        R = F - 4 + adj(W, j)                      # STI at gap <= R reaches row r
        for g in range(gaps):
            if g <= R: out[g] += L
    return out

if __name__ == '__main__':
    tot = collections.Counter(); bad = collections.Counter()
    for path in sys.argv[1:]:
        for l in open(path):
            p = l.rstrip('\n').split('\t')
            if p[0] == 'F' and p[1] != 'group':        # v4 / v5
                key, L, rows, off, st, seq = path + ':' + p[1], int(p[2]), int(p[3]), int(p[4]), int(p[5]), list(map(int, p[8].split(',')))
            elif p[0] == 'A' and len(p) > 8 and p[1] == 'sti' and p[2] != 'rgba32':   # v3
                continue
            elif len(p) == 7 and p[0] == 'tile_sti' and p[2] == 'rdram':          # v2
                continue
            else:
                continue
            tot[key] += 1
            if predict(L, rows, off, st) != seq[:15]:
                bad[key] += 1
                print('MISS', key, L, rows, off, st, seq, predict(L, rows, off, st))
    for k in tot: print(k, 'match', tot[k] - bad[k], '/', tot[k])
