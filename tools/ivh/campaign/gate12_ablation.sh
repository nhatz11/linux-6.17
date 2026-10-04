#!/bin/bash
# FULL STACK vs stock PV, across the whole suite.
#   ivh arm = migration with BOTH gates (preempt_event_source=2 makes Gate 2 live)
#             + best AS (t12 + open-gate head bypass + lock-skipping combo)
#   pv  arm = stock pvqspinlock, no migration
# Question: does enabling Gate 2 cost enough to threaten the headline wins?
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
CSV=gate12_$(date +%m%d_%H%M%S).csv
echo "workload,rep,arm,value,tl_rejects,migrations" > $CSV
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
arm(){ case $1 in
  pv)  # "gate1 only" -- everything identical to the ivh arm EXCEPT Gate 2
       echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       echo 2 > $S/ivh_pv_preempt_src
       echo 0 > $S/ivh_preempt_event_source    # GATE 2 OFF (paravirt path, dead here)
       echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
       echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
       echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
       echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "gate1 arm FAILED"; exit 1; }
       [ "$(cat $S/ivh_preempt_event_source)" = 0 ] || { echo "gate2-off FAILED"; exit 1; } ;;
  ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       echo 2 > $S/ivh_pv_preempt_src            # AS tier-2 source
       echo 2 > $S/ivh_preempt_event_source      # GATE 2 LIVE (TSC path)
       echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
       echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
       echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
       echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "ivh arm FAILED"; exit 1; }
       [ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "gate2 FAILED"; exit 1; } ;;
esac; }
run(){ case $1 in
  hackbench)       /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
  perf_sched_pipe) perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)';;
  stressng_dentry) stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+';;
  ebizzy_mmap)     /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)';;
  fsmark_tmpfs)    rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
                   fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1 2>/dev/null | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1;;
  dbench)          mkdir -p /root/dbench_test; dbench -F -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+';;
  schbench)        schbench -m 4 -t 4 -r 15 2>&1 | grep -oP 'Requests per second:\s*\K[0-9]+' | tail -1;;
  psearchy)        rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done
                   (cd /root/mosbench/psearchy && timeout 900 ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x) 2>&1 \
                     | grep -o "throughput: [0-9.]*" | tail -1 | awk '{print $2}';;
  *)               s=$(date +%s.%N)
                   (cd $PARSECDIR && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16) >/dev/null 2>&1
                   e=$(date +%s.%N); echo "$e-$s"|bc;;
esac; }
WL="hackbench perf_sched_pipe stressng_dentry ebizzy_mmap fsmark_tmpfs dbench dedup vips blackscholes swaptions freqmine ferret bodytrack canneal psearchy"
for w in $WL; do
  echo "########## $w ##########"
  for r in 1 2 3; do
    [ $((r%2)) -eq 1 ] && ORDER="pv ivh" || ORDER="ivh pv"
    for a in $ORDER; do
      arm $a; sync; echo 3 > /proc/sys/vm/drop_caches
      t0=$(rej); m0=$(mig); v=$(run $w); t1=$(rej); m1=$(mig)
      echo "$w,$r,$a,${v:-FAIL},$((t1-t0)),$((m1-m0))" | tee -a $CSV
    done
  done
done
echo 0 > $S/ivh_preempt_event_source
echo "WROTE $CSV"; echo GATE12-DONE
