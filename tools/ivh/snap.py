#!/usr/bin/env python3
"""One-shot snapshot of the counters the spin-vs-halt decomposition needs.

Prints "key value" lines. Array counters are flattened to name.CAUSE, so
ivh_node_halt_cycles.TIER2 is directly addressable -- that cell is the whole
point: cycles a waiter spent HALTED because tier 2 bailed it out, i.e. wait
time that was converted from burned spinning into a yielded vCPU.
"""
import subprocess, sys, re

NAMES = [
    "ivh_slowpath_wait_ns", "ivh_slowpath_wait_events",
    "ivh_node_halt_cycles", "ivh_node_halt_events",
    "ivh_head_halt_cycles", "ivh_head_halt_events",
    "ivh_node_spin_iters_sum", "ivh_node_spin_attempts",
    "ivh_node_spin_success_iters_sum", "ivh_head_spin_iters_sum",
    "ivh_beat_tier1_fired", "ivh_beat_tier2_fired",
    "ivh_head_bypass_fired", "ivh_head_obs_actionable",
    "ivh_evict_marked", "ivh_evict_requeued", "ivh_evict_lookahead_refused",
    "ivh_evict_walks", "ivh_pv_wait_calls",
]
out = subprocess.run(
    ["python3", "/root/ivh_tools/read_ivh_counters.py"] + NAMES,
    capture_output=True, text=True).stdout
res = {}
for line in out.splitlines():
    m = re.match(r'^(\S+)\s+\[(\S+)\s*\]\s*=\s*(\d+)', line)
    if m:
        res[f"{m.group(1)}.{m.group(2)}"] = int(m.group(3)); continue
    m = re.match(r'^(\S+)\s*=\s*(\d+)', line)
    if m:
        res[m.group(1)] = int(m.group(2))
for k in sorted(res):
    print(k, res[k])
