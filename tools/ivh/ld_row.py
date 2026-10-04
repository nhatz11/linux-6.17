#!/usr/bin/env python3
"""Emit one ladder CSV row. Single implementation, used by both the main loop
and the adaptive-extension loop so the two cannot drift apart."""
import sys, re

w, a, r, v, dr, t0, t1, migs, f0, f1, out, nspi = sys.argv[1:13]

def load(p):
    d = {}
    for line in open(p):
        m = re.match(r'^(\S+)\s+\[(\S+)\s*\]\s*=\s*(\d+)', line)
        if m:
            d[f"{m.group(1)}.{m.group(2)}"] = int(m.group(3)); continue
        m = re.match(r'^(\S+)\s*=\s*(\d+)', line)
        if m:
            d[m.group(1)] = int(m.group(2))
    return d

b, e = load(f0), load(f1)
D = lambda k: e.get(k, 0) - b.get(k, 0)
dur = float(t1) - float(t0)
val = float(v)
perf = (1000.0 / val if val > 0 else 0.0) if dr == "lo" else val

# Complete node spin: the give-up pair (:2158) plus the acquire-during-spin
# pair (:1946). GLOCK-11's comment at :1931 says these exist to be summed.
it = D('ivh_node_spin_iters_sum') + D('ivh_node_spin_success_iters_sum')
at = D('ivh_node_spin_attempts') + D('ivh_node_spin_success_attempts')
spin_ns = it * float(nspi)

open(out, 'a').write(
    f"{w},{a},{r},{perf:.4f},{dur:.3f},{it},{at},{spin_ns:.0f},"
    f"{D('ivh_slowpath_wait_ns')},{D('ivh_slowpath_wait_events')},{migs},"
    f"{D('ivh_beat_tier1_fired')},{D('ivh_beat_tier2_fired')},{D('ivh_cs_head_bailed')},"
    f"{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')}\n")

print(f"  {w:18} {a:>20} r{r} perf={perf:10,.1f} SPIN={spin_ns/1e9:7.2f}s ({it/1e6:8,.0f}M it) "
      f"mig={int(migs):6,} t1={D('ivh_beat_tier1_fired'):8,} t2={D('ivh_beat_tier2_fired'):7,} "
      f"heh={D('ivh_cs_head_bailed'):6,} hb={D('ivh_head_bypass_fired'):5,} sk={D('ivh_evict_marked'):5,}")
