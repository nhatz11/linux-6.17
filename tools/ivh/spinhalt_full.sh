#!/bin/bash
# SPIN-vs-HALT DECOMPOSITION: does any combination of tier2 / head bypass /
# lock skipping reduce the wait a waiter BURNS, even when wall wait is flat?
#
# WHY THIS EXISTS. ivh_slowpath_wait_ns is WALL time in the slowpath. It
# conflates two things with completely different costs:
#   spinning -- burns a vCPU, stealing cycles from every other runnable thread
#   halted   -- yields the vCPU; costs only the pv_wait/kick round trip
# An early-bail mechanism (tier 1, tier 2) converts spin into halt. Wall wait
# stays flat or rises slightly, so measuring wall wait alone scores those
# mechanisms as null -- while missing the thing they actually do. The claim
# "IVH reduces the wait" is defensible on BURNED time and not on wall time, so
# burned time is what this harness measures.
#
# THE DECOMPOSITION, per run:
#   wall_ns  = ivh_slowpath_wait_ns                      (sched_clock, ns)
#   halt_ns  = (node_halt_cycles.TOTAL + head_halt_cycles.TOTAL) / 2200
#   spin_ns  = wall_ns - halt_ns                         <-- THE METRIC
# TSC is 2200.000 MHz (dmesg "tsc: Detected"), clocksource=tsc.
#
# ATTRIBUTION IS FREE: node_halt_cycles is an array indexed by bail cause, so
# node_halt_cycles.TIER2 is exactly "cycles halted because tier 2 fired" --
# wait that tier 2 moved off the CPU. Same for TIER1 and EXHAUST. That lets the
# paper say which mechanism converted how much, not just that something did.
#
# ADDITIVE LADDER -- each arm adds ONE mechanism, so any effect is attributable:
# FULL 2x2x2 FACTORIAL over {tier2, head bypass, lock skipping}, plus stock PV.
# A factorial rather than a ladder because the mechanisms are not independent:
# tier 2 and head bypass share ivh_pv_beat_threshold, and skipping changes queue
# order underneath both. Only a factorial separates main effects from
# interactions; a ladder confounds each step with every step before it.
#
#   pv           stock pvqspinlock
#   ivh          adaptive mode, tier 1 only. NOTE tier 1 IS upstream's entire
#                pv_wait_early() check (qspinlock_paravirt.h:1361), so this arm
#                is NOT "PV + tier1" -- it is PV's own early-bail logic running
#                under the IVH halt/wake path. The mode is the only variable.
#   t2           + tier 2 @ beat_threshold 1 ms (at the shipped 5 ms it fires 0)
#   t2_hb        + head bypass (needs BOTH enable=1 AND probe=1)
#   t2_hb_skip   + lock skipping @ evict_threshold 500 us (needs node_stamp=1)
#
# Migration is OFF in every arm. Host contention is uniform.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
SNAP="python3 /root/ivh_tools/snap.py"

BENCHES="${BENCHES:-hackbench_pipe_thr ebizzy_mmap dbench_16 sysbench_mutex parsec_vips stressng_dentry}"
REPS="${REPS:-5}"
OUT="${OUT:-/root/ivh_tools/spinhalt_$(date +%m%d-%H%M%S).csv}"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  # Arm names encode the factorial: presence of "t2", "hb", "skip" in the name
  # turns that factor on. "ivh" is all three off -- tier 1 only, which IS
  # upstream's own pv_wait_early() check, so it is the IVH-mode control rather
  # than an added mechanism.
  #
  # ORDER IS LOAD-BEARING: /root/spin_mode sets ivh_pv_tier2_enable=1 (line 131)
  # and forces ivh_pv_beat_threshold=11000000. Every feature sysctl must
  # therefore be written AFTER spin_mode, never before, or spin_mode silently
  # reverts it. Each factor is asserted to its intended value below -- including
  # asserted to OFF when it should be off, which is what catches this class of
  # bug rather than letting a leaked factor confound the factorial.
  local A="$1" want_t2=0 want_hb=0 want_sk=0 want_beat=11000000
  case "$A" in *t2*)   want_t2=1; want_beat=2200000 ;; esac
  case "$A" in *hb*)   want_hb=1; want_beat=2200000 ;; esac
  case "$A" in *skip*) want_sk=1 ;; esac

  if [ "$A" = pv ]; then
    /root/spin_mode 1 >/dev/null 2>&1
    want_t2=0; want_hb=0; want_sk=0; want_beat=11000000
  else
    /root/spin_mode 2 >/dev/null 2>&1
  fi

  # --- everything below runs AFTER spin_mode ---
  echo 0 > $S/ivh_universal_eligible
  echo "$want_beat" > $S/ivh_pv_beat_threshold
  echo "$want_t2"   > $S/ivh_pv_tier2_enable
  echo "$want_sk"   > $S/ivh_pv_evict_enable
  if [ "$want_hb" = 1 ]; then
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
    echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
  else
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  fi
  if [ "$want_sk" = 1 ]; then
    echo 1 > $S/ivh_pv_evict_node_stamp; echo 1 > $S/ivh_pv_evict_lookahead
    echo 1 > $S/ivh_pv_requeue_nosteal;  echo 2 > $S/ivh_pv_evict_hop_cap
    echo 1100000 > $S/ivh_pv_evict_threshold
  fi
  if [ "$A" != pv ]; then
    echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
    echo 1 > $S/ivh_pv_tier1_enable
  fi
  echo 1 > $S/ivh_slowpath_wait_measure

  # --- assert EVERY factor, on and off alike ---
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1), want $2"; exit 1; }; }
  chk ivh_adaptive_mode $([ "$A" = pv ] && echo 0 || echo 2)
  chk ivh_pv_tier2_enable   "$want_t2"
  chk ivh_head_bypass_probe "$want_hb"
  chk ivh_pv_evict_enable   "$want_sk"
  chk ivh_pv_beat_threshold "$want_beat"
  chk ivh_universal_eligible 0
  chk ivh_slowpath_wait_measure 1
  [ "$want_sk" = 0 ] || chk ivh_pv_evict_node_stamp 1
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

echo "workload,arm,rep,perf,dur_s,wall_ns,wait_events,halt_cyc_total,halt_cyc_t1,halt_cyc_t2,halt_cyc_exh,halt_cyc_head,halt_ev_total,node_spin_iters,head_spin_iters,t1_fired,t2_fired,bypass_fired,evict_marked" > "$OUT"
ARMS=(pv ivh t2 hb skip t2_hb t2_skip hb_skip t2_hb_skip)
N=${#ARMS[@]}
echo "spin/halt: $N arms x $REPS reps x $(echo $BENCHES | wc -w) workloads -> $OUT"
echo "  metric = BURNED spin time (wall - halt), plus halt attributed by bail cause"

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  setarm ivh; runbench "$w" >/dev/null 2>&1
  for r in $(seq 1 "$REPS"); do
    off=$(( (r - 1) % N ))
    for i in $(seq 0 $((N - 1))); do
      a=${ARMS[$(( (i + off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      $SNAP > /tmp/snap0.$$
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      $SNAP > /tmp/snap1.$$
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" /tmp/snap0.$$ /tmp/snap1.$$ "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,f0,f1,out = sys.argv[1:11]
def load(p):
    d={}
    for line in open(p):
        k,x=line.split(); d[k]=int(x)
    return d
b,e=load(f0),load(f1)
D=lambda k: e.get(k,0)-b.get(k,0)
dur=float(t1)-float(t0); val=float(v)
perf=(1000.0/val if val>0 else 0.0) if dr=="lo" else val
wall=D('ivh_slowpath_wait_ns'); ev=D('ivh_slowpath_wait_events')
hn=D('ivh_node_halt_cycles.TOTAL'); hh=D('ivh_head_halt_cycles.TOTAL')
h1=D('ivh_node_halt_cycles.TIER1'); h2=D('ivh_node_halt_cycles.TIER2')
hx=D('ivh_node_halt_cycles.EXHAUST')
hev=D('ivh_node_halt_events.TOTAL')+D('ivh_head_halt_events.TOTAL')
ns=D('ivh_node_spin_iters_sum'); hs=D('ivh_head_spin_iters_sum')
open(out,'a').write(f"{w},{a},{r},{perf:.4f},{dur:.3f},{wall},{ev},{hn+hh},{h1},{h2},{hx},{hh},{hev},"
                   f"{ns},{hs},{D('ivh_beat_tier1_fired')},{D('ivh_beat_tier2_fired')},"
                   f"{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')}\n")
halt_ns=(hn+hh)/2.2; spin_ns=wall-halt_ns
pct=100*halt_ns/wall if wall else 0
print(f"  {w:18} {a:>11} r{r} perf={perf:10,.1f} wall={wall/1e9:7.2f}s "
      f"halt={halt_ns/1e9:7.2f}s ({pct:5.1f}%) SPIN={spin_ns/1e9:7.2f}s  "
      f"t2halt={h2/2.2/1e9:6.2f}s bypass={D('ivh_head_bypass_fired'):5,} skip={D('ivh_evict_marked'):5,}")
PY
      rm -f /tmp/snap0.$$ /tmp/snap1.$$
    done
  done
done
/root/spin_mode 2 >/dev/null 2>&1
echo 11000000 > $S/ivh_pv_beat_threshold; echo 0 > $S/ivh_head_bypass_probe
echo "WROTE $OUT"; echo SPINHALT-DONE
