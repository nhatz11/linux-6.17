#!/usr/bin/env python3
"""
Point 7 with vcap capacity + EWMA active time.

Reports throughput AND Gate 2 firing rate per threshold, because #7's
original failure mode was ambiguous: flat throughput could mean the knob is
dead OR that rejection does not drive throughput. G-LOCK-50 showed the
latter for last_active. Only the pair of curves distinguishes them.
"""
import sys, os, statistics, collections

out = sys.argv[1]
KIND = {"memtier_memcached": "hi", "hackbench_pipe_thr": "lo",
        "ebizzy_mmap": "hi", "dbench_16": "hi",
        "nhextend_full": "hi", "parsec_vips": "lo"}
ORDER = ["memtier_memcached", "hackbench_pipe_thr", "ebizzy_mmap",
         "dbench_16", "nhextend_full", "parsec_vips"]

vals = collections.defaultdict(list)
gate = collections.defaultdict(lambda: [0, 0, 0])
with open(os.path.join(out, "raw.tsv")) as f:
    next(f)
    for ln in f:
        p = ln.rstrip("\n").split("\t")
        if len(p) < 7 or p[3] == "NA":
            continue
        arm, b, _r, v, ev, fi, mg = p[0], p[1], p[2], p[3], p[4], p[5], p[6]
        try:
            vals[(arm, b)].append(float(v))
        except ValueError:
            continue
        g = gate[arm]
        g[0] += int(ev); g[1] += int(fi); g[2] += int(mg)

arms = [a for a in ["pv"] + sorted((x for x in {k[0] for k in vals} if x != "pv"),
                                   key=int) if any((a, b) in vals for b in ORDER)]
benches = [b for b in ORDER if any((a, b) in vals for a in arms)]

print(f"{'arm':>10} {'fire%':>7} {'migs':>9} " + " ".join(f"{b[:13]:>14}" for b in benches))
for a in arms:
    ev, fi, mg = gate[a]
    fr = f"{100.0*fi/ev:>6.2f}%" if ev else f"{'-':>7}"
    row = f"{(a if a=='pv' else '%.2fms'%(int(a)/1e6)):>10} {fr:>7} {mg:>9} "
    for b in benches:
        d = vals.get((a, b))
        row += f"{statistics.median(d):>14.2f} " if d else f"{'-':>14} "
    print(row)

print(f"\n%% vs PV   (TIME -> % time saved;  THROUGHPUT -> % gain)")
print(f"{'arm':>10} " + " ".join(f"{b[:13]:>14}" for b in benches))
for a in arms:
    if a == "pv":
        continue
    row = f"{'%.2fms'%(int(a)/1e6):>10} "
    for b in benches:
        d, p = vals.get((a, b)), vals.get(("pv", b))
        if not d or not p:
            row += f"{'-':>14} "; continue
        m, pv = statistics.median(d), statistics.median(p)
        pct = (pv-m)/pv*100 if KIND[b] == "lo" else (m-pv)/pv*100
        row += f"{pct:>+13.2f}% "
    print(row)

print("\n=== SENSITIVITY: does the threshold move anything? ===")
swept = [a for a in arms if a != "pv"]
ev_rates = [100.0*gate[a][1]/gate[a][0] for a in swept if gate[a][0]]
if ev_rates:
    lo, hi = min(ev_rates), max(ev_rates)
    print(f"  {'gate firing':<20}{lo:>7.2f}% -> {hi:<7.2f}%"
          + (f"  ({hi/lo:.1f}x)" if lo else ""))
for b in benches:
    v = [statistics.median(vals[(a, b)]) for a in swept if (a, b) in vals]
    if len(v) < 2:
        continue
    lo, hi = min(v), max(v)
    span = (hi-lo)/lo*100
    tag = "FLAT" if span < 3 else ("RESPONDS" if span > 8 else "weak")
    cvs = [100*statistics.stdev(vals[(a, b)])/statistics.mean(vals[(a, b)])
           for a in swept if len(vals.get((a, b), [])) > 1]
    cv = max(cvs) if cvs else 0
    warn = "  *** span < worst CV, NOT significant ***" if span < cv else ""
    print(f"  {b:<20}{lo:>9.2f} -> {hi:<9.2f} ({span:+6.1f}%)  worstCV={cv:4.1f}%  {tag}{warn}")
