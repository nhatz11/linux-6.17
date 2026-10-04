#!/bin/bash
# Does the G-LOCK-39 hrtimer sampler cost the PV arm throughput?
# PV arm fixed (nothing reads capacity there), only ivh_tks_sampler_ns varies.
#   on  = 200000 (G-LOCK-39 shipped)      off = 0 (G-LOCK-30 tick behaviour)
set -u
S=/proc/sys/kernel
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
[ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "pv arm did not take"; exit 1; }
CSV=sampler_cost_pv_$(date +%m%d_%H%M%S).csv
echo "pair,workload,sampler,value" > $CSV
set_s() { echo 0 > $S/ivh_tks_phase_pct; echo "$1" > $S/ivh_tks_sampler_ns;
          [ "$(cat $S/ivh_tks_sampler_ns)" = "$1" ] || { echo "sampler_ns write FAILED"; exit 1; }; }
ebizzy()   { /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)'; }
pipebench(){ perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)'; }
for p in 1 2 3 4; do
  if (( p % 2 == 1 )); then order="200000 0 0 200000"; else order="0 200000 200000 0"; fi
  for s in $order; do
    set_s $s; sleep 2
    v=$(ebizzy);    echo "$p,ebizzy_mmap,$s,$v"      | tee -a $CSV
    v=$(pipebench); echo "$p,perf_sched_pipe,$s,$v"  | tee -a $CSV
  done
done
set_s 200000
echo "WROTE $CSV"
