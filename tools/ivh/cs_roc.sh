#!/bin/bash
# Precision/recall tradeoff for is_cs_preempted(), criterion 1.
#
# The floor (ivh_cs_noise_cycles, since last_cs is sub-us) trades fire COUNT
# against precision. This produces the curve a reviewer needs: a detection
# rate with a precision attached, not a single n=16 point.
#
# SCORING: ivh_cs_v_flagged[irqoff][verdict]. The reader labels the first
# axis with bail-cause names but it is the holder's IRQ STATE --
# row "NONE" = IRQs ENABLED, row "TIER1" = IRQs DISABLED.
# Holds taken with IRQs off are EXCLUDED, not counted as negatives:
# ivh_vact_tick() is driven by the timer IRQ and the hrtimer sampler, and
# the audit runs before local_irq_restore(), so the detector is read before
# it could have seen a gap and returns "no preemption" by construction.
set -u
S=/proc/sys/kernel
VMD=tdvirsh-trust_domain-4fea1ea2-761d-46bb-b3e1-4213dc10e6a7
HOST="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""
VM=$($HOST "pgrep -f $VMD | head -1" 2>/dev/null)
NT=$($HOST "ls /proc/$VM/task 2>/dev/null | while read t; do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && echo x; done | wc -l" 2>/dev/null)
echo "host VM pid=$VM vcpu_threads=$NT"
[ "$NT" = "16" ] || { echo "*** expected 16 vcpu threads, got '$NT' -- ABORT ***"; exit 1; }
ss(){ $HOST "for t in \$(ls /proc/$VM/task 2>/dev/null); do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && cat /proc/$VM/task/\$t/schedstat; done" 2>/dev/null; }

set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0;    set_ ivh_cs_verdict 1
set_ ivh_cs_criterion 1;     set_ ivh_pv_rot_enable 0
set_ ivh_tks_sampler_ns 200000; set_ ivh_vact_jump_ns 300000
set_ ivh_vact_min_preempt_ns 2000
echo "sampler armed for verdict resolution -- throughput from this run is NOT valid"
echo
printf "%8s %8s %9s %8s %8s %9s %10s %s\n" "floor" "fires" "fires/s" "scored" "correct" "PRECISION" "blind" "host-stolen"

R="python3 /root/ivh_tools/read_ivh_counters.py"
for c in 4400 11000 22000 55000 110000 330000 1100000; do
	set_ ivh_cs_noise_cycles "$c"
	[ "$(cat $S/ivh_cs_noise_cycles)" = "$c" ] || { echo "  !! floor $c rejected"; continue; }
	$R ivh_cs_v_flagged > /tmp/roc_a.txt 2>&1
	ss > /tmp/roc_ssa.txt
	T0=$(date +%s%N)
	timeout 120 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
	T1=$(date +%s%N)
	ss > /tmp/roc_ssb.txt
	$R ivh_cs_v_flagged > /tmp/roc_b.txt 2>&1
	python3 - "$c" "$T0" "$T1" <<'PY'
import re, sys
def p(f):
    a = {}
    for ln in open(f):
        m = re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]', ln)
        if m:
            a[m.group(2)] = {int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', m.group(4))}
    return a
def rd(f):
    w = 0
    for ln in open(f):
        q = ln.split()
        if len(q) == 3: w += int(q[1])
    return w
A = p('/tmp/roc_a.txt'); B = p('/tmp/roc_b.txt')
d = lambda k: [B.get(k, {}).get(i, 0) - A.get(k, {}).get(i, 0) for i in range(4)]
on, off = d('NONE'), d('TIER1')
wall = (int(sys.argv[3]) - int(sys.argv[2])) / 1e9
stolen = (rd('/tmp/roc_ssb.txt') - rd('/tmp/roc_ssa.txt')) / 1e9
fires = sum(on) + sum(off)
scored = on[0] + on[1] + on[2]
correct = on[1] + on[2]
prec = f"{100.0*correct/scored:.1f}%" if scored else "n/a"
print(f"{int(sys.argv[1])//2200:7d}us {fires:8d} {fires/wall:9.1f} {scored:8d} {correct:8d} {prec:>9} "
      f"{sum(off):10d}  {stolen:.0f} CPU-s ({100.0*stolen/(wall*16):.0f}%)")
PY
done
set_ ivh_tks_sampler_ns 0; set_ ivh_cs_verdict 0; set_ ivh_cs_noise_cycles 22000
echo CSROC-DONE
