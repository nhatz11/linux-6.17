#!/usr/bin/env python3
"""Point 15 report: lock-acquisition rate per workload, and the stratified pick.

Two instruments, deliberately kept separate:
  contended/s  lock:contention_begin -- KERNEL spinlock contention only.
  holds/s      ivh_cs_prev_hold_hist -- acquisitions that formed a stamped MCS
               queue. A cross-check on the ranking.

CRITICAL: lock:contention_begin does NOT see userspace synchronisation.
PARSEC and NHextend synchronise via pthread mutex -> futex, so they read at or
below the idle background (~64/s) no matter how much they actually contend.
For those, a near-zero rate means "wrong instrument", NOT "no contention" --
parsec_ferret reads 44-59/s yet has a +15.21% recorded migration win. They are
reported separately and must not be placed on the same axis.
"""
import csv, statistics as st, collections, sys

IDLE = 64.0   # measured: 127 events in 2s on an idle box
USERSPACE = ("parsec_", "nhextend")

rows = collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    try:
        sec = float(r["seconds"]); c = float(r["contended"]); h = float(r["holds"])
    except (ValueError, KeyError):
        continue
    if sec > 0:
        rows[r["workload"]].append((sec, c / sec, h / sec))

def med(v, i):
    return st.median([x[i] for x in v])

kern, user = [], []
for n, v in rows.items():
    rec = (n, med(v, 0), med(v, 1), med(v, 2), len(v),
           max(x[1] for x in v) - min(x[1] for x in v))
    (user if n.startswith(USERSPACE) else kern).append(rec)

def show(title, rs, note=""):
    print(f"\n{'='*86}\n{title}\n{'='*86}")
    print(f"  {'workload':22}{'sec':>7}{'contended/s':>13}{'holds/s':>11}"
          f"{'c/h':>7}{'reps':>6}{'spread':>10}")
    for n, s, c, h, k, sp in sorted(rs, key=lambda x: -x[2]):
        ratio = c / h if h else float("inf")
        flag = ""
        if c < IDLE * 2:
            flag = "  <- at/below idle background"
        if s < 1:
            flag += "  <- run <1s"
        print(f"  {n:22}{s:7.1f}{c:13,.0f}{h:11,.0f}{ratio:7.1f}{k:6d}"
              f"{sp:10,.0f}{flag}")
    if note:
        print(note)

show("KERNEL-LOCK WORKLOADS -- stratification axis is valid here", kern)
show("USERSPACE-SYNC WORKLOADS -- axis INVALID, wrong instrument", user,
     "\n  These synchronise in userspace (pthread mutex -> futex).\n"
     "  lock:contention_begin cannot see it. Do NOT rank these by the numbers\n"
     "  above; they need a futex-rate instrument to be placed on a scale.")

if kern:
    ks = sorted(kern, key=lambda x: -x[2])
    lo, hi = ks[-1][2], ks[0][2]
    print(f"\n  kernel-group range: {lo:,.0f} -> {hi:,.0f} contended/s "
          f"({hi/lo if lo else 0:,.0f}x)")
    print("\n  STRATIFIED PICK (by contended/s, kernel group):")
    for label, sel in (("INTENSE", ks[:2]), ("MID", ks[len(ks)//2-1:len(ks)//2+1]),
                       ("INFREQUENT", ks[-2:])):
        for n, s, c, h, k, sp in sel:
            print(f"    {label:11} {n:22}{c:12,.0f}/s   ({s:.0f}s/run)")
