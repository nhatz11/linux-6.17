#!/usr/bin/env python3
"""Per-workload arm summary for p78.sh (median across reps, vs the pv arm)."""
import sys, csv, statistics as st
out, wl = sys.argv[1], sys.argv[2]
rows = [r for r in csv.DictReader(open(out)) if r['workload'] == wl]
if not rows: sys.exit()
arms, order = {}, []
for r in rows:
    arms.setdefault(r['arm'], []).append(r)
    if r['arm'] not in order: order.append(r['arm'])
order = (['pv'] if 'pv' in order else []) + [a for a in order if a != 'pv']
med = lambda rs, k: st.median([float(x[k]) for x in rs])
base = med(arms['pv'], 'perf') if 'pv' in arms else None
# TIME-scored workloads: perf is seconds, lower is better
lower_better = wl in ('parsec_vips','parsec_dedup','tinyconfig','parsec_bodytrack',
                      'parsec_canneal','hackbench_pipe_thr')
print(f"  --- {wl} ---")
for a in order:
    rs = arms[a]; p = med(rs, 'perf')
    d = ""
    if base:
        pct = 100*(base-p)/base if lower_better else 100*(p-base)/base
        d = f"{pct:+7.2f}%"
    print(f"    {a:>8} n={len(rs)} perf={p:>12,.2f} {d:>9}  "
          f"migs={med(rs,'mig_n'):>8,.0f} cost={med(rs,'cost_mean_us'):7.2f}us "
          f"delay={med(rs,'delay_mean_us'):8.2f}us wait={med(rs,'wait_ns')/1e9:7.3f}s "
          f"g2rej={med(rs,'g2_reject'):>10,.0f} sc_max={med(rs,'sc_max'):>3.0f}")
