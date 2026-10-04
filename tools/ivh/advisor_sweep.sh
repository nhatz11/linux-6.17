#!/bin/bash
# "Does the best combination always contain tier 2?"
#
# DESIGN: stock PV, then three mechanism combinations crossed with three
# staleness thresholds.
#
#   combos     t2            tier 2 only
#              hbskip        head bypass + lock skipping, NO tier 2
#              all           tier 2 + head bypass + lock skipping
#   thresholds 500 us (1,100,000 cyc) / 1 ms (2,200,000) / 2 ms (4,400,000)
#
# hbskip exists to be a fair competitor: it is the strongest combination that
# contains no tier 2, so if "the best combo always uses tier 2" is false, this
# arm is where it breaks. It is not a strawman -- it carries BOTH of the other
# mechanisms.
#
# ONE STALENESS CLOCK. The threshold sets every predicate in the arm that has
# one: ivh_pv_beat_threshold (tier 2's bail test AND head bypass's observer,
# which share it) and ivh_pv_evict_threshold (skipping). So "500 us" means the
# whole stack calls a vCPU preempted at 500 us, rather than each mechanism
# using a private clock. That keeps the three combos comparable at each T,
# which is the only way the ranking question is well posed.
#
# METRIC: on-CPU wait = wall - halted.
#   wall   = ivh_slowpath_wait_ns                                  (ns)
#   halted = (node_halt_cycles.TOTAL + head_halt_cycles.TOTAL)/2.2 (ns @2200MHz)
# Wall time alone scores an early-bail mechanism as null, because converting
# spin into halt leaves wall unchanged. on-CPU wait is the vCPU actually held
# while waiting -- the quantity that costs other threads.
#
# ADAPTIVE REPS: 3 per arm, extended to 5 for that workload if any arm's
# on-CPU wait CV exceeds CV_MAX (default 6%).
#
# TWO CLASSES OF WORKLOAD, TWO DIFFERENT TESTS. These six do not all carry the
# same amount of lock wait, and it would be dishonest to pool them as if they
# did. Measured aggregate slowpath wall wait per run:
#
#   hackbench_pipe_thr  52-60 s     EFFECT workloads -- enough wait for a
#   dbench_16            6.3-6.8 s  reduction to be resolvable. The question
#   ebizzy_mmap          3.1-3.8 s  here is "how much does it fall".
#   stressng_dentry      (tbd)
#
#   parsec_vips          0.25-0.35s DO-NO-HARM controls -- these already manage
#   sysbench_mutex       0.17-0.22s wait well, so there is nothing to win. The
#                                   question is the opposite one: does the
#                                   stack REGRESS a workload that is already
#                                   good? A null here is the desired result and
#                                   should be reported as non-inferiority (is
#                                   the arm within a stated margin of PV), NOT
#                                   as a failed effect.
#
# Reporting a do-no-harm workload's noisy percentage next to hackbench's would
# make a ~0.1 s baseline look like a real regression at the slightest jitter;
# absolute milliseconds and a margin are the honest presentation there.
#
# ORDER IS LOAD-BEARING: /root/spin_mode sets ivh_pv_tier2_enable=1 and forces
# ivh_pv_beat_threshold=11000000, so every feature sysctl is written AFTER it
# and asserted -- including asserted OFF when it should be off, which is what
# catches a leaked factor.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
SNAP="python3 /root/ivh_tools/snap.py"

BENCHES="${BENCHES:-hackbench_pipe_thr ebizzy_mmap dbench_16 sysbench_mutex parsec_vips stressng_dentry}"
REPS0="${REPS0:-3}"; REPS_HI="${REPS_HI:-5}"; CV_MAX="${CV_MAX:-6.0}"
OUT="${OUT:-/root/ivh_tools/advisor_$(date +%m%d-%H%M%S).csv}"

declare -A THR=( [500us]=1100000 [1ms]=2200000 [2ms]=4400000 )

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  local A="$1" want_t2=0 want_hb=0 want_sk=0 beat=11000000 ev=1100000
  if [ "$A" != pv ]; then
    local combo="${A%%@*}" tl="${A##*@}"
    beat="${THR[$tl]}"; ev="${THR[$tl]}"
    case "$combo" in
      t2)     want_t2=1 ;;
      hbskip) want_hb=1; want_sk=1 ;;
      all)    want_t2=1; want_hb=1; want_sk=1 ;;
      *) echo "FATAL: unknown combo $combo"; exit 1 ;;
    esac
  fi
  if [ "$A" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
  else                   /root/spin_mode 2 >/dev/null 2>&1; fi
  # --- all feature writes AFTER spin_mode ---
  echo 0 > $S/ivh_universal_eligible
  echo "$beat"    > $S/ivh_pv_beat_threshold
  echo "$want_t2" > $S/ivh_pv_tier2_enable
  echo "$want_sk" > $S/ivh_pv_evict_enable
  if [ "$want_hb" = 1 ]; then
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
    echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
  else
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  fi
  if [ "$want_sk" = 1 ]; then
    echo 1 > $S/ivh_pv_evict_node_stamp; echo 1 > $S/ivh_pv_evict_lookahead
    echo 1 > $S/ivh_pv_requeue_nosteal;  echo 2 > $S/ivh_pv_evict_hop_cap
    echo "$ev" > $S/ivh_pv_evict_threshold
  fi
  if [ "$A" != pv ]; then
    echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
    echo 1 > $S/ivh_pv_tier1_enable
  fi
  echo 1 > $S/ivh_slowpath_wait_measure
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1), want $2"; exit 1; }; }
  chk ivh_adaptive_mode $([ "$A" = pv ] && echo 0 || echo 2)
  chk ivh_pv_tier2_enable "$want_t2"; chk ivh_head_bypass_probe "$want_hb"
  chk ivh_pv_evict_enable "$want_sk"; chk ivh_pv_beat_threshold "$beat"
  chk ivh_universal_eligible 0;       chk ivh_slowpath_wait_measure 1
  [ "$want_sk" = 0 ] || { chk ivh_pv_evict_node_stamp 1; chk ivh_pv_evict_threshold "$ev"; }
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

one_rep(){   # $1=workload $2=arm $3=rep
  setarm "$2"; lookup "$1"
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
  $SNAP > /tmp/asnap0.$$
  local t0 t1 v; t0=$(date +%s.%N); v=$(runbench "$1"); t1=$(date +%s.%N)
  $SNAP > /tmp/asnap1.$$
  python3 - "$1" "$2" "$3" "${v:-0}" "$B_DR" "$t0" "$t1" /tmp/asnap0.$$ /tmp/asnap1.$$ "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,f0,f1,out = sys.argv[1:11]
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
h2=D('ivh_node_halt_cycles.TIER2')
halt=ht/2.2; spin=wall-halt
open(out,'a').write(f"{w},{a},{r},{perf:.4f},{dur:.3f},{wall},{ev},{ht},{h2},"
    f"{D('ivh_beat_tier2_fired')},{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')}\n")
print(f"  {w:18} {a:>12} r{r} perf={perf:10,.1f} wall={wall/1e9:6.2f}s halt={halt/1e9:6.2f}s "
      f"ONCPU={spin/1e9:6.2f}s  t2={D('ivh_beat_tier2_fired'):7,} hb={D('ivh_head_bypass_fired'):5,} sk={D('ivh_evict_marked'):5,}")
PY
  rm -f /tmp/asnap0.$$ /tmp/asnap1.$$
}

echo "workload,arm,rep,perf,dur_s,wall_ns,wait_events,halt_cyc,halt_cyc_t2,t2_fired,bypass_fired,evict_marked" > "$OUT"
ARMS=(pv)
for c in t2 hbskip all; do for t in 500us 1ms 2ms; do ARMS+=("$c@$t"); done; done
N=${#ARMS[@]}
echo "advisor sweep: $N arms x $(echo $BENCHES|wc -w) workloads, ${REPS0} reps (-> ${REPS_HI} if CV>${CV_MAX}%) -> $OUT"

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! not in registry"; continue; }
  setarm "${ARMS[1]}"; runbench "$w" >/dev/null 2>&1
  for r in $(seq 1 "$REPS0"); do
    off=$(( (r-1) % N ))
    for i in $(seq 0 $((N-1))); do one_rep "$w" "${ARMS[$(( (i+off) % N ))]}" "$r"; done
  done
  EXTRA=$(python3 - "$OUT" "$w" "$CV_MAX" <<'PY'
import csv,sys,collections,statistics as st
rows=[r for r in csv.DictReader(open(sys.argv[1])) if r['workload']==sys.argv[2]]
g=collections.defaultdict(list)
for r in rows:
    g[r['arm']].append(int(r['wall_ns'])-int(r['halt_cyc'])/2.2)
worst=0.0; who=""
for a,v in g.items():
    if len(v)>=2 and st.mean(v):
        cv=100*st.stdev(v)/st.mean(v)
        if cv>worst: worst, who = cv, a
print(f"{worst:.2f} {who}" if worst>float(sys.argv[3]) else "")
PY
)
  if [ -n "$EXTRA" ]; then
    echo "  >> CV ${EXTRA%% *}% on arm ${EXTRA##* } exceeds ${CV_MAX}% -- extending to ${REPS_HI} reps"
    for r in $(seq $((REPS0+1)) "$REPS_HI"); do
      off=$(( (r-1) % N ))
      for i in $(seq 0 $((N-1))); do one_rep "$w" "${ARMS[$(( (i+off) % N ))]}" "$r"; done
    done
  else
    echo "  >> all arms within ${CV_MAX}% CV -- ${REPS0} reps sufficient"
  fi
done
/root/spin_mode 2 >/dev/null 2>&1
echo 11000000 > $S/ivh_pv_beat_threshold; echo 0 > $S/ivh_head_bypass_probe
echo "WROTE $OUT"; echo ADVISOR-DONE
