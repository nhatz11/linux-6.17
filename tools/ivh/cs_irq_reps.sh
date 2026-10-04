#!/bin/bash
# Repeat the IRQ-split long-hold audit, with HOST-side confirmation that the
# vCPUs really were contended during each window.
#
# ivh_cs_v_flagged[irqoff][verdict]: rows print with bail-cause labels but the
# axis is the holder's IRQ state -- row "NONE" = IRQs ENABLED,
# row "TIER1" = IRQs DISABLED.
#
# Why the split matters: ivh_vact_tick() is driven by the timer IRQ and the
# hrtimer sampler. A hold taken entirely with IRQs off gets no tick and no
# sampler, and the audit in __ivh_cs_owner_clear() runs BEFORE IRQs are
# restored -- so the detector is read before it could have seen the gap and
# returns "no preemption" by construction. Those rows are UNKNOWABLE, not
# negative. Only the IRQs-ON row is scoreable.
set -u
S=/proc/sys/kernel
VMD=tdvirsh-trust_domain-4fea1ea2-761d-46bb-b3e1-4213dc10e6a7
HOST="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""

# Re-resolve the pid EVERY run: the TD is recreated on reboot and a stale pid
# silently reads zeros, which looks exactly like an idle host.
VM=$($HOST "pgrep -f $VMD | head -1" 2>/dev/null)
NT=$($HOST "ls /proc/$VM/task 2>/dev/null | while read t; do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && echo x; done | wc -l" 2>/dev/null)
echo "host VM pid=$VM  vcpu_threads=$NT"
[ "$NT" = "16" ] || { echo "*** expected 16 vcpu threads, got '$NT' -- ABORT ***"; exit 1; }

ss(){ $HOST "for t in \$(ls /proc/$VM/task 2>/dev/null); do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && cat /proc/$VM/task/\$t/schedstat; done" 2>/dev/null; }

set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0;    set_ ivh_cs_verdict 1
set_ ivh_cs_criterion 1;     set_ ivh_cs_noise_cycles 1100000     # 500us floor
set_ ivh_tks_sampler_ns 200000; set_ ivh_vact_jump_ns 300000
set_ ivh_vact_min_preempt_ns 2000
echo "floor=$(( $(cat $S/ivh_cs_noise_cycles) / 2200 ))us  sampler=$(cat $S/ivh_tks_sampler_ns)ns  (sampler armed: throughput here is NOT valid)"
echo

R="python3 /root/ivh_tools/read_ivh_counters.py"
for rep in 1 2 3 4; do
	$R ivh_cs_v_flagged > /tmp/ir_a.txt 2>&1
	ss > /tmp/ir_ss_a.txt
	T0=$(date +%s%N)
	timeout 120 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
	T1=$(date +%s%N)
	ss > /tmp/ir_ss_b.txt
	$R ivh_cs_v_flagged > /tmp/ir_b.txt 2>&1
	python3 - "$rep" "$T0" "$T1" <<'PY'
import re, sys
def p(f):
    a = {}
    for ln in open(f):
        m = re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]', ln)
        if m:
            a[m.group(2)] = {int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', m.group(4))}
    return a
def rd(f):
    r = w = c = 0
    for ln in open(f):
        q = ln.split()
        if len(q) == 3:
            r += int(q[0]); w += int(q[1]); c += int(q[2])
    return r, w, c
A = p('/tmp/ir_a.txt'); B = p('/tmp/ir_b.txt')
d = lambda k: [B.get(k, {}).get(i, 0) - A.get(k, {}).get(i, 0) for i in range(4)]
on  = d('NONE')    # IRQs enabled  -> scoreable
off = d('TIER1')   # IRQs disabled -> blind by construction
try:
    ra, wa, ca = rd('/tmp/ir_ss_a.txt'); rb, wb, cb = rd('/tmp/ir_ss_b.txt')
    wall = (int(sys.argv[3]) - int(sys.argv[2])) / 1e9
    host = (f"host: stolen={(wb-wa)/1e9:6.1f} CPU-s ({100.0*(wb-wa)/1e9/(wall*16):4.1f}% of vCPU time), "
            f"{cb-ca} deschedules, wall={wall:.1f}s")
except Exception:
    host = "host: UNAVAILABLE"
pos = on[1] + on[2]; scoreable = on[0] + on[1] + on[2]
prec = f"{100.0*pos/scoreable:.0f}%" if scoreable else "n/a"
print(f"rep{sys.argv[1]}  {host}")
print(f"      IRQs ON  (scoreable): no-preempt={on[0]:<4} PREEMPTED={on[1]:<4} ambiguous={on[2]:<4} unknowable={on[3]:<4}  -> {pos}/{scoreable} = {prec}")
print(f"      IRQs OFF (blind)    : no-preempt={off[0]:<4} PREEMPTED={off[1]:<4} ambiguous={off[2]:<4} unknowable={off[3]:<4}")
PY
done
set_ ivh_tks_sampler_ns 0; set_ ivh_cs_verdict 0; set_ ivh_cs_noise_cycles 22000
echo IRQREPS-DONE
