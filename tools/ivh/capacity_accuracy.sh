#!/bin/bash
# ESTIMATOR ACCURACY vs TRUE HOST STEAL -- strictly concurrent, n reps.
#
# Ground truth: SCHED_FIFO busy-spinner pinned to the vCPU. FIFO preempts every
# normal and SCHED_IDLE guest thread (including vcap_probe's workers), so a gap
# means the vCPU itself was off a physical CPU -- host preemption, not guest
# scheduling. RT throttling still parks it for (period-runtime) once per period;
# those gaps are ~50ms vs ~100us host quanta, so they are EXCLUDED BY SIZE.
#
# Kernel claim: capacity is sampled CONCURRENTLY with the probe, over the same
# window, because the EMA half-life (~10.5s) means a before/after read would
# mostly reflect conditions outside the window.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
CPUS=${CPUS:-"3 7 11 15"}; SECS=${SECS:-8}; REPS=${REPS:-5}
OUT=$T/capacity_accuracy_$(date +%m%d-%H%M%S).csv
echo "rep,cpu,elapsed_ns,host_ns,host_frac,throttle_ns,cap_mean,k_frac,ratio" > $OUT
for r in $(seq 1 $REPS); do
 for c in $CPUS; do
  ( for i in $(seq 1 200); do
      python3 $T/read_vact_rq.py ivh_uc_capacity 2>/dev/null \
        | sed 's/.*per-cpu=\[//;s/\].*//' | cut -d, -f$((c+1)) | tr -d ' '
      sleep 0.2
    done ) > /tmp/ca_$c.txt 2>/dev/null &
  SP=$!
  timeout -k 5 $((SECS+25)) $T/vcpu_gone $c $SECS 1 2>/dev/null > /tmp/pr_$c.txt
  kill $SP 2>/dev/null; wait $SP 2>/dev/null
  awk -v r=$r -v c=$c -v capf=/tmp/ca_$c.txt '
    /^cpu=/{split($2,a,"="); el=a[2]}
    /^hist/{split($3,g,"="); split($5,s,"=");
            if(g[2]>=10000000) thr+=s[2]; else if(g[2]>=50000) host+=s[2]}
    END{ n=0;sum=0; while((getline L<capf)>0) if(L+0>0){sum+=L;n++}
         cap=n?sum/n:0; hf=host/el; kf=cap?1-cap/1024:0
         printf "%d,%d,%d,%d,%.6f,%d,%.1f,%.6f,%.4f\n", r,c,el,host+0,hf,thr+0,cap,kf,(hf>0?kf/hf:0) }' /tmp/pr_$c.txt >> $OUT
  rm -f /tmp/ca_$c.txt /tmp/pr_$c.txt
 done
 printf "  rep%-3s done\n" "$r"
done
echo "DONE -> $OUT"
