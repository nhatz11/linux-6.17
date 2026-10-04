#!/bin/bash
# EVAL POINT 15, corrected metric: lock ACQUISITIONS per second, not contentions.
#
# WHY THE METRIC CHANGED. IVH acts at spinlock ACQUISITION -- ivh_pre_lock() is
# called from _raw_spin_lock* (kernel/locking/spinlock.c:308,327,345) on every
# acquisition -- whereas lock:contention_begin fires only in
# queued_spin_lock_slowpath (qspinlock.c:334), i.e. only when contended. Using
# contention inverted the ranking: parsec_dedup reads 1,035 contended/s (below
# the ~64/s idle floor once, in the PV arm) but 272,228 ACQUISITIONS/s, and it
# is one of the strongest IVH wins (+86.86%). Acquisitions exceed contentions
# by 9x on stressng_dentry and 23-40x on PARSEC.
#
# WHAT ivh_prelock_calls IS NOT. It counts acquisitions ELIGIBLE for IVH, after
# five bails in ivh_pre_lock(): bpf_sched_enabled, universal_eligible &&
# !ivh_exclude, in_task() && preemptible() && lock_depth==0,
# !rcu_preempt_depth(), and __state == TASK_RUNNING. The RCU one matters --
# that comment says "an enormous share of spin_lock() callers (dcache, lockref,
# net, slab) hold one" -- so acquisitions inside RCU readers are NOT counted.
# A true total needs CONFIG_LOCK_STAT (a rebuild; evaluation.md 11.1 Tier B).
# fentry on _raw_spin_lock* is not an option: bpftrace flags it a "dangerous
# function" and drops events under its own mitigation.
#
# MUST run in the IVH arm: the counter is behind the universal_eligible bail,
# so it reads zero in the PV arm by construction.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
M(){ python3 /root/ivh_tools/migcount.py; }
REPS="${REPS:-5}"
OUT="${OUT:-/root/ivh_tools/acqrate_$(date +%m%d-%H%M%S).csv}"

/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
for k in ivh_pv_tier1_enable ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_runs ivh_pv_evict_enable ivh_pv_evict_lookahead ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
echo 0 > $S/ivh_head_bypass_hold; echo 2 > $S/ivh_pv_evict_hop_cap; echo 0 > $S/ivh_tks_sampler_ns
[ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: IVH arm"; exit 1; }
[ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "FATAL: counter is gated off"; exit 1; }

NHX="nhextend_full|/root|hi|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16|x|+64.0"
lookup(){ local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}" "$NHX"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    [ "$n" = "$1" ] && { B_DIR="${dir:-/root}"; B_CMD="$c"; return 0; }
  done; return 1; }

LIST="${LIST:-stressng_dentry perf_epoll_wait hackbench_pipe_thr sysbench_mutex ebizzy_mmap dbench_16 fsmark_tmpfs wis_mmap2 schbench nhextend_full parsec_dedup parsec_vips parsec_ferret parsec_bodytrack parsec_swaptions parsec_blackscholes}"
echo "workload,rep,seconds,prelock,contended,migrations" > "$OUT"
echo "IVH arm, REPS=$REPS -> $OUT"
for w in $LIST; do
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  [ -d "$B_DIR" ] || B_DIR=/root
  ( cd "$B_DIR" && eval "$B_CMD" ) >/dev/null 2>&1        # warmup
  for r in $(seq 1 "$REPS"); do
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    p0=$(G ivh_prelock_calls); m0=$(M); t0=$(date +%s.%N)
    c=$(cd "$B_DIR" && perf stat -a -e lock:contention_begin -x, -- \
         bash -c "$B_CMD >/dev/null 2>&1" 2>&1 | awk -F, '/contention_begin/{print $1}')
    t1=$(date +%s.%N); p1=$(G ivh_prelock_calls); m1=$(M)
    python3 -c "print(f'$w,$r,{$t1-$t0:.3f},{$p1-$p0},${c:-0},{$m1-$m0}')" >> "$OUT"
  done
  tail -$REPS "$OUT" | python3 -c "
import sys,statistics as st
a=[];c=[];g=[]
for l in sys.stdin:
    f=l.strip().split(',')
    if len(f)>=6 and float(f[2])>0:
        a.append(int(f[3])/float(f[2])); c.append(float(f[4])/float(f[2])); g.append(int(f[5]))
if a:
    print(f'  {\"$w\":21} n={len(a)} acq={st.median(a):12,.0f}/s  cont={st.median(c):10,.0f}/s'
          f'  ratio={st.median(a)/max(st.median(c),1):5.1f}x  spread={max(a)/max(min(a),1):.2f}x'
          f'  migs={st.median(g):8,.0f}')"
done
echo "WROTE $OUT"; echo ACQRATE-DONE
