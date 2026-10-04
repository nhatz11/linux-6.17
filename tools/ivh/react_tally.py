#!/usr/bin/env python3
"""Tally one arm of the G-LOCK-46b react measurement, or summarise many.

Per-run mode:   react_tally.py <arm> <before> <after>   -> one TSV line
Summary mode:   react_tally.py --summary < raw          -> pooled + spread

ivh_cs_react[had_tail][state][bucket] is flattened by the counter reader to
6 rows of 32: row = had_tail * 3 + state.
  row 3 = queue present, head still spinning
  row 4 = queue present, head halted ON ITS OWN        <- numerator
  row 5 = queue present, WE latched _Q_SLOW_VAL        <- excluded
Rows 0-2 are had_tail == 0: no queue existed, so no head could react and the
hold does not belong in the denominator.
"""
import re
import statistics as st
import sys

LO, HI = 20, 32          # buckets >= 476.6 us at 2200 MHz


def rows(s):
    return [{int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', p)}
            for p in s.split('|') if p.strip()]


def one(arm, a_raw, b_raw):
    a, b = rows(a_raw), rows(b_raw)
    while len(a) < 6:
        a.append({})
    while len(b) < 6:
        b.append({})
    d = [{k: b[i].get(k, 0) - a[i].get(k, 0) for k in set(a[i]) | set(b[i])}
         for i in range(6)]

    def S(idx, lo, hi):
        return sum(max(d[idx].get(k, 0), 0) for k in range(lo, hi))

    spin, halted, self_ = S(3, LO, HI), S(4, LO, HI), S(5, LO, HI)
    n = spin + halted
    sh_spin, sh_halt = S(3, 0, LO), S(4, 0, LO)
    shp = (100.0 * sh_halt / (sh_spin + sh_halt)) if (sh_spin + sh_halt) else 0.0
    pct = (100.0 * halted / n) if n else float('nan')
    print(f"{arm}\t{n}\t{halted}\t{self_}\t{shp:.2f}\t{pct:.1f}")


def summary():
    data = {}
    for ln in sys.stdin:
        f = ln.split('\t')
        if len(f) < 6:
            continue
        data.setdefault(f[0], []).append((int(f[1]), int(f[2]), int(f[3]), float(f[4])))
    order = [k for k in ("PV", "bail0", "bail1") if k in data]
    label = {"PV": "stock PV (mode=0)", "bail0": "IVH, CS detect-only",
             "bail1": "IVH, CS acts"}
    print(f"{'arm':22} {'n':>6} {'halted':>7} {'pooled':>8} {'per-run mean':>13} "
          f"{'sd':>6} {'range':>13}  {'short':>6}")
    pooled = {}
    for k in order:
        runs = data[k]
        n = sum(r[0] for r in runs)
        h = sum(r[1] for r in runs)
        per = [100.0 * r[1] / r[0] for r in runs if r[0]]
        pooled[k] = 100.0 * h / n if n else float('nan')
        sd = st.stdev(per) if len(per) > 1 else 0.0
        sh = max(r[3] for r in runs)
        print(f"{label[k]:22} {n:6d} {h:7d} {pooled[k]:7.1f}% {st.mean(per):12.1f}% "
              f"{sd:6.1f} {min(per):5.1f}-{max(per):<5.1f}  {sh:5.2f}%")
    print()
    if "PV" in pooled and "bail1" in pooled:
        runs_pv = [100.0 * r[1] / r[0] for r in data["PV"] if r[0]]
        runs_b1 = [100.0 * r[1] / r[0] for r in data["bail1"] if r[0]]
        d = pooled["bail1"] - pooled["PV"]
        noise = max(st.stdev(runs_pv) if len(runs_pv) > 1 else 0,
                    st.stdev(runs_b1) if len(runs_b1) > 1 else 0)
        verdict = "RESOLVED" if abs(d) > 2 * noise else "NOT RESOLVED (delta < 2 sd)"
        print(f"  IVH+CS vs stock PV : {d:+.1f} points   [{verdict}]")
    if "PV" in pooled and "bail0" in pooled:
        print(f"  IVH detect-only vs stock PV : {pooled['bail0']-pooled['PV']:+.1f} points")
    if "bail0" in pooled and "bail1" in pooled:
        print(f"  CS acting vs detect-only    : {pooled['bail1']-pooled['bail0']:+.1f} points")
    print("\n  short = max short-hold halted%% across runs; must stay ~0 or the")
    print("  signal is queue depth, not preemption.")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--summary":
        summary()
    else:
        one(sys.argv[1], sys.argv[2], sys.argv[3])
