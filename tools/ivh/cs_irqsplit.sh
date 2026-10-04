#!/bin/bash
# Why do >500us holds show "no preemption"? Three candidates:
#   (a) host preempted the holder, detector MISSED it (false negative)
#   (b) holder ran the whole time but was INTERRUPTED (spin_lock leaves IRQs
#       on) -- vCPU alive, hold stretched, "no preempt" is CORRECT
#   (c) a genuinely 500us-long critical section (implausible for a raw
#       spinlock: preemption is off, it cannot sleep)
# ivh_cs_v_flagged[irqoff][verdict] separates (b): if the no-preempt long
# holds concentrate at irqoff=0, they are interrupted, not long.
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1; set_ ivh_cs_head_bail 0; set_ ivh_cs_verdict 1
set_ ivh_cs_criterion 1; set_ ivh_cs_noise_cycles 1100000      # 500us floor
set_ ivh_tks_sampler_ns 200000; set_ ivh_vact_jump_ns 300000
R="python3 /root/ivh_tools/read_ivh_counters.py"
$R ivh_cs_v_flagged ivh_cs_v_unflagged > /tmp/iq_a.txt 2>&1
timeout 110 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
$R ivh_cs_v_flagged ivh_cs_v_unflagged > /tmp/iq_b.txt 2>&1
set_ ivh_tks_sampler_ns 0; set_ ivh_cs_verdict 0; set_ ivh_cs_noise_cycles 22000
python3 - <<'PY'
import re
# rows come out labelled with bail-cause names but the axis is irqoff:
#   row 0 ("NONE") = IRQs ENABLED, row 1 ("TIER1") = IRQs DISABLED
ROW={"NONE":"IRQs ON ","TIER1":"IRQs OFF"}
V=["no-preempt","PREEMPTED","ambiguous","unknowable"]
def p(f):
    a={}
    for ln in open(f):
        m=re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]',ln)
        if m: a[(m.group(1),m.group(2))]={int(x):int(y) for x,y in re.findall(r'\((\d+),\s*(\d+)\)',m.group(4))}
    return a
A=p('/tmp/iq_a.txt'); B=p('/tmp/iq_b.txt')
for name,title in (('ivh_cs_v_flagged','FLAGGED (held > 500us)'),):
    print(f"=== {title} ===")
    print(f"  {'':10} " + "  ".join(f"{v:>11}" for v in V))
    for k,lab in ROW.items():
        x=A.get((name,k),{}); y=B.get((name,k),{})
        r=[y.get(i,0)-x.get(i,0) for i in range(4)]
        print(f"  {lab:10} " + "  ".join(f"{v:>11}" for v in r))
    tot={}
    for k in ROW:
        x=A.get((name,k),{}); y=B.get((name,k),{})
        for i in range(4): tot[i]=tot.get(i,0)+y.get(i,0)-x.get(i,0)
    on=A.get((name,'NONE'),{}); onb=B.get((name,'NONE'),{})
    off=A.get((name,'TIER1'),{}); offb=B.get((name,'TIER1'),{})
    np_on=onb.get(0,0)-on.get(0,0); np_off=offb.get(0,0)-off.get(0,0)
    print()
    print(f"  'no-preempt' long holds:  IRQs ON = {np_on}   IRQs OFF = {np_off}")
    if np_on+np_off:
        print(f"  -> {100.0*np_on/(np_on+np_off):.0f}% of them held the lock with interrupts ENABLED")
        print("     (those are INTERRUPTED holds, not 500us critical sections)")
PY
echo IRQSPLIT-DONE
