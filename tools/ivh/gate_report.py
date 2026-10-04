#!/usr/bin/env python3
"""Summarise a gate sweep: performance, IPI traffic, migrations, preempted CS.

Validity first. A gate knob that does not move its own reject counter is
inert, and every throughput column below it is then measuring noise -- that
is how the head-bypass "shut gate" result survived as long as it did. So the
monotonicity of migs and g2rej is reported before any verdict is drawn.
"""
import os
import statistics as st
import sys

knob = os.environ.get("KNOB", "knob")
rows = {}
for ln in open(sys.argv[1]):
    f = ln.split('\t')
    if len(f) < 8:
        continue
    rows.setdefault(f[0], []).append(
        dict(files=float(f[1]), migs=int(f[2]), g2=int(f[3]),
             share=float(f[4]), lng=int(f[5]), tot=int(f[6]), ipi=int(f[7])))

order = (["pv"] if "pv" in rows else []) + sorted(
    (k for k in rows if k != "pv"), key=int)


def med(k, f):
    return st.median([r[f] for r in rows[k]])


print(f"{knob:>14} {'files/s':>10} {'vs pv':>8} {'migs':>7} {'g2rej':>9} "
      f"{'long%':>7} {'IPI/kfile':>10}")
base = med("pv", "files") if "pv" in rows else None
for k in order:
    v = med(k, "files")
    rel = f"{100*(v-base)/base:+.1f}%" if base else "--"
    ipik = med(k, "ipi") / (v / 1000.0) if v else 0
    label = "PV" if k == "pv" else (f"{int(k)/1e6:g}ms" if int(k) >= 100000 else k)
    print(f"{label:>14} {v:10.0f} {rel:>8} {med(k,'migs'):7.0f} "
          f"{med(k,'g2'):9.0f} {med(k,'share'):7.3f} {ipik:10.0f}")

sw = [k for k in order if k != "pv"]
if len(sw) > 2:
    m = [med(k, "migs") for k in sw]
    g = [med(k, "g2") for k in sw]
    up = all(m[i] <= m[i+1] for i in range(len(m)-1))
    dn = all(m[i] >= m[i+1] for i in range(len(m)-1))
    print(f"\n  VALIDITY: migs across sweep = {[int(x) for x in m]}")
    print(f"            g2rej            = {[int(x) for x in g]}")
    if max(m) == min(m):
        print("            *** KNOB INERT -- migration count does not respond.")
        print("            *** Throughput differences above are NOT attributable")
        print("            *** to this knob. Do not pick a value from them.")
    elif up or dn:
        print(f"            monotonic ({'rising' if up else 'falling'}) -- "
              f"knob is live, {max(m)/max(min(m),1):.1f}x span.")
    else:
        print("            NON-MONOTONIC -- expected rising with threshold "
              "(higher = rejects less = migrates more).")
        print("            Treat the optimum as unresolved until explained.")

    best = max(sw, key=lambda k: med(k, "files"))
    spread = [med(k, "files") for k in sw]
    allv = [r["files"] for k in sw for r in rows[k]]
    noise = st.stdev(allv) if len(allv) > 1 else 0
    bl = f"{int(best)/1e6:g}ms" if int(best) >= 100000 else best
    print(f"\n  best throughput at {bl} "
          f"({med(best,'files'):.0f} files/s)")
    print(f"  spread across thresholds = {max(spread)-min(spread):.0f} files/s, "
          f"pooled sd = {noise:.0f}")
    if max(spread) - min(spread) < noise:
        print("  -> spread is INSIDE the noise: this knob does not pick a "
              "winner on throughput.")
