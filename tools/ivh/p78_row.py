#!/usr/bin/env python3
"""Emit one CSV row for p78.sh."""
import sys, re, os

(wl, arm, rep, val, met, t0, t1, migk, f0, f1, fbt, fsc, armval, mode, out) = sys.argv[1:16]
dur = float(t1) - float(t0)

def ctr(path):
    d = {}
    for l in open(path):
        m = re.match(r'\s*(\S+)\s*=\s*(\d+)', l)
        if m: d[m.group(1)] = int(m.group(2))
    return d
c0, c1 = ctr(f0), ctr(f1)
D = lambda k: c1.get(k, 0) - c0.get(k, 0)

bt = open(fbt).read() if os.path.exists(fbt) else ""
g  = lambda k: int(m.group(1)) if (m := re.search(rf'@{k}:\s*(\d+)', bt)) else 0
n_mig   = g('n_mig')
cost    = g('total_sum_ns')        # t_attach - t_eval : MIGRATION COST
delay   = g('delay_sum_ns')        # t_run    - t_attach: MIGRATION DELAY
n_commit= g('n_commit')

sc = open(fsc).read() if os.path.exists(fsc) else ""
m = re.search(r'samples=(\d+) max=(\d+)', sc)
sc_n, sc_max = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
atcap = 0.0
m = re.search(r'distribution: (\{.*\})', sc)
if m and sc_n:
    try:
        d = eval(m.group(1))
        cap = int(armval) if mode == 'p8' and armval != 'pv' else None
        if cap: atcap = 100.0 * sum(v for k, v in d.items() if k >= cap) / sc_n
    except Exception: pass

# perf: TIME workloads are scored by wall seconds (lower better)
perf = val if val not in ('', None) else f"{dur:.4f}"

row = [wl, arm, rep, perf, f"{dur:.3f}", n_mig, cost,
       f"{cost/n_mig/1000:.2f}" if n_mig else "0",
       delay, f"{delay/n_mig/1000:.2f}" if n_mig else "0",
       cost + delay,
       D('ivh_slowpath_wait_ns'), D('ivh_slowpath_wait_events'),
       D('ivh_node_spin_iters_sum') + D('ivh_node_spin_success_iters_sum'),
       D('ivh_head_spin_iters_sum') + D('ivh_head_spin_iters_bail_sum'),
       D('ivh_steal_imminent_time_left_reject'),
       D('ivh_steal_imminent_capacity_reject'),
       D('ivh_prelock_calls'), migk, sc_max, f"{atcap:.1f}",
       D('ivh_beat_tier1_fired'), D('ivh_beat_tier2_fired'),
       D('ivh_cs_head_bailed'), D('ivh_head_bypass_fired'), D('ivh_evict_marked')]
open(out, 'a').write(",".join(str(x) for x in row) + "\n")

flag = ""
if n_commit and n_mig and n_commit - n_mig > max(20, 0.01*n_commit): flag += " COMMIT-LEAK"
if arm != 'pv' and n_mig == 0: flag += " ZERO-MIGRATIONS"
if arm != 'pv' and D('ivh_head_bypass_fired') == 0: flag += " HB-DEAD"
if arm != 'pv' and D('ivh_beat_tier2_fired') == 0: flag += " T2-DEAD"
print(f"  {wl:20} {arm:>8} r{rep} perf={perf:>12} migs={n_mig:>7} "
      f"cost={cost/n_mig/1000 if n_mig else 0:7.2f}us delay={delay/n_mig/1000 if n_mig else 0:8.2f}us "
      f"wait={D('ivh_slowpath_wait_ns')/1e9:7.3f}s sc_max={sc_max:>2}{flag}")
