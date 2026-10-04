#!/usr/bin/env python3
"""Point 11 report: lock-skipping staleness threshold sweep.

Two metrics, per the agreed scope -- performance and total wait. No p99.

  perf        direction-corrected, higher is better; shown as % vs the PV arm
  wait_s      ivh_slowpath_wait_ns, aggregate spinlock wait across all vCPUs.
              THE metric: the expectation is that wait falls, not that
              throughput rises.
  wait/acq    wait_ns / wait_events. Reported alongside total wait because
              total wait is only directly comparable on FIXED-WORK benchmarks
              (hackbench -l150000, parsec, sysbench --events). On a TIME-BOXED
              benchmark (ebizzy -S, dbench -t) a faster arm does more
              acquisitions and therefore accrues more total wait even when each
              acquisition waits less -- total wait would then read as a
              regression when it is the opposite. Per-acquisition wait is
              immune to that; where the two disagree, per-acquisition is right.

Firing evidence (marked / requeued / lookahead_refused) is printed per arm so a
flat result cannot be confused with a knob that never engaged.

Migration is off in every arm by construction; migs is printed only as a check
that the eligibility gate held.
"""
import csv, sys, statistics as st, math

FIXED_WORK = {"hackbench_pipe_thr", "parsec_dedup", "parsec_vips", "sysbench_mutex"}

def lab(a):
    if a == 0: return "PV"
    us = a // 2200
    return f"{us}us" if us < 1000 else f"{us//1000}ms"

def welch(a, b):
    """t and two-sided p for unequal variance. Returns (t, p) or (0,1)."""
    if len(a) < 2 or len(b) < 2: return 0.0, 1.0
    ma, mb = st.mean(a), st.mean(b)
    va, vb = st.variance(a)/len(a), st.variance(b)/len(b)
    if va + vb == 0: return 0.0, 1.0
    t = (ma - mb) / math.sqrt(va + vb)
    df = (va+vb)**2 / ((va**2/(len(a)-1)) + (vb**2/(len(b)-1))) if (va and vb) else len(a)+len(b)-2
    # normal approximation to the two-sided p (df here is 6-10, so this is
    # slightly optimistic; treat |t| >= 2.5 as the real bar)
    p = math.erfc(abs(t)/math.sqrt(2))
    return t, p

def load(path):
    rows = []
    for r in csv.DictReader(open(path)):
        try:
            rows.append(dict(
                w=r['workload'], arm=int(r['arm_cyc']), rep=int(r['rep']),
                perf=float(r['perf']), dur=float(r['dur_s']),
                wait=int(r['wait_ns']), ev=int(r['wait_events']),
                mk=int(r['ev_marked']), rq=int(r['ev_requeued']),
                lr=int(r['ev_lookahead_ref']), migs=int(r['migs'])))
        except (ValueError, KeyError):
            continue
    return rows

def main(path):
    rows = load(path)
    if not rows:
        print("no rows"); return
    ws = list(dict.fromkeys(r['w'] for r in rows))
    arms = sorted({r['arm'] for r in rows if r['arm']})
    leak = sum(r['migs'] for r in rows)
    print(f"{len(rows)} rows from {path}")
    print(f"migration leak check: {leak} migrations total "
          f"({'OK -- gate held' if leak == 0 else 'LEAKED, sweep confounded'})\n")

    # ---------- PER WORKLOAD ----------
    for w in ws:
        sub = [r for r in rows if r['w'] == w]
        pv = [r for r in sub if r['arm'] == 0]
        if not pv: continue
        fixed = w in FIXED_WORK
        pvp = [r['perf'] for r in pv]
        pvw = [r['wait']/1e9 for r in pv]
        pva = [r['wait']/max(r['ev'],1) for r in pv]
        print("=" * 112)
        print(f"{w}   [{'fixed-work' if fixed else 'TIME-BOXED -- read wait/acq, not total wait'}]")
        print(f"  PV (n={len(pv)}): perf {st.mean(pvp):,.1f}  wait {st.mean(pvw):.2f}s  "
              f"wait/acq {st.mean(pva):,.0f}ns")
        print(f"  {'arm':>7}{'n':>3}{'perf':>11}{'vs PV':>9}{'t':>7}"
              f"{'wait_s':>9}{'vs PV':>9}{'t':>7}{'wait/acq':>11}{'vs PV':>9}"
              f"{'marked':>9}{'la_ref':>9}")
        for a in arms:
            use = [r for r in sub if r['arm'] == a]
            if not use: continue
            ap = [r['perf'] for r in use]
            aw = [r['wait']/1e9 for r in use]
            aa = [r['wait']/max(r['ev'],1) for r in use]
            tp, _ = welch(ap, pvp)
            tw, _ = welch(pvw, aw)          # positive t == IVH waits LESS
            print(f"  {lab(a):>7}{len(use):>3}{st.mean(ap):>11,.1f}"
                  f"{100*(st.mean(ap)-st.mean(pvp))/st.mean(pvp):>8.2f}%{tp:>7.2f}"
                  f"{st.mean(aw):>9.2f}{100*(st.mean(aw)-st.mean(pvw))/st.mean(pvw):>8.2f}%{tw:>7.2f}"
                  f"{st.mean(aa):>11,.0f}"
                  f"{100*(st.mean(aa)-st.mean(pva))/st.mean(pva):>8.2f}%"
                  f"{st.mean([r['mk'] for r in use]):>9,.0f}"
                  f"{st.mean([r['lr'] for r in use]):>9,.0f}")
        print()

    # ---------- POOLED ----------
    print("=" * 112)
    print("POOLED -- each workload contributes its own % change, so no single one dominates.")
    print("Pooled t is across the per-workload % changes (n = workloads), testing 'the knob")
    print("moves this metric in a consistent direction', not 'it moves it a lot on one run'.")
    print("=" * 112)
    print(f"  {'arm':>7}{'nw':>4}{'perf vs PV':>13}{'t':>7}{'wait vs PV':>13}{'t':>7}"
          f"{'wait/acq vs PV':>17}{'t':>7}{'marked/run':>12}")
    for a in arms:
        dp, dw, da = [], [], []
        mk = []
        for w in ws:
            sub = [r for r in rows if r['w'] == w]
            pv = [r for r in sub if r['arm'] == 0]
            use = [r for r in sub if r['arm'] == a]
            if not pv or not use: continue
            f = lambda rs, k: st.mean([k(r) for r in rs])
            pp = f(pv, lambda r: r['perf']); ap = f(use, lambda r: r['perf'])
            pw = f(pv, lambda r: r['wait']); aw = f(use, lambda r: r['wait'])
            pa = f(pv, lambda r: r['wait']/max(r['ev'],1))
            aa = f(use, lambda r: r['wait']/max(r['ev'],1))
            dp.append(100*(ap-pp)/pp); dw.append(100*(aw-pw)/pw); da.append(100*(aa-pa)/pa)
            mk.append(f(use, lambda r: r['mk']))
        if not dp: continue
        def tt(v):
            return st.mean(v)/(st.stdev(v)/math.sqrt(len(v))) if len(v) > 1 and st.stdev(v) else 0.0
        print(f"  {lab(a):>7}{len(dp):>4}{st.mean(dp):>12.2f}%{tt(dp):>7.2f}"
              f"{st.mean(dw):>12.2f}%{tt(dw):>7.2f}{st.mean(da):>16.2f}%{tt(da):>7.2f}"
              f"{st.mean(mk):>12,.0f}")
    print("\n  Sign convention: perf positive = faster. wait negative = less spinning.")
    print("  |t| >= 2.57 is the two-sided 5% bar at these degrees of freedom.")

if __name__ == "__main__":
    main(sys.argv[1])
