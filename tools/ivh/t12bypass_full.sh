#!/bin/bash
# t12+bypass vs stock PV.  NO LOCK SKIPPING ANYWHERE (ivh_pv_evict_enable=0,
# asserted in every arm) -- skipping is closed, see eval_final.md section 11.
#
# THE REASON THIS HARNESS EXISTS: tier 2 and head bypass are BOTH inert in the
# shipped configuration, and for the same reason. Both read
# ivh_pv_beat_threshold, shipped at 11,000,000 cycles (5 ms). Measured
# 2026-09-28, one hackbench run each:
#
#   beat_threshold   obs_samples   obs_stale   actionable   bypass_fired   tier2_fired
#   11,000,000 (5ms)   1,726,222           0            0              0    456/6.3M
#    2,200,000 (1ms)   2,189,903      23,160          414            412     102,814
#      220,000 (100us) 2,180,193      97,543        1,019          1,019     356,775
#
# The observer runs (1.7M samples); it dies at the staleness test -- at 5 ms NO
# head is ever judged preempted. So "enable=1" is not enough for either
# mechanism, and every earlier arm labelled t12+bypass at the shipped threshold
# was in fact TIER 1 ONLY. Hence the two threshold arms below.
#
# head bypass ALSO needs ivh_head_bypass_probe=1: the bypass code lives inside
# ivh_head_observe(), whose only call site is gated on that sysctl.
#
# ARMS
#   pv          stock pvqspinlock
#   t1          tier 1 only -- the REFERENCE. Without it a win cannot be
#               attributed to tier2+bypass rather than to tier1.
#   t12b_1ms    tier1+tier2+bypass, beat_threshold 2,200,000
#   t12b_100us  tier1+tier2+bypass, beat_threshold   220,000
#
# Migration is OFF in every arm (isolation practice; also uniform host
# contention makes it near-dead anyway).
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }

BENCHES="${BENCHES:-hackbench_pipe_thr ebizzy_mmap dbench_16 sysbench_mutex parsec_vips stressng_dentry}"
REPS="${REPS:-8}"
OUT="${OUT:-/root/ivh_tools/t12b_$(date +%m%d-%H%M%S).csv}"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  echo 0 > $S/ivh_universal_eligible          # migration off in ALL arms
  echo 0 > $S/ivh_pv_evict_enable             # NO skipping in ANY arm
  case "$1" in
    pv)
      /root/spin_mode 1 >/dev/null 2>&1
      echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
      [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: pv arm did not take"; exit 1; }
      ;;
    *)
      /root/spin_mode 2 >/dev/null 2>&1; echo 0 > $S/ivh_universal_eligible
      echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
      echo 1 > $S/ivh_pv_tier1_enable
      if [ "$1" = t1 ]; then
        echo 0 > $S/ivh_pv_tier2_enable
        echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
      else
        echo 1 > $S/ivh_pv_tier2_enable
        echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
        echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
        case "$1" in
          t12b_1ms)   echo 2200000 > $S/ivh_pv_beat_threshold ;;
          t12b_100us) echo  220000 > $S/ivh_pv_beat_threshold ;;
        esac
        [ "$(cat $S/ivh_head_bypass_probe)" = 1 ] || { echo "FATAL: bypass probe off -- observer never runs"; exit 1; }
      fi
      [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: $1 arm did not take"; exit 1; }
      ;;
  esac
  echo 0 > $S/ivh_pv_evict_enable
  [ "$(cat $S/ivh_pv_evict_enable)" = 0 ] || { echo "FATAL: skipping is on"; exit 1; }
  [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "FATAL: migration is on"; exit 1; }
  echo 1 > $S/ivh_slowpath_wait_measure
  [ "$(cat $S/ivh_slowpath_wait_measure)" = 1 ] || { echo "FATAL: wait_measure off"; exit 1; }
}

runbench(){
  lookup "$1" || { echo 0; return; }
  if [ "$B_EXT" = TIME ]; then
    local a b; a=$(date +%s.%N); ( cd "$B_DIR" && eval "$B_CMD" ) >/dev/null 2>&1; b=$(date +%s.%N)
    python3 -c "print(f'{$b-$a:.4f}')"
  else
    ( cd "$B_DIR" && eval "$B_CMD" 2>&1 ) | eval "$B_EXT" | tail -1
  fi
}

echo "workload,arm,rep,perf,dur_s,wait_ns,wait_events,t1_fired,t2_fired,bypass_fired,obs_stale,obs_actionable,obs_free_open" > "$OUT"
ARMS=(pv t1 t12b_1ms t12b_100us)
N=${#ARMS[@]}
echo "t12+bypass: $N arms x $REPS reps x $(echo $BENCHES | wc -w) workloads -> $OUT"
echo "  NO skipping in any arm. tier2+bypass need beat_threshold < 5ms to fire at all."

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  setarm t1; runbench "$w" >/dev/null 2>&1          # warmup, discarded
  for r in $(seq 1 "$REPS"); do
    off=$(( (r - 1) % N ))
    for i in $(seq 0 $((N - 1))); do
      a=${ARMS[$(( (i + off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      w0=$(G ivh_slowpath_wait_ns);  e0=$(G ivh_slowpath_wait_events)
      a0=$(G ivh_beat_tier1_fired);  b0=$(G ivh_beat_tier2_fired)
      f0=$(G ivh_head_bypass_fired); s0=$(G ivh_head_obs_stale)
      c0=$(G ivh_head_obs_actionable); o0=$(G ivh_head_obs_free_open)
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" \
        "$(( $(G ivh_slowpath_wait_ns)-w0 ))" "$(( $(G ivh_slowpath_wait_events)-e0 ))" \
        "$(( $(G ivh_beat_tier1_fired)-a0 ))" "$(( $(G ivh_beat_tier2_fired)-b0 ))" \
        "$(( $(G ivh_head_bypass_fired)-f0 ))" "$(( $(G ivh_head_obs_stale)-s0 ))" \
        "$(( $(G ivh_head_obs_actionable)-c0 ))" "$(( $(G ivh_head_obs_free_open)-o0 ))" \
        "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,wn,we,f1,f2,fb,os_,oa,ofo,out = sys.argv[1:17]
dur=float(t1)-float(t0); val=float(v)
perf=(1000.0/val if val>0 else 0.0) if dr=="lo" else val
open(out,'a').write(f"{w},{a},{r},{perf:.4f},{dur:.3f},{wn},{we},{f1},{f2},{fb},{os_},{oa},{ofo}\n")
print(f"  {w:20} {a:>11} r{r} perf={perf:11,.1f} dur={dur:6.2f}s "
      f"wait={int(wn)/1e9:7.2f}s t1={int(f1):8,} t2={int(f2):8,} bypass={int(fb):6,}")
PY
    done
  done
done
/root/spin_mode 2 >/dev/null 2>&1
echo 11000000 > $S/ivh_pv_beat_threshold; echo 0 > $S/ivh_head_bypass_probe
echo "WROTE $OUT"; echo T12B-DONE
