#!/bin/bash
# The user's hypothesis: long-CS and preempted-holder are separable by a
# DURATION CONSTANT. Hold-duration histogram says the valley is b19
# (238-477us). criterion 1 tests held > last_cs + noise_cycles, and last_cs
# is sub-us, so noise_cycles acts as that absolute floor.
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; [ "$(cat $S/$1)" = "$2" ] || echo "  !! $1 rejected"; }
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0;    set_ ivh_cs_verdict 1
set_ ivh_cs_criterion 1;     set_ ivh_pv_rot_enable 0
set_ ivh_tks_sampler_ns 200000; set_ ivh_vact_jump_ns 300000
R="python3 /root/ivh_tools/read_ivh_counters.py"
N="ivh_cs_v_flagged ivh_cs_v_unflagged"
arm(){
  set_ ivh_cs_noise_cycles "$1"
  $R $N > /tmp/cf_a.txt 2>&1
  timeout 110 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
  $R $N > /tmp/cf_b.txt 2>&1
  python3 - "$2" <<'PY'
import re,sys
def p(f):
    a={}
    for ln in open(f):
        m=re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]',ln)
        if m: a[(m.group(1),m.group(2))]={int(x):int(y) for x,y in re.findall(r'\((\d+),\s*(\d+)\)',m.group(4))}
    return a
A=p('/tmp/cf_a.txt'); B=p('/tmp/cf_b.txt')
f=[0]*4; u=[0]*4
for c in ("NONE","TIER1","TIER2","EXHAUST","TIER1_AGREED","TIER1_DISAGREED"):
    x=A.get(('ivh_cs_v_flagged',c),{}); y=B.get(('ivh_cs_v_flagged',c),{})
    for i in range(4): f[i]+=y.get(i,0)-x.get(i,0)
    x=A.get(('ivh_cs_v_unflagged',c),{}); y=B.get(('ivh_cs_v_unflagged',c),{})
    for i in range(4): u[i]+=y.get(i,0)-x.get(i,0)
tp=f[1]+f[2]; fp=f[0]; fn=u[1]+u[2]
prec=100.0*tp/(tp+fp) if tp+fp else float('nan')
rec =100.0*tp/(tp+fn) if tp+fn else float('nan')
print(f"floor={sys.argv[1]:>7}  fires={sum(f):<6} judg={f[0]+f[1]+f[2]:<5} "
      f"TP={tp:<5} FP={fp:<5} FN={fn:<5} unk={f[3]:<5}  PRECISION={prec:5.1f}%  RECALL={rec:5.1f}%")
PY
}
echo "absolute hold floor vs precision/recall (valley is at 238-477us)"
for c in 110000 550000 1100000 1650000 2200000; do arm $c "$((c/2200))us"; done
set_ ivh_tks_sampler_ns 0; set_ ivh_cs_verdict 0; set_ ivh_cs_noise_cycles 22000
echo CSFLOOR-DONE
