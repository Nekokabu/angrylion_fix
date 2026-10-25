#!/usr/bin/env python3
"""Compare rdp_load_hazard read-backs (console vs. reference without hazards).

    rdp_hazard_diff.py CONSOLE_DIR REFERENCE_DIR [--case N] [--verbose]

REFERENCE_DIR is a run of the same ROM in an emulator that does not model
load hazards (e.g. angrylion-rdp-plus); it gives the TMEM contents a load
produces when nothing interferes. For every case the TMEM bytes that differ
are grouped into address ranges and classified:

  B@same   the byte the load would have read at the same source offset,
           but from source B (the address set by Set_Texture_Image after
           the load)
  A+d      a byte of source A from another offset (d = offset difference)
  zero     not written (TMEM was cleared to 0 before the load)
  other    none of these

Source A byte i = (i*37+11) & 255, source B byte i = (i*53+0x5A) & 255, so
a byte value identifies its source offset modulo 256.
"""
import argparse, csv, os, sys
from collections import Counter

INV37 = 173                         # 37 * 173 = 1 (mod 256)
A = lambda i: (i * 37 + 11) & 255
B = lambda i: (i * 53 + 0x5A) & 255
A_OFF = {A(i): i for i in range(256)}
B_OFF = {B(i): i for i in range(256)}

def tmem_of(readback):
    """read-back (64x64 I8, line 8) -> linear TMEM bytes"""
    t = bytearray(4096)
    for k in range(4096):
        row, x = divmod(k, 64)
        t[row * 64 + (x ^ (4 if row & 1 else 0))] = readback[k]
    return t

def load_index(d):
    rows = [l for l in open(os.path.join(d, 'index.txt')) if not l.startswith('#')]
    return {int(r['case']): r for r in csv.DictReader(rows, delimiter='\t')}

def read_runs(d, case):
    runs = []
    r = 0
    while os.path.exists(os.path.join(d, f'case_{case:03d}_{r}.bin')):
        runs.append(open(os.path.join(d, f'case_{case:03d}_{r}.bin'), 'rb').read())
        r += 1
    return runs

def ranges(addrs):
    out, start, prev = [], None, None
    for a in sorted(addrs):
        if start is None: start = prev = a
        elif a == prev + 1: prev = a
        else: out.append((start, prev)); start = prev = a
    if start is not None: out.append((start, prev))
    return out

def classify(hw, ref):
    if hw == 0: return 'zero'
    if ref in A_OFF and hw in B_OFF and B_OFF[hw] == A_OFF[ref]: return 'B@same'
    if hw in A_OFF and ref in A_OFF: return f'A{(A_OFF[hw] - A_OFF[ref] + 128) % 256 - 128:+d}'
    if hw in A_OFF and ref == 0: return 'A(moved)'
    return 'other'

def describe(r):
    return f"{r['load']} {r['format']} w{r['width']} r{r['rows']} dxt{r['dxt']} gap {r['gap']} {r['gap_type']} -> {r['change']}"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('console'); ap.add_argument('reference')
    ap.add_argument('--case', type=int); ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    idx = load_index(a.console)
    for c in sorted(idx):
        if a.case is not None and c != a.case: continue
        hw_runs = read_runs(a.console, c); ref_runs = read_runs(a.reference, c)
        if not hw_runs or not ref_runs: continue
        ref = tmem_of(ref_runs[0])
        results = []
        for run in hw_runs:
            hw = tmem_of(run)
            diff = [i for i in range(4096) if hw[i] != ref[i]]
            results.append((hw, diff))
        same_runs = len({bytes(h) for h, _ in results})
        hw, diff = results[0]
        line = f"{c:3d} {describe(idx[c])}: "
        if not diff:
            print(line + "identical" + ("" if same_runs == 1 else f" (runs differ: {same_runs} variants)"))
            continue
        kinds = Counter(classify(hw[i], ref[i]) for i in diff)
        rs = ', '.join(f'{s}-{e}' if s != e else f'{s}' for s, e in ranges(diff))
        print(line + f"{len(diff)} bytes differ at TMEM {rs}; " + ', '.join(f'{k} x{n}' for k, n in kinds.most_common())
              + ("" if same_runs == 1 else f"  [runs differ: {same_runs} variants: " +
                 ', '.join(str(len(d)) for _, d in results) + " bytes]"))
        if a.verbose:
            for i in diff[:64]:
                print(f"      {i:4d}: hw {hw[i]:02X} ref {ref[i]:02X} {classify(hw[i], ref[i])}")

if __name__ == '__main__':
    sys.exit(main())
