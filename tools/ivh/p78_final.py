#!/usr/bin/env python3
"""Cross-workload analysis for points 7 and 8.

Reports, per arm, pooled across workloads:
  - performance vs the pv baseline (sign-corrected: TIME workloads are inverted)
  - migration cost / delay per migration
  - TOTAL time spent in migration  = migs * (cost + delay)
  - spin wait (ivh_slowpath_wait_ns), normalised per unit of work
  - the swept gate's own counter, to expose degenerate arms
"""
import sys, csv, statistics as st

LOWER_BETTER = {'parsec_vips','parsec_dedup','tinyconfig','parsec_bodytrack',
                'parsec_canneal','hackbench_pipe_thr'}

def load(paths):
    rows=[]
    for p in paths: rows += list(csv.DictReader(open(p)))
    return rows

def main(paths):
    rows = load(paths)
    wls, arms = [], []
    for r in rows:
        if r['workload'] not in wls: wls.append(r['workload'])
        if r['arm'] not in arms: arms.append(r['arm'])
    arms = (['pv'] if 'pv' in arms else []) + [a for a in arms if a!='pv']
    key = lambda w,a: [r for r in rows if r['workload']==w and r['arm']==a]
    med = lambda rs,k: st.median([float(x[k]) for x in rs]) if rs else float('nan')

    print("="*118)
    print("PER-WORKLOAD PERFORMANCE vs stock PV  (+ = better, sign-corrected)")
    print("="*118)
    print(f"{'workload':22}" + "".join(f"{a:>13}" for a in arms[1:]))
    print("-"*118)
    deltas={a:[] for a in arms[1:]}
    for w in wls:
        pv = key(w,'pv')
        if not pv: continue
        b = med(pv,'perf'); line=f"{w:22}"
        for a in arms[1:]:
            rs=key(w,a)
            if not rs: line+=f"{'-':>13}"; continue
            p=med(rs,'perf')
            d=100*(b-p)/b if w in LOWER_BETTER else 100*(p-b)/b
            deltas[a].append(d); line+=f"{d:>+12.2f}%"
        print(line)
    print("-"*118)
    print(f"{'MEDIAN':22}" + "".join(f"{st.median(deltas[a]):>+12.2f}%" if deltas[a] else f"{'-':>13}" for a in arms[1:]))

    for label,fn in [("MIGRATION COST per migration (us)", lambda rs: med(rs,'cost_mean_us')),
                     ("MIGRATION DELAY per migration (us)", lambda rs: med(rs,'delay_mean_us')),
                     ("TOTAL TIME IN MIGRATION (s)  = migs x (cost+delay)",
                      lambda rs: med(rs,'mig_total_ns')/1e9),
                     ("SPIN WAIT (s, ivh_slowpath_wait_ns)", lambda rs: med(rs,'wait_ns')/1e9),
                     ("MIGRATIONS", lambda rs: med(rs,'mig_n')),
                     ("GATE-2 REJECTS (degenerate if flat between arms)", lambda rs: med(rs,'g2_reject')),
                     ("MAX OBSERVED CONCURRENCY (sc_max)", lambda rs: med(rs,'sc_max'))]:
        print("\n"+"="*118); print(label); print("="*118)
        print(f"{'workload':22}" + "".join(f"{a:>13}" for a in arms))
        print("-"*118)
        for w in wls:
            line=f"{w:22}"
            for a in arms:
                rs=key(w,a)
                line += f"{fn(rs):>13,.2f}" if rs else f"{'-':>13}"
            print(line)

if __name__=='__main__': main(sys.argv[1:])
