#!/bin/bash
# HOW ACCURATE IS THE CAPACITY / STEAL ESTIMATOR?
#
# Ground truth: vcpu_gone busy-spins pinned to ONE vCPU and records wall-clock
# gaps -- the literal time this vCPU was not running. Busy-spinning is required,
# not incidental: idle must be ~0 so the kernel's avail_c == elapsed, and the
# vCPU must look as runnable to the host as any loaded one.
#
# Kernel claim, from kernel/sched/core.c:
#     avail = elapsed - idle ; used = avail - steal ; capacity = EMA(used*1024/avail)
# so  1 - capacity/1024  IS the kernel's gone-fraction, and d(ivh_tks_steal_ns)
# is its absolute steal.
#
# Thresholds are MATCHED: ivh_tks_deadband_ns (50us) is what the estimator
# discards, so the prober is read at its 50us threshold.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
CPUS=${CPUS:-"13 14 12 15 5"}; SECS=${SECS:-15}; REPS=${REPS:-3}
OUT=$T/capacity_validate_$(date +%m%d-%H%M%S).csv
[ "$(cat $S/ivh_steal_source)" = 2 ] || { echo "need ivh_steal_source=2"; exit 1; }
[ "$(cat $S/ivh_uc_enabled)"   = 1 ] || { echo "need ivh_uc_enabled=1"; exit 1; }
echo "rep,cpu,elapsed_ns,probe_gone_ns,probe_events,probe_frac,k_steal_ns,cap_mean,cap_min,k_frac" > $OUT
rq(){ timeout -k 5 60 python3 $T/read_vact_rq.py "$1" 2>/dev/null | awk -v c=$2 '
  /per-cpu=/{ sub(/.*per-cpu=\[/,""); sub(/\].*/,""); n=split($0,a,", "); print a[c+1] }'; }
for r in $(seq 1 $REPS); do
 for c in $CPUS; do
  s0=$(rq ivh_tks_steal_ns $c)
  # sample capacity DURING the run -- it is an EMA and recovers once the cpu idles
  ( for i in $(seq 1 40); do rq ivh_uc_capacity $c; sleep 0.35; done ) > /tmp/cap_$c.txt 2>/dev/null &
  SAMP=$!
  P=$($T/vcpu_gone $c $SECS 2>/dev/null)
  kill $SAMP 2>/dev/null; wait $SAMP 2>/dev/null
  s1=$(rq ivh_tks_steal_ns $c)
  echo "$P" | awk -v r=$r -v c=$c -v s0="$s0" -v s1="$s1" -v capf=/tmp/cap_$c.txt '
    /^cpu=/{split($2,a,"="); el=a[2]}
    /thresh_ns=50000 /{split($2,g,"="); split($3,e,"="); split($4,f,"=");
      gone=g[2]; ev=e[2]; frac=f[2]}
    END{
      n=0; sum=0; mn=99999
      while ((getline line < capf) > 0) { if (line+0>0) { sum+=line; n++; if (line+0<mn) mn=line+0 } }
      cap = n? sum/n : 0
      printf "%d,%d,%d,%d,%d,%.6f,%d,%.1f,%d,%.6f\n", r,c,el,gone,ev,frac,(s1-s0),cap,(n?mn:0),(cap?1-cap/1024:0)
    }' >> $OUT
  rm -f /tmp/cap_$c.txt
 done
 printf "  rep%-3s done\n" "$r"
done
echo "DONE -> $OUT"
