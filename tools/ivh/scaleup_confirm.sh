#!/bin/bash
# Scale fsmark_tmpfs and sysbench_mutex until the PV arm clears 5 s, then
# re-confirm IVH still helps at the larger size.
#
# Both run under a second at the campaign's recorded invocation
# (fs_mark -n 2000 -> 0.48 s; sysbench --mutex-locks=40000 -> 0.59 s), which
# is too short for a stable throughput delta. Scaled linearly:
#     fs_mark  -n 2000 -> 30000   0.48 s -> 5.64 s  (1875 MB in /dev/shm)
#     sysbench --mutex-locks 40000 -> 600000   0.59 s -> 6.02 s
# Both do FIXED WORK, so the PV/IVH ratio is scale-invariant in principle --
# this run tests that in practice. If the scaled delta matches the recorded
# one, the larger size is a safe substitute; if not, the workload's benefit is
# size-dependent and the recorded figure does not transfer.
set -u
S=/proc/sys/kernel
PAIRS="${PAIRS:-5}"
pv(){ echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
      echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
      [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL pv arm"; exit 1; }; }
ivh(){ /root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
      echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
      echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
      echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
      echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
      echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
      [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL ivh arm"; exit 1; }
      [ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "FATAL gate2"; exit 1; }; }
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

run_fsmark(){ rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
  fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1 2>/dev/null \
    | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1; }
run_sysbench(){ sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=600000 run 2>&1 \
    | grep -oP 'total time:\s*\K[0-9.]+'; }

ab(){ # $1=label $2=fn $3=hi|lo
  echo "=== $1  ($3 = $([ "$3" = hi ] && echo higher || echo lower) is better) ==="
  $2 >/dev/null 2>&1          # warmup, discarded
  : > /tmp/ab_$1.txt
  for p in $(seq 1 $PAIRS); do
    if [ $((p % 2)) -eq 1 ]; then ORDER="pv ivh"; else ORDER="ivh pv"; fi
    for a in $ORDER; do
      $a; sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      m0=$(mig); t0=$(date +%s.%N); v=$($2); t1=$(date +%s.%N); m1=$(mig)
      echo "$a ${v:-0} $(python3 -c "print(round($t1-$t0,2))") $((m1-m0))" >> /tmp/ab_$1.txt
      printf "  p%d %-4s val=%-12s %5ss migs=%s\n" $p $a "${v:-0}" "$(python3 -c "print(round($t1-$t0,2))")" "$((m1-m0))"
    done
  done
  python3 - "$1" "$3" <<'PY'
import sys,statistics as st,math
lab,d=sys.argv[1],sys.argv[2]
pv=[];iv=[]
for l in open(f"/tmp/ab_{lab}.txt"):
    f=l.split(); (pv if f[0]=="pv" else iv).append(float(f[1]))
mp,mi=st.median(pv),st.median(iv)
gain = 100*(mi-mp)/mp if d=="hi" else 100*(mp-mi)/mp
n=min(len(pv),len(iv)); diffs=[(iv[i]-pv[i]) if d=="hi" else (pv[i]-iv[i]) for i in range(n)]
sd=st.stdev(diffs) if n>1 else 0
t=st.mean(diffs)/(sd/math.sqrt(n)) if sd else float('nan')
crit={3:3.182,4:2.776,5:2.571,6:2.571}.get(n,2.571)
print(f"  PV median {mp:,.2f}   IVH median {mi:,.2f}   IVH {gain:+.1f}%")
print(f"  {sum(1 for x in diffs if x>0)}/{n} pairs favour IVH   t={t:.2f} (crit {crit})  "
      f"{'SIGNIFICANT' if abs(t)>crit else 'NOT sig'}")
PY
}
ab fsmark run_fsmark hi
ab sysbench run_sysbench lo
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible; echo 2 > $S/ivh_preempt_event_source
echo SCALEUP-DONE
