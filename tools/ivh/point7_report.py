#!/usr/bin/env python3
"""Point 7 report: raw rows and per-arm averages, four metrics kept SEPARATE.

Metrics, none combined into a ratio here (that is done downstream by hand):
  perf         direction-corrected, higher is better; shown as % vs the PV arm
  ipi/kwork    RES+CAL+TLB per 1000 units of work (work = perf x duration).
               Dividing by perf alone is wrong for fixed-work benchmarks --
               it double-counts the speedup.
  wait_s       aggregate spinlock wait (ivh_slowpath_wait_ns)
  mig_cost_ms  stopper dispatch + the actual move. NO runqueue wait.
  mig_rqwait_s logged as context only; it is a property of how loaded the
               target vCPU is (~1 EEVDF base slice, 2.8 ms here), not a cost of
               the migration mechanism.

CAPACITY SETTLING: the PV arm perturbs ivh_uc_capacity for ~2-3 runs after it,
during which Gate 1 passes everything. Those rows are NOT comparable and are
excluded from the averages by default (g1_reject below SETTLED_MIN), but are
still counted and reported so the exclusion is visible.
"""
import csv, sys, statistics as st, collections

SETTLED_MIN = 50_000          # g1_reject below this => capacity not settled

def load(path):
    rows = []
    for r in csv.DictReader(open(path)):
        try:
            rows.append(dict(
                w=r['workload'], arm=int(r['arm_ns']), rep=int(r['rep']),
                perf=float(r['perf']), dur=float(r['dur_s']), ipi=int(r['ipi']),
                wait=int(r['wait_ns']), migs=int(r['migs']), mig_n=int(r['mig_n']),
                cost=int(r['mig_cost_ns']), rqw=int(r['mig_rqwait_ns']),
                g1=int(r['g1_reject'])))
        except (ValueError, KeyError):
            continue
    return rows

def lab(a):
    return "PV" if a == 0 else (f"{a//1000}us" if a < 1_000_000 else f"{a//1_000_000}ms")

def main(path):
    rows = load(path)
    if not rows:
        print("no rows"); return
    print(f"{len(rows)} rows from {path}\n")

    # ---------- RAW ----------
    print("=" * 108)
    print("RAW")
    print("=" * 108)
    for w in dict.fromkeys(r['w'] for r in rows):
        print(f"\n--- {w} ---")
        print(f"  {'arm':>7}{'rep':>4}{'perf':>12}{'dur_s':>8}{'wait_s':>9}"
              f"{'migs':>8}{'cost_ms':>9}{'us/mig':>8}{'rqwait_s':>10}{'g1':>10}  settled")
        for r in sorted((x for x in rows if x['w'] == w), key=lambda x: (x['arm'], x['rep'])):
            per = r['cost'] / r['mig_n'] / 1000 if r['mig_n'] else 0.0
            ok = "" if r['arm'] == 0 else ("yes" if r['g1'] >= SETTLED_MIN else "NO")
            print(f"  {lab(r['arm']):>7}{r['rep']:>4}{r['perf']:>12,.1f}{r['dur']:>8.2f}"
                  f"{r['wait']/1e9:>9.2f}{r['migs']:>8,}{r['cost']/1e6:>9.1f}{per:>8.1f}"
                  f"{r['rqw']/1e9:>10.2f}{r['g1']:>10,}  {ok}")

    # ---------- AVERAGED ----------
    print("\n" + "=" * 108)
    print(f"AVERAGED (medians; IVH arms with g1_reject < {SETTLED_MIN:,} excluded as capacity-unsettled)")
    print("=" * 108)
    for w in dict.fromkeys(r['w'] for r in rows):
        sub = [r for r in rows if r['w'] == w]
        pv = [r for r in sub if r['arm'] == 0]
        if not pv:
            continue
        pv_perf = st.median([r['perf'] for r in pv])
        pv_wait = st.median([r['wait'] for r in pv])
        pv_ipik = st.median([r['ipi'] / max(r['perf'] * r['dur'] / 1000, 1e-9) for r in pv])
        print(f"\n--- {w} ---   PV: perf {pv_perf:,.1f}  wait {pv_wait/1e9:.2f}s")
        print(f"  {'arm':>7}{'n':>3}{'perf':>12}{'vs PV':>9}{'ipi/kwork':>11}{'vs PV':>9}"
              f"{'wait_s':>9}{'saved_s':>9}{'migs':>8}{'cost_ms':>9}{'us/mig':>8}{'drop':>6}")
        arms = sorted({r['arm'] for r in sub if r['arm']})
        for a in arms:
            allr = [r for r in sub if r['arm'] == a]
            use = [r for r in allr if r['g1'] >= SETTLED_MIN]
            dropped = len(allr) - len(use)
            if not use:
                print(f"  {lab(a):>7}{0:>3}   -- all {len(allr)} reps capacity-unsettled --")
                continue
            perf = st.median([r['perf'] for r in use])
            ipik = st.median([r['ipi'] / max(r['perf'] * r['dur'] / 1000, 1e-9) for r in use])
            wait = st.median([r['wait'] for r in use])
            migs = st.median([r['migs'] for r in use])
            cost = st.median([r['cost'] for r in use])
            mign = st.median([r['mig_n'] for r in use])
            per = cost / mign / 1000 if mign else 0.0
            print(f"  {lab(a):>7}{len(use):>3}{perf:>12,.1f}{100*(perf-pv_perf)/pv_perf:>8.1f}%"
                  f"{ipik:>11,.0f}{100*(ipik-pv_ipik)/pv_ipik:>8.1f}%"
                  f"{wait/1e9:>9.2f}{(pv_wait-wait)/1e9:>9.2f}{migs:>8,.0f}"
                  f"{cost/1e6:>9.1f}{per:>8.1f}{dropped:>6}")

    # ---------- POOLED ----------
    print("\n" + "=" * 108)
    print("POOLED ACROSS WORKLOADS -- normalised per workload so no single one dominates")
    print("=" * 108)
    arms = sorted({r['arm'] for r in rows if r['arm']})
    print(f"  {'arm':>7}{'n':>4}{'perf vs PV':>13}{'ipi vs PV':>12}{'spin saved':>13}"
          f"{'mig cost':>12}{'us/mig':>9}")
    for a in arms:
        rel_p, rel_i, sav, cst, per = [], [], [], [], []
        for w in dict.fromkeys(r['w'] for r in rows):
            sub = [r for r in rows if r['w'] == w]
            pv = [r for r in sub if r['arm'] == 0]
            use = [r for r in sub if r['arm'] == a and r['g1'] >= SETTLED_MIN]
            if not pv or not use:
                continue
            pp = st.median([r['perf'] for r in pv])
            pw = st.median([r['wait'] for r in pv])
            pi = st.median([r['ipi'] / max(r['perf'] * r['dur'] / 1000, 1e-9) for r in pv])
            ap = st.median([r['perf'] for r in use])
            aw = st.median([r['wait'] for r in use])
            ai = st.median([r['ipi'] / max(r['perf'] * r['dur'] / 1000, 1e-9) for r in use])
            ac = st.median([r['cost'] for r in use])
            an_ = st.median([r['mig_n'] for r in use])
            rel_p.append(100 * (ap - pp) / pp)
            rel_i.append(100 * (ai - pi) / pi)
            sav.append((pw - aw) / 1e9)
            cst.append(ac / 1e6)
            if an_: per.append(ac / an_ / 1000)
        if rel_p:
            print(f"  {lab(a):>7}{len(rel_p):>4}{st.median(rel_p):>12.1f}%{st.median(rel_i):>11.1f}%"
                  f"{st.median(sav):>12.2f}s{st.median(cst):>11.1f}ms{st.median(per):>9.1f}")
    print("\n  Nothing above is a ratio. spin saved and mig cost are in the same units (time)")
    print("  and can be combined downstream however the argument requires.")

if __name__ == "__main__":
    main(sys.argv[1])
