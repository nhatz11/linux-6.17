#!/bin/bash
# Rank every adaptive-spinning variant, ALL with migration ON.
#
# From the docs the best known AS is t12 + head bypass with the gate OPEN
# (runs=1 hold=0) = +5.73% vs stock PV pooled. The SHIPPED gate (runs=3,
# hold=220000) never fires, which is why bypass was wrongly called inert.
# Lock skipping (combo = lookahead + nosteal @ hop_cap=2) is null on real
# workloads; its only benefit is qlockbench p99.99 -9.8%. The question here is
# whether adding skip to the best arm costs less than 5%, which would let the
# tail-latency result be claimed from a configuration we actually ship.
#
# arms:
#   pv            stock pvqspinlock, migration OFF          (reference)
#   t12           migration + tier1 + tier2
#   t12bypL       + head bypass, OPEN gate (runs=1 hold=0)
#   t12combo      t12 + lock skipping (lookahead+nosteal, hop_cap=2)
#   t12bypLcombo  everything
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
REPS=${REPS:-6}
CSV=as_ranking_$(date +%m%d_%H%M%S).csv
echo "workload,rep,arm,value,migrations" > $CSV
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

base_as(){   # common AS baseline knobs
  echo 2 > $S/ivh_pv_preempt_src          # TSC heartbeat; vcpu_is_preempted() is false here
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 0 > $S/ivh_head_bypass_enable
  echo 0 > $S/ivh_pv_evict_enable; echo 0 > $S/ivh_pv_evict_lookahead
  echo 0 > $S/ivh_pv_requeue_nosteal; echo 1 > $S/ivh_pv_evict_hop_cap
}
bypass_open(){ echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold; }
skip_combo(){  echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
               echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap; }

arm(){
  case $1 in
    pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1; base_as
         echo 0 > $S/ivh_head_bypass_enable
         [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "ARM pv FAILED"; exit 1; } ;;
    t12) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1; base_as ;;
    t12bypL) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1; base_as; bypass_open ;;
    t12combo) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1; base_as; skip_combo ;;
    t12bypLcombo) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1; base_as; bypass_open; skip_combo ;;
  esac
  if [ "$1" != pv ]; then
    [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "ARM $1 FAILED (adaptive_mode)"; exit 1; }
    [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "ARM $1 FAILED (eligible)"; exit 1; }
  fi
}

run(){ case $1 in
  hackbench)       /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
  stressng_dentry) stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+';;
  dbench)          mkdir -p /root/dbench_test; dbench -F -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+';;
  ebizzy_mmap)     /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)';;
  fsmark_tmpfs)    rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
                   fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1 2>/dev/null | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1;;
  perf_sched_pipe) perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)';;
  dedup|vips)      s=$(date +%s.%N)
                   (cd $PARSECDIR && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16) >/dev/null 2>&1
                   e=$(date +%s.%N); echo "$e-$s"|bc;;
esac; }

ARMS=(pv t12 t12bypL t12combo t12bypLcombo)
for w in hackbench perf_sched_pipe stressng_dentry ebizzy_mmap fsmark_tmpfs dbench vips dedup; do
  echo "########## $w ##########"
  for r in $(seq $REPS); do
    # rotate arm order each rep so no arm sits in a fixed position
    n=${#ARMS[@]}; off=$(( (r-1) % n )); ORDER=()
    for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i+off) % n ))]}"); done
    for a in "${ORDER[@]}"; do
      arm $a; sync; echo 3 > /proc/sys/vm/drop_caches
      m0=$(mig); v=$(run $w); m1=$(mig)
      echo "$w,$r,$a,${v:-FAIL},$((m1-m0))" | tee -a $CSV
    done
  done
done
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
echo "WROTE $CSV"; echo AS-RANKING-DONE
