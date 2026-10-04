#!/bin/bash
# G-LOCK-45: recall of is_cs_preempted(), bucketed by hold duration.
#
#   ivh_cs_hold_by_flag[0][b] = holds of duration-bucket b NOT flagged
#   ivh_cs_hold_by_flag[1][b] = holds of bucket b that WERE flagged
#   recall(b) = [1][b] / ([0][b] + [1][b])
#
# Both rows from one site, one population, one run. Only contended
# acquisitions are stamped, so every hold counted here had a queue.
#
# Usage: recall45.sh [floor_cycles]   (default 1100000 = 500us)
set -u
S=/proc/sys/kernel
FLOOR="${1:-1100000}"
set_(){ echo "$2" > $S/$1 2>/dev/null; [ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"; }

set_ ivh_cs_recall_hist 1          # the new counter
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0
set_ ivh_cs_criterion 1;     set_ ivh_cs_noise_cycles "$FLOOR"
set_ ivh_pv_rot_enable 0;    set_ ivh_cs_verdict 1   # REQUIRED: the verdict block owns the clear of ivh_cs_flagged_acq.
                             # With it off, stale deposits persist and dep==tsc never matches.
set_ ivh_tks_sampler_ns 0    # verdict computed but unused here; sampler not needed
set_ ivh_adaptive_mode 2;    set_ ivh_pv_beat_threshold 11000000
set_ ivh_pv_tier1_enable 1;  set_ ivh_pv_tier2_enable 1
echo "floor=$(( FLOOR / 2200 ))us  (is_cs_preempted fires when held > last_cs + floor)"

R="python3 /root/ivh_tools/read_ivh_counters.py"
get(){ $R ivh_cs_hold_by_flag 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' | tr '\n' '|'; }
# Workload is selectable: only CONTENDED acquisitions are stamped, so a
# deeper queue grows the denominator. -g1 = 16 threads on 16 vCPUs;
# -g4 = 64 threads, oversubscribed, much deeper queues. NOT qlockbench --
# it resolves ~everything on the steal path (queue share 0.12%), so it
# produces contention but almost no QUEUED waiters, and a queued waiter is
# what is_cs_preempted() needs in order to ever run.
WL="${WL:--g1}"
echo "workload: hackbench -T $WL -f8"
A=$(get)
timeout 150 hackbench -T $WL -f8 -l250000 >/dev/null 2>&1
B=$(get)
set_ ivh_cs_recall_hist 0
python3 - "$A" "$B" <<'PY'
import re, sys
MHZ = 2200.0
def rows(s):
    out = []
    for part in s.split('|'):
        if not part.strip(): continue
        out.append({int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', part)})
    return out
a, b = rows(sys.argv[1]), rows(sys.argv[2])
while len(a) < 2: a.append({})
while len(b) < 2: b.append({})
d = [{k: b[i].get(k, 0) - a[i].get(k, 0) for k in set(a[i]) | set(b[i])} for i in range(2)]
print(f"\n{'bucket':>7} {'>= us':>10} {'holds':>12} {'flagged':>9} {'RECALL':>8}")
tot_n = tot_f = 0
for k in sorted(set(d[0]) | set(d[1])):
    miss, hit = max(d[0].get(k, 0), 0), max(d[1].get(k, 0), 0)
    n = miss + hit
    if n <= 0: continue
    tot_n += n; tot_f += hit
    lo = (2.0 ** k) / MHZ
    mark = "  <-- preemption mode" if lo >= 476 else ""
    print(f"  b{k:<4} {lo:10.1f} {n:12,} {hit:9,} {100.0*hit/n:7.1f}%{mark}")
print(f"\n  ALL holds          n={tot_n:,}  flagged={tot_f:,}  recall={100.0*tot_f/max(tot_n,1):.2f}%")
pn = sum(max(d[0].get(k,0),0)+max(d[1].get(k,0),0) for k in range(20,32))
pf = sum(max(d[1].get(k,0),0) for k in range(20,32))
print(f"  PREEMPTED (>477us) n={pn:,}  flagged={pf:,}  "
      + (f"RECALL={100.0*pf/pn:.1f}%" if pn else "no events"))
PY
echo RECALL45-DONE
