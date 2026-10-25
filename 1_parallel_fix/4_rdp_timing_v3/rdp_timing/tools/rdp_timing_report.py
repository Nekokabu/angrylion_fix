#!/usr/bin/env python3
"""Turn rdp_timing results into per-primitive and per-pixel costs.

    rdp_timing_report.py RESULTS.txt                     table for both passes
    rdp_timing_report.py RESULTS.txt --pass vi_off       one pass only
    rdp_timing_report.py RESULTS.txt --group rect        one group only
    rdp_timing_report.py RESULTS.txt --metric pipebusy   other counter
    rdp_timing_report.py RESULTS.txt --csv derived.csv   also write CSV
    rdp_timing_report.py A.txt --compare B.txt           A vs B (e.g. console vs emulator)

RESULTS.txt is the summary file written by the ROM (rdp_timing_NNN.txt).

net = metric - metric of the empty list from the same source (rdram/xbus)
      in the same pass, i.e. the cost of the commands themselves;
per_prim = net / k;  per_px = per_prim / pixels.

Metrics: clock, bufbusy, pipebusy, tmem (RCP cycles) and count (CPU COUNT
ticks, converted to RCP cycles: x 4/3). Standard library only.
"""

import argparse
import csv
import sys

METRICS = {
    "clock": ("clock_med", 1.0),
    "bufbusy": ("bufbusy_med", 1.0),
    "pipebusy": ("pipebusy_med", 1.0),
    "tmem": ("tmem_med", 1.0),
    "count": ("count_med", 4.0 / 3.0),   # 46.875 MHz -> 62.5 MHz
}


def load(path):
    header, lines = {}, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                parts = line[1:].strip().split("\t")
                if len(parts) == 2:
                    header[parts[0].strip()] = parts[1].strip()
                elif line.startswith("# ABORTED"):
                    header["aborted"] = line[1:].strip()
            elif line.strip():
                lines.append(line)
    rows = list(csv.DictReader(lines, delimiter="\t"))
    for r in rows:
        for key in ("id", "fb", "w", "h", "k", "pixels"):
            r[key] = int(r[key])
    return header, rows


def value(row, metric):
    col, scale = METRICS[metric]
    return int(row[col]) * scale


def derive(rows, metric):
    base = {(r["pass"], r["source"]): value(r, metric)
            for r in rows if r["group"] == "baseline"}
    out = []
    for r in rows:
        if r["group"] == "baseline":
            continue
        net = value(r, metric) - base.get((r["pass"], r["source"]), 0.0)
        per_prim = net / r["k"] if r["k"] else 0.0
        per_px = per_prim / r["pixels"] if r["pixels"] else None
        out.append(dict(r, net=net, per_prim=per_prim, per_px=per_px))
    return out


def describe(r):
    parts = [r["prim"]]
    if r["cycle"] != "-":
        parts.append(r["cycle"])
    if r["fb"]:
        parts.append(f"fb{r['fb']}")
    if r["tex"] != "-":
        parts.append(r["tex"] + ("/" + r["filter"] if r["filter"] != "-" else ""))
    if r["flags"] != "-":
        parts.append(r["flags"])
    if r["source"] == "xbus":
        parts.append("xbus")
    return " ".join(parts)


def size_str(r):
    return f"{r['w']}x{r['h']}" if r["w"] else ""


def fmt(v, digits=2):
    return "-" if v is None else f"{v:.{digits}f}"


def print_table(rows, metric):
    print(f"{'id':>4} {'pass':6} {'group':8} {'case':44} {'size':>8} {'k':>4} "
          f"{metric + '_net':>12} {'per_prim':>10} {'per_px':>8}")
    for r in rows:
        print(f"{r['id']:>4} {r['pass']:6} {r['group']:8} {describe(r)[:44]:44} {size_str(r):>8} "
              f"{r['k']:>4} {r['net']:>12.0f} {r['per_prim']:>10.1f} {fmt(r['per_px']):>8}")


def compare(rows_a, rows_b, metric, name_a, name_b):
    b_by = {(r["id"], r["pass"]): r for r in rows_b}
    print(f"{'id':>4} {'pass':6} {'case':44} {'size':>8} "
          f"{'A per_prim':>10} {'B per_prim':>10} {'B/A':>6} render")
    render_diff = 0
    for a in rows_a:
        b = b_by.get((a["id"], a["pass"]))
        if not b:
            continue
        if describe(a) != describe(b) or a["k"] != b["k"]:
            print(f"{a['id']:>4} case differs between files, skipped")
            continue
        ratio = b["per_prim"] / a["per_prim"] if a["per_prim"] else None
        same = "-" if a["crc32"] == "-" else ("same" if a["crc32"] == b["crc32"] else "DIFF")
        render_diff += same == "DIFF"
        print(f"{a['id']:>4} {a['pass']:6} {describe(a)[:44]:44} {size_str(a):>8} "
              f"{a['per_prim']:>10.1f} {b['per_prim']:>10.1f} {fmt(ratio):>6} {same}")
    print(f"\nA = {name_a}\nB = {name_b}\ncases rendering differently: {render_diff}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("results")
    ap.add_argument("--pass", dest="pass_", choices=["vi_off", "vi_on"])
    ap.add_argument("--group")
    ap.add_argument("--metric", choices=sorted(METRICS), default="clock")
    ap.add_argument("--csv", help="write the derived table to this CSV file")
    ap.add_argument("--compare", metavar="OTHER", help="compare with another results file")
    args = ap.parse_args()

    header, rows = load(args.results)
    print("# " + ", ".join(f"{k}={v}" for k, v in header.items()))
    if "aborted" in header:
        print("# WARNING: run was aborted, results are incomplete")
    if all(int(r[METRICS[args.metric][0]]) == 0 for r in rows):
        print(f"# WARNING: every {args.metric} value is 0 - counters not working "
              "(emulator without RDP timing?)")

    def select(rs):
        return [r for r in rs
                if (not args.pass_ or r["pass"] == args.pass_)
                and (not args.group or r["group"] == args.group)]

    derived = select(derive(rows, args.metric))

    if args.compare:
        _, rows_b = load(args.compare)
        compare(derived, select(derive(rows_b, args.metric)), args.metric, args.results, args.compare)
    else:
        print_table(derived, args.metric)

    if args.csv:
        cols = ["id", "pass", "source", "group", "prim", "cycle", "fb", "tex", "filter", "flags",
                "w", "h", "k", "pixels", "net", "per_prim", "per_px", "drawn", "crc32"]
        with open(args.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(cols[:14] + [f"{args.metric}_net", "per_prim", "per_px", "drawn", "crc32"])
            for r in derived:
                w.writerow([r[c] if c not in ("per_px",) else ("" if r[c] is None else r[c]) for c in cols])
        print(f"# wrote {args.csv}", file=sys.stderr)


if __name__ == "__main__":
    main()
