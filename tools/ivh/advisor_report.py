#!/usr/bin/env python3
"""Advisor report: does the best combination always contain tier 2?

METRIC: on-CPU wait = wall - halted. Wall time alone scores an early-bail
mechanism as null, because converting spin into halt leaves wall unchanged
while moving the waiter off the vCPU. on-CPU wait is the vCPU actually HELD
while waiting -- the resource other threads lose.

  wall   = ivh_slowpath_wait_ns                                  (ns)
  halted = (node_halt_cycles.TOTAL + head_halt_cycles.TOTAL)/2.2 (ns @ 2200MHz)

TWO CLASSES, TWO TESTS. Pooling them would be dishonest -- a 0.1 s baseline and
a 50 s baseline do not deserve the same statistic.
  EFFECT      enough wait to resolve a reduction -> "how much did it fall"
  DO-NO-HARM  already manages wait well -> non-inferiority against a margin,
              reported in ABSOLUTE ms, because a percentage of a 0.1 s baseline
              turns ordinary jitter into an apparent large regression.
"""
import csv, sys, statistics as st, collections, math

DO_NO_HARM = {"parsec_vips", "sysbench_mutex"}
MARGIN_MS  = 20.0          # non-inferiority margin for the do-no-harm class
ARM_ORDER  = ["pv"] + [f"{c}@{t}" for c in ("t2","hbskip","all") for t in ("500us","1ms","2ms")]

def load(p):
    g = collections.defaultdict(list)
    for r in csv.DictReader(open(p)):
        try:
            g[(r['workload'], r['arm'])].append(dict(
                oncpu=int(r['wall_ns']) - int(r['halt_cyc'])/2.2,
                wall=int(r['wall_ns']), halt=int(r['halt_cyc'])/2.2,
                perf=float(r['perf']), t2=int(r['t2_fired']),
                hb=int(r['bypass_fired']), sk=int(r['evict_marked'])))
        except (ValueError, KeyError):
            continue
    return g

def welch(a, b):
    if len(a) < 2 or len(b) < 2: return 0.0
    va, vb = st.variance(a)/len(a), st.variance(b)/len(b)
    return 0.0 if va+vb == 0 else (st.mean(a)-st.mean(b))/math.sqrt(va+vb)

def main(path):
    g = load(path)
    ws = list(dict.fromkeys(k[0] for k in g))
    m = lambda v, f: st.mean([f(x) for x in v])

    winners = {}
    for cls, title in (("effect","EFFECT WORKLOADS -- how much did on-CPU wait fall"),
                       ("dnh",f"DO-NO-HARM CONTROLS -- non-inferiority, margin {MARGIN_MS:.0f} ms")):
        sel = [w for w in ws if (w in DO_NO_HARM) == (cls == "dnh")]
        if not sel: continue
        print("=" * 104); print(title); print("=" * 104)
        for w in sel:
            pv = g.get((w,'pv'))
            if not pv: continue
            po = m(pv, lambda x: x['oncpu']); pp = m(pv, lambda x: x['perf'])
            print(f"\n--- {w} ---  PV on-CPU wait {po/1e9:.3f}s   (n={len(pv)})")
            if cls == "dnh":
                print(f"  {'arm':>13}{'n':>3}{'on-CPU':>10}{'delta':>10}{'verdict':>16}{'perf':>9}")
            else:
                print(f"  {'arm':>13}{'n':>3}{'on-CPU':>10}{'vs PV':>9}{'t':>7}{'perf vs PV':>12}{'t2 fires':>11}{'hb':>7}{'skip':>7}")
            best = None
            for a in ARM_ORDER:
                v = g.get((w,a))
                if not v: continue
                o = m(v, lambda x: x['oncpu']); pf = m(v, lambda x: x['perf'])
                if a != 'pv' and (best is None or o < best[1]): best = (a, o)
                if cls == "dnh":
                    d = (o-po)/1e6
                    verdict = "" if a=='pv' else ("NON-INFERIOR" if d <= MARGIN_MS else "REGRESSION")
                    print(f"  {a:>13}{len(v):>3}{o/1e9:>9.3f}s{d:>+9.1f}ms{verdict:>16}{pf:>9,.1f}")
                else:
                    t = welch([x['oncpu'] for x in pv],[x['oncpu'] for x in v])
                    print(f"  {a:>13}{len(v):>3}{o/1e9:>9.2f}s{100*(o-po)/po:>+8.1f}%{t:>7.2f}"
                          f"{100*(pf-pp)/pp:>+11.2f}%{m(v,lambda x:x['t2']):>11,.0f}"
                          f"{m(v,lambda x:x['hb']):>7,.0f}{m(v,lambda x:x['sk']):>7,.0f}")
            if best: winners[w] = best[0]

    print("\n" + "=" * 104)
    print("THE CLAIM: does the best arm always contain tier 2?")
    print("=" * 104)
    if not winners:
        print("  no completed workloads yet"); return
    for w, a in winners.items():
        has = "t2" in a or a.startswith("all")
        print(f"  {w:22} best arm = {a:<14} {'contains t2' if has else 'NO t2  <-- COUNTEREXAMPLE'}")
    bad = [w for w,a in winners.items() if not ("t2" in a or a.startswith("all"))]
    print()
    if bad:
        print(f"  CLAIM FAILS on {len(bad)}/{len(winners)}: {', '.join(bad)}")
    else:
        print(f"  CLAIM HOLDS on all {len(winners)} workloads measured so far.")
    print("\n  Note: 'best' is the lowest mean on-CPU wait. Where two arms are within")
    print("  noise the ranking is not meaningful -- read the t column before claiming")
    print("  one combination beats another.")

if __name__ == "__main__":
    main(sys.argv[1])
