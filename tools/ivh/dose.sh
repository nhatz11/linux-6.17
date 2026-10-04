#!/bin/bash
# DOSE-RESPONSE PROOF for is_cs_preempted().
#
# Claim to prove: the predicate detects HOST PREEMPTION.
# Test: its fire rate must track host-measured stolen time, and fall to ~0
# when the host is not stealing.
#
# Non-circular by construction:
#   predictor    = guest-side lock hold duration (rdtsc delta, exact)
#   ground truth = host-side /proc/<vcpu-tid>/schedstat wait_ns
# Two independent machines, two independent clocks, no shared instrument.
# In particular this does NOT use ivh_vact (the in-guest TSC-jump oracle),
# whose 500us resolution is what invalidated the earlier precision numbers.
#
# Usage:  dose.sh <label>      e.g.  dose.sh off / light / medium / heavy
set -u
LABEL="${1:?usage: dose.sh <label>}"
S=/proc/sys/kernel
OUT=/root/ivh_tools/dose_results.txt
VMD=tdvirsh-trust_domain-4fea1ea2-761d-46bb-b3e1-4213dc10e6a7
HOST="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""

# pid changes whenever the TD is recreated; a stale pid reads zeros silently
VM=$($HOST "pgrep -f $VMD | head -1" 2>/dev/null)
NT=$($HOST "ls /proc/$VM/task 2>/dev/null | while read t; do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && echo x; done | wc -l" 2>/dev/null)
[ "$NT" = "16" ] || { echo "*** expected 16 vcpu threads, got '$NT' (pid=$VM) -- ABORT ***"; exit 1; }
ss(){ $HOST "for t in \$(ls /proc/$VM/task 2>/dev/null); do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && cat /proc/$VM/task/\$t/schedstat; done" 2>/dev/null; }

set_(){ echo "$2" > $S/$1 2>/dev/null; }
# detector: hold-duration predicate, DETECT-ONLY, 500us floor
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0
set_ ivh_cs_criterion 1;     set_ ivh_cs_noise_cycles 1100000
set_ ivh_pv_rot_enable 0
# oracle OFF: not used, and its sampler would cost ~48% throughput
set_ ivh_tks_sampler_ns 0;   set_ ivh_vact_jump_ns 1500000
set_ ivh_cs_verdict 0

R="python3 /root/ivh_tools/read_ivh_counters.py"
fired(){ $R ivh_cs_fired 2>/dev/null | awk '/^ivh_cs_fired/{print $3}'; }
hist(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]'; }

A=$(fired); HA=$(hist); ss > /tmp/dose_ssa.txt
T0=$(date +%s%N)
timeout 120 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
T1=$(date +%s%N)
ss > /tmp/dose_ssb.txt; B=$(fired); HB=$(hist)

python3 - "$LABEL" "$A" "$B" "$T0" "$T1" "$HA" "$HB" >> $OUT <<'PY'
import re, sys
label, a, b, t0, t1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
def buck(s):
    return {int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', s)}
ha, hb = buck(sys.argv[6]), buck(sys.argv[7])
def rd(f):
    w = 0
    for ln in open(f):
        q = ln.split()
        if len(q) == 3: w += int(q[1])
    return w
wall = (t1 - t0) / 1e9
stolen = (rd('/tmp/dose_ssb.txt') - rd('/tmp/dose_ssa.txt')) / 1e9
pct = 100.0 * stolen / (wall * 16)
fires = b - a
# independent guest-side check: holds landing in the >477us mode
longh = sum(hb.get(k, 0) - ha.get(k, 0) for k in range(20, 32))
allh = sum(hb.get(k, 0) - ha.get(k, 0) for k in range(0, 32))
print(f"{label:8} wall={wall:5.1f}s  HOST stolen={stolen:7.1f} CPU-s ({pct:5.1f}%)  "
      f"|  detector fires={fires:8d} ({fires/wall:8.1f}/s)  "
      f"|  holds>477us={longh:7d} ({1e6*longh/max(allh,1):6.1f} ppm of {allh})")
PY
tail -1 $OUT
