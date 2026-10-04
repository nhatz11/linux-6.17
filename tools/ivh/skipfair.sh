#!/bin/bash
# IS LOCK SKIPPING BEING TESTED FAIRLY?
#
# The recorded win (2026-09-22, waitskip.sh, 36 vCPU, n=8 paired) was dbench
# -11.43% acquisition wait, t=-4.85, replicated at a second spin threshold.
# Its design: "tier1+tier2 OFF in BOTH arms -- only ivh_pv_evict_enable +
# lookahead + nosteal differ."
#
# Every arm of the advisor sweep had TIER 1 ON. Tier 1 bails when
# prev->state != VCPU_RUNNING -- the SAME preempted-successor case eviction
# targets. So tier 1 may already be removing the waiters eviction exists to
# remove, leaving eviction to pay its overhead for work already done. That
# would make eviction look dead here while the recorded result stands.
#
# This harness tests exactly that, as a 2x2:
#   base_t1off   tier1 OFF, no skip   <- the recorded baseline
#   skip_t1off   tier1 OFF, skip ON   <- the recorded treatment
#   base_t1on    tier1 ON,  no skip
#   skip_t1on    tier1 ON,  skip ON   <- what the advisor sweep measured
#   pv           stock PV, for reference
#
# If skip helps at t1off but not t1on, tier 1 is stealing its cases and the
# paper's result is intact -- eviction is redundant WITH tier 1, not broken.
# If it helps in neither, the recorded win does not reproduce on this kernel.
#
# METRIC: per-acquisition WALL wait = ivh_slowpath_wait_ns / _events. That is
# the recorded metric, and it is the one fair to eviction: eviction does not
# halt, so on-CPU wait (wall - halted) can only charge its overhead and never
# credit its mechanism, which is faster acquisition. Halt time is still logged.
#
# tier 2 is OFF in every arm -- it is not the variable here and it dominates
# any wait metric when on.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
# Optional sizing override (e.g. SIZING=/root/ivh_tools/sizing36.sh for 36 vCPU).
# The registry is sized for 16 vCPU; running it unchanged on a larger box
# under-subscribes and makes a null uninformative.
[ -n "${SIZING:-}" ] && { source "$SIZING"; echo "  SIZING OVERRIDE: $SIZING"; }
SNAP="python3 /root/ivh_tools/snap.py"
BENCHES="${BENCHES:-dbench_16 hackbench_pipe_thr stressng_dentry}"
REPS="${REPS:-8}"
OUT="${OUT:-/root/ivh_tools/skipfair_$(date +%m%d-%H%M%S).csv}"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  local A="$1" t1=0 sk=0
  case "$A" in
    base_t1off) t1=0; sk=0 ;;
    skip_t1off) t1=0; sk=1 ;;
    base_t1on)  t1=1; sk=0 ;;
    skip_t1on)  t1=1; sk=1 ;;
    pv) ;;
    *) echo "FATAL: unknown arm $A"; exit 1 ;;
  esac
  # spin_mode FIRST: it sets tier2_enable=1 and forces beat_threshold, so every
  # feature write must follow it (see eval_final 5.5 / the advisor harness).
  if [ "$A" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
  else                   /root/spin_mode 2 >/dev/null 2>&1; fi
  echo 0 > $S/ivh_universal_eligible
  echo 0 > $S/ivh_pv_tier2_enable            # tier 2 off in EVERY arm
  echo 11000000 > $S/ivh_pv_beat_threshold
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  if [ "$A" != pv ]; then
    echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
    echo "$t1" > $S/ivh_pv_tier1_enable
    echo "$sk" > $S/ivh_pv_evict_enable
    if [ "$sk" = 1 ]; then
      # every documented optimization, matching the recorded best combo
      echo 1 > $S/ivh_pv_evict_node_stamp; echo 1 > $S/ivh_pv_evict_lookahead
      echo 1 > $S/ivh_pv_requeue_nosteal;  echo 2 > $S/ivh_pv_evict_hop_cap
      echo 4 > $S/ivh_pv_requeue_max;      echo 0 > $S/ivh_pv_skip_point
      echo 1100000 > $S/ivh_pv_evict_threshold
    fi
  else
    echo 0 > $S/ivh_pv_evict_enable
  fi
  echo 1 > $S/ivh_slowpath_wait_measure
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
  chk ivh_adaptive_mode $([ "$A" = pv ] && echo 0 || echo 2)
  chk ivh_pv_tier2_enable 0; chk ivh_head_bypass_probe 0
  chk ivh_universal_eligible 0; chk ivh_slowpath_wait_measure 1
  if [ "$A" != pv ]; then
    chk ivh_pv_tier1_enable "$t1"; chk ivh_pv_evict_enable "$sk"
    [ "$sk" = 0 ] || { chk ivh_pv_evict_lookahead 1; chk ivh_pv_requeue_nosteal 1
                       chk ivh_pv_evict_hop_cap 2; chk ivh_pv_evict_node_stamp 1; }
  fi
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

echo "workload,arm,rep,perf,dur_s,wall_ns,wait_events,halt_cyc,t1_fired,evict_marked,evict_requeued,evict_la_ref,evict_walks" > "$OUT"
ARMS=(pv base_t1off skip_t1off base_t1on skip_t1on)
N=${#ARMS[@]}
echo "skip fairness: $N arms x $REPS reps x $(echo $BENCHES|wc -w) workloads -> $OUT"
echo "  metric = per-acquisition WALL wait (the recorded metric, fair to eviction)"

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! not in registry"; continue; }
  setarm base_t1on; runbench "$w" >/dev/null 2>&1
  for r in $(seq 1 "$REPS"); do
    off=$(( (r-1) % N ))
    for i in $(seq 0 $((N-1))); do
      a=${ARMS[$(( (i+off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      $SNAP > /tmp/sf0.$$
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      $SNAP > /tmp/sf1.$$
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" /tmp/sf0.$$ /tmp/sf1.$$ "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,f0,f1,out=sys.argv[1:11]
def load(p):
    d={}
    for l in open(p):
        k,x=l.split(); d[k]=int(x)
    return d
b,e=load(f0),load(f1); D=lambda k: e.get(k,0)-b.get(k,0)
dur=float(t1)-float(t0); val=float(v)
perf=(1000.0/val if val>0 else 0.0) if dr=="lo" else val
wall=D('ivh_slowpath_wait_ns'); ev=D('ivh_slowpath_wait_events')
ht=D('ivh_node_halt_cycles.TOTAL')+D('ivh_head_halt_cycles.TOTAL')
open(out,'a').write(f"{w},{a},{r},{perf:.4f},{dur:.3f},{wall},{ev},{ht},"
  f"{D('ivh_beat_tier1_fired')},{D('ivh_evict_marked')},{D('ivh_evict_requeued')},"
  f"{D('ivh_evict_lookahead_refused')},{D('ivh_evict_walks')}\n")
per=wall/max(ev,1)
print(f"  {w:18} {a:>11} r{r} perf={perf:9,.1f} wall/acq={per:9,.0f}ns "
      f"wall={wall/1e9:6.2f}s t1={D('ivh_beat_tier1_fired'):8,} sk={D('ivh_evict_marked'):6,} "
      f"laref={D('ivh_evict_lookahead_refused'):6,} walks={D('ivh_evict_walks'):8,}")
PY
      rm -f /tmp/sf0.$$ /tmp/sf1.$$
    done
  done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 11000000 > $S/ivh_pv_beat_threshold
echo "WROTE $OUT"; echo SKIPFAIR-DONE
