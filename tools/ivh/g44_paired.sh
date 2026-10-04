#!/bin/bash
# G-LOCK-44: sweep the tier-1 halt-duration gate and score precision.
#
# Three metrics, two of them INDEPENDENT of the cutoff being tuned, so the
# result is not circular with it:
#   1. precision  = tier-1 halts lasting >=100us / all tier-1 halts
#                   (correlated with the cutoff -- read with 2 and 3)
#   2. kick ratio = ivh_wake_hypercall / pv_wait_calls. A halt taken because
#                   prev was genuinely away must be ENDED by a kick. At the
#                   shipped config this was 0.7%, which is what a population
#                   of false halts looks like. Independent of the cutoff.
#   3. wall clock. Independent.
# Host schedstat over the same window gives the ground-truth preemption load.
S=/proc/sys/kernel
VM=2301655
HOST="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""
echo 0 > $S/ivh_tks_sampler_ns          # MANDATORY for any throughput number
echo 2 > $S/ivh_adaptive_mode; echo 1 > $S/ivh_pv_tier1_enable
echo 1 > $S/ivh_pv_tier2_enable; echo 0 > $S/ivh_pv_tier1_confirm
echo 11000000 > $S/ivh_pv_beat_threshold
R="python3 /root/ivh_tools/read_ivh_counters.py"
N="ivh_node_halt_cycles ivh_node_halt_events ivh_node_halt_hist ivh_tier1_halt_fresh ivh_pv_wait_calls ivh_wake_hypercall"
hostss(){ $HOST "for t in \$(ls /proc/$VM/task); do grep -q '^CPU ' /proc/$VM/task/\$t/comm 2>/dev/null && cat /proc/$VM/task/\$t/schedstat; done" 2>/dev/null; }
arm(){
  echo "$1" > $S/ivh_pv_tier1_halt_min
  [ "$(cat $S/ivh_pv_tier1_halt_min)" = "$1" ] || { echo "REJECTED $1"; return; }
  sleep 1
  hostss > /tmp/g44_ss_a.txt; $R $N > /tmp/g44_a.txt 2>&1
  local t0=$(date +%s%N); hackbench -T -g1 -f8 -l100000 >/dev/null 2>&1; local t1=$(date +%s%N)
  $R $N > /tmp/g44_b.txt 2>&1; hostss > /tmp/g44_ss_b.txt
  python3 - "$2" "$t0" "$t1" <<'PY'
import re,sys
def p(f):
    s={};h={}
    for ln in open(f):
        m=re.match(r'^(ivh_\w+)\s+\[(\w+)\s*\]\s+=\s+(\d+)',ln)
        if m: s[(m.group(1),m.group(2))]=int(m.group(3)); continue
        m=re.match(r'^(ivh_node_halt_hist)\s+\[(\w+)\s*\]\s+sum=\d+\s+nonzero_buckets=\[(.*)\]',ln)
        if m: h[m.group(2)]={int(a):int(b) for a,b in re.findall(r'\((\d+),\s*(\d+)\)',m.group(3))}; continue
        m=re.match(r'^(ivh_\w+)\s+=\s+(\d+)',ln)
        if m: s[(m.group(1),'')]=int(m.group(2))
    return s,h
def ss(f):
    r=w=c=0
    for ln in open(f):
        q=ln.split()
        if len(q)==3: r+=int(q[0]); w+=int(q[1]); c+=int(q[2])
    return r,w,c
sa,ha=p('/tmp/g44_a.txt'); sb,hb=p('/tmp/g44_b.txt'); MHZ=2200.0
wall=(int(sys.argv[3])-int(sys.argv[2]))/1e9
tot=over=cyc=0
for c in ("TIER1","TIER1_AGREED","TIER1_DISAGREED"):
    A=ha.get(c,{}); B=hb.get(c,{})
    cyc+=sb.get(('ivh_node_halt_cycles',c),0)-sa.get(('ivh_node_halt_cycles',c),0)
    for b in set(A)|set(B):
        d=B.get(b,0)-A.get(b,0)
        if d>0:
            tot+=d
            if (2.0**b)/MHZ>=100.0: over+=d
fresh=sb.get(('ivh_tier1_halt_fresh',''),0)-sa.get(('ivh_tier1_halt_fresh',''),0)
pw=sb.get(('ivh_pv_wait_calls',''),0)-sa.get(('ivh_pv_wait_calls',''),0)
kick=sb.get(('ivh_wake_hypercall',''),0)-sa.get(('ivh_wake_hypercall',''),0)
try:
    ra,wa,ca=ss('/tmp/g44_ss_a.txt'); rb,wb,cb=ss('/tmp/g44_ss_b.txt')
    host=f"host_wait={(wb-wa)/1e9:6.1f}CPU-s"
except Exception: host="host=n/a"
print(f"halt_min={sys.argv[1]:8s} wall={wall:5.2f}s t1_halts={tot:<7d} refused={fresh:<8d} "
      f"mean={cyc/max(tot,1)/MHZ:6.1f}us PREC={100.0*over/max(tot,1):5.1f}% "
      f"true={over:<6d} KICK={100.0*kick/max(pw,1):5.2f}% {host}")
PY
}
echo "G-LOCK-44 tier-1 halt-duration gate sweep (threshold=5ms, confirm=0)"
echo "PREC = tier-1 halts >=100us.  KICK = halts actually ended by a kick (independent)."
# PAIRED: each cutoff runs immediately after a fresh baseline, so slow
# host-contention drift cancels within the pair. Absolute PREC is not
# comparable across pairs; the DELTA within a pair is.
for rep in 1 2; do
  for v in 44000 110000 220000; do
    arm 0 "BASE"
    arm $v "$((v/2200))us"
  done
done
echo 0 > $S/ivh_pv_tier1_halt_min
echo G44SWEEP-DONE
