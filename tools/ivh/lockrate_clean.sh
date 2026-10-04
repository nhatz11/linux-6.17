#!/bin/bash
# Clean lock-acquisition rates: kernel-group workloads only, many reps.
#
# Kernel group only, because lock:contention_begin cannot see userspace
# (pthread->futex) synchronisation: parsec_ferret medians 58/s and swaptions
# 67/s, BELOW the ~64/s idle floor, while carrying +15.2%/+10.4% wins. More
# reps cannot fix a blind instrument, so PARSEC is excluded by construction
# rather than measured badly.
#
# Uses the SCALED configs where they exist (fsmark -n 30000 = 5.6s,
# sysbench --mutex-locks=600000 = 5.9s); at the campaign sizes both run under
# a second and their rates were unusable.
#
# Warmup discarded per workload: its absence produced the fsmark 86,939/s and
# swaptions 685/s artifacts in the first point-15 pass.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
HS(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'sum=[0-9]+' | cut -d= -f2; }
REPS="${REPS:-9}"
OUT="${OUT:-/root/ivh_tools/lockrate_clean_$(date +%m%d-%H%M%S).csv}"
LIST="${LIST:-stressng_dentry perf_epoll_wait hackbench_pipe_thr sysbench_mutex ebizzy_mmap dbench_16 fsmark_tmpfs wis_mmap2 schbench perf_sched_pipe}"

echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: PV arm"; exit 1; }
for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do echo 1 > $S/$k; done
[ "$(cat $S/ivh_cs_owner_enable)" = 1 ] || { echo "FATAL: CS stamping disarmed"; exit 1; }
echo 0 > $S/ivh_tks_sampler_ns

lookup(){ local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    [ "$n" = "$1" ] && { B_DIR="$dir"; B_CMD="$c"; return 0; }
  done; return 1; }

echo "workload,rep,seconds,contended,holds" > "$OUT"
echo "PV arm, REPS=$REPS, warmup discarded -> $OUT"
for w in $LIST; do
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  ( cd "$B_DIR" && eval "$B_CMD" ) >/dev/null 2>&1        # warmup
  for r in $(seq 1 "$REPS"); do
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    h0=$(HS); t0=$(date +%s.%N)
    c=$(cd "$B_DIR" && perf stat -a -e lock:contention_begin -x, -- \
         bash -c "$B_CMD >/dev/null 2>&1" 2>&1 | awk -F, '/contention_begin/{print $1}')
    t1=$(date +%s.%N); h1=$(HS)
    python3 -c "
print(f'$w,$r,{$t1-$t0:.3f},${c:-0},{$h1-$h0}')" >> "$OUT"
  done
  tail -$REPS "$OUT" | python3 -c "
import sys,statistics as st
v=[]
for l in sys.stdin:
    f=l.strip().split(',')
    if len(f)>=5 and float(f[2])>0: v.append(float(f[3])/float(f[2]))
if v:
    lo,hi=min(v),max(v)
    print(f'  {\"$w\":20} n={len(v)} median={st.median(v):10,.0f}/s  range {lo:,.0f}-{hi:,.0f}  spread {hi/max(lo,1):.2f}x')"
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible; echo 2 > $S/ivh_preempt_event_source
echo "WROTE $OUT"; echo LOCKRATE-CLEAN-DONE
