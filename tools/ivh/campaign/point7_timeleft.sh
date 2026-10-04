#!/bin/bash
# EVAL POINT 7: sensitivity of ivh_time_left_threshold_ns (Gate 2's constant).
# Full stack throughout -- migration + both gates LIVE + best AS. The ONLY
# variable is the threshold. No PV arm: every threshold is compared against
# the others, so a common reference is unnecessary.
#
# Range chosen from the measured burst distribution (ivh_tools/burst_probe.py):
# contended-vCPU active bursts have median 1.04 ms, p90 2.05 ms, p99 67-157 ms.
# The shipped 4 ms sits at roughly the 96th percentile, so the interesting
# region is BELOW it. 4 ms is included as the anchor (one cheap pass) so the
# sweep can say whether anything beats the shipped value.
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
REPS=${REPS:-2}
THRESH=${THRESH:-"250000 500000 1000000 2000000 4000000 8000000 16000000"}
CSV=point7_$(date +%m%d_%H%M%S).csv
echo "workload,thresh_ns,rep,value,tl_rejects,migrations" > $CSV
rej(){ python3 - <<'PY'
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f); offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    print(sum(r.read_u64(f,ph,sym["ivh_steal_imminent_time_left_reject"]+o) for o in offs))
PY
}
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
fullstack(){
  echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
  echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
  echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
  echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
  [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "AS arm FAILED"; exit 1; }
  [ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "gate2 FAILED"; exit 1; }
}
run(){ case $1 in
  hackbench)       /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
  perf_sched_pipe) perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)';;
  stressng_dentry) stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+';;
  ebizzy_mmap)     /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)';;
  fsmark_tmpfs)    rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
                   fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1 2>/dev/null | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1;;
  dbench)          mkdir -p /root/dbench_test; dbench -F -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+';;
  *)               s=$(date +%s.%N)
                   (cd $PARSECDIR && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16) >/dev/null 2>&1
                   e=$(date +%s.%N); echo "$e-$s"|bc;;
esac; }
WL="hackbench perf_sched_pipe stressng_dentry ebizzy_mmap fsmark_tmpfs dbench dedup vips"
fullstack
TA=($THRESH); n=${#TA[@]}
for r in $(seq $REPS); do
  off=$(( (r-1) % n )); ORDER=()
  for i in $(seq 0 $((n-1))); do ORDER+=("${TA[$(( (i+off) % n ))]}"); done
  echo "########## rep $r  order: ${ORDER[*]} ##########"
  for t in "${ORDER[@]}"; do
    echo "$t" > $S/ivh_time_left_threshold_ns
    [ "$(cat $S/ivh_time_left_threshold_ns)" = "$t" ] || { echo "thresh write FAILED $t"; exit 1; }
    for w in $WL; do
      sync; echo 3 > /proc/sys/vm/drop_caches
      t0=$(rej); m0=$(mig); v=$(run $w); t1=$(rej); m1=$(mig)
      echo "$w,$t,$r,${v:-FAIL},$((t1-t0)),$((m1-m0))" | tee -a $CSV
    done
  done
done
echo 4000000 > $S/ivh_time_left_threshold_ns
# 2026-09-28: was 'echo 0 > $S/ivh_preempt_event_source' here, which LEAVES
# MIGRATION DEAD for everything that runs afterwards. At source=0 Gate 2 reads
# rq->last_active_time, which on TDX is never written (no KVM_FEATURE_STEAL_TIME
# -> paravirt_steal_enabled never set), so the gate rejects nothing and
# ivh_migrations_done stays 0. Restore the live TSC path instead.
echo 2 > $S/ivh_preempt_event_source
echo "WROTE $CSV"; echo POINT7-DONE
