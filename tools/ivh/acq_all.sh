#!/bin/bash
# EVAL POINT 15: TOTAL lock acquisitions per second -- every thread that
# REQUESTS a lock, not just the ones IVH could act on.
#
# Instrument: ftrace function profiler (ivh_tools/lockrate.sh), counting entries
# to _raw_spin_lock{,_irqsave,_irq,_bh,_nested,...}, _raw_spin_trylock{,_bh},
# the rwlock variants, and both queued-spinlock slowpaths. No PMU, no BPF, no
# eligibility filtering.
#
# WHY NOT THE OTHER TWO METRICS:
#   lock:contention_begin  fires only in queued_spin_lock_slowpath, i.e. only
#     when CONTENDED -- 1.2% of schbench's acquisitions. Using it ranked
#     parsec_dedup at 1,035/s (below the idle floor) when it is a top-3 IVH win.
#   ivh_prelock_calls      counts only acquisitions ELIGIBLE for IVH, after
#     five bails in ivh_pre_lock() including !rcu_preempt_depth() -- 45% of
#     schbench's total. Also reads zero in the PV arm by construction.
#
# Overhead: the profiler costs time per call, so fixed-duration workloads do
# less work while traced. Counts are exact; absolute throughput under tracing
# is NOT comparable to an untraced run. Relative ranking is what this is for.
set -u
source /root/ivh_tools/ivh_benchmarks.sh
REPS="${REPS:-3}"
OUT="${OUT:-/root/ivh_tools/acq_all_$(date +%m%d-%H%M%S).csv}"
NHX="nhextend_full|/root|hi|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16|x|+64.0"
lookup(){ local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}" "$NHX"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    [ "$n" = "$1" ] && { B_DIR="${dir:-/root}"; B_CMD="$c"; return 0; }
  done; return 1; }
LIST="${LIST:-stressng_dentry perf_epoll_wait hackbench_pipe_thr sysbench_mutex ebizzy_mmap dbench_16 fsmark_tmpfs wis_mmap2 schbench nhextend_full parsec_dedup parsec_vips parsec_ferret parsec_bodytrack parsec_swaptions parsec_blackscholes}"
echo "workload,rep,seconds,total_acq,slowpath" > "$OUT"
echo "ftrace profiler, REPS=$REPS -> $OUT"
for w in $LIST; do
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  [ -d "$B_DIR" ] || B_DIR=/root
  for r in $(seq 1 "$REPS"); do
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    o=$(cd "$B_DIR" && bash /root/ivh_tools/lockrate.sh "$w" bash -c "$B_CMD" 2>/dev/null)
    python3 - "$w" "$r" "$OUT" <<PY
import re,sys
o="""$o"""
w,r,out=sys.argv[1:4]
sec=re.search(r'wall ([0-9.]+)s',o)
tot=re.search(r'TOTAL\s+(\d+)',o)
slow=sum(int(m) for m in re.findall(r'queued_spin_lock_slowpath\s+(\d+)',o))
if sec and tot:
    open(out,'a').write(f"{w},{r},{float(sec.group(1)):.3f},{tot.group(1)},{slow}\n")
PY
  done
  tail -$REPS "$OUT" | python3 -c "
import sys,statistics as st
a=[];s=[]
for l in sys.stdin:
    f=l.strip().split(',')
    if len(f)>=5 and float(f[2])>0: a.append(int(f[3])/float(f[2])); s.append(int(f[4])/float(f[2]))
if a: print(f'  {\"$w\":21} n={len(a)} acq={st.median(a):12,.0f}/s  slowpath={st.median(s):9,.0f}/s'
            f'  ({100*st.median(s)/max(st.median(a),1):4.1f}%)  spread={max(a)/max(min(a),1):.2f}x')"
done
echo "WROTE $OUT"; echo ACQ-ALL-DONE
