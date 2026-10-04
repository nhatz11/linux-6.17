#!/bin/bash
# Corrected A/B: sampler OFF in BOTH arms (removes the instrument's own cost).
set -u
S=/proc/sys/kernel
echo 0 > $S/ivh_tks_sampler_ns; echo 0 > $S/ivh_tks_phase_pct
[ "$(cat $S/ivh_tks_sampler_ns)" = 0 ] || { echo "sampler still on"; exit 1; }

# migration liveness under the new calibration
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
m0=$(python3 /root/ivh_tools/migcount.py); timeout 60 hackbench -T -g1 -f8 -l60000 >/dev/null 2>&1
m1=$(python3 /root/ivh_tools/migcount.py)
echo "migration liveness (sampler off): delta $((m1-m0))"
[ "$((m1-m0))" -gt 0 ] || { echo "FATAL: migration does not fire with sampler off"; exit 1; }

CSV=corrected_ab_$(date +%m%d_%H%M%S).csv
echo "block,pos,mode,workload,value" > $CSV
arm() { case $1 in
  pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || exit 1 ;;
  ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || exit 1 ;;
esac; }
ebizzy()   { /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)'; }
pipebench(){ perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)'; }
for b in 1 2 3 4; do
  if (( b % 2 == 1 )); then order="pv ivh ivh pv"; else order="ivh pv pv ivh"; fi
  QUIET=1 MIN_S=30 MAX_S=120 /root/ivh_tools/wait_capacity_settled.sh >/dev/null 2>&1
  pos=0
  for m in $order; do
    pos=$((pos+1)); arm $m
    echo "$b,$pos,$m,ebizzy_mmap,$(ebizzy)"         | tee -a $CSV
    echo "$b,$pos,$m,perf_sched_pipe,$(pipebench)"  | tee -a $CSV
  done
done
echo "WROTE $CSV"
