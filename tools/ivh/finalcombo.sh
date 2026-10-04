#!/bin/bash
# FINAL COMBINATION SWEEP -- which spin-side mechanisms still help MIGRATION?
#
# Baseline is MIGRATION ONLY, not stock PV. Arms 2..7 are reported against arm 1,
# because the question is what ADDS to migration, not what beats PV.
#
# THE FIVE MECHANISMS, separated (each an independent factor):
#   t1   tier-1 halt     -- halt when the PREDECESSOR is halted.
#                          ivh_pv_tier1_enable. NOTE: this is upstream's entire
#                          pv_wait_early() check, not an IVH addition.
#   t2   tier-2 halt     -- halt when the PREDECESSOR's TSC beat is stale.
#                          ivh_pv_tier2_enable + ivh_pv_beat_threshold.
#   heh  head early halt -- the QUEUE HEAD halts because the LOCK HOLDER's
#                          acquisition stamp is stale (is_cs_preempted). This is
#                          the user's design rule 1. It has been DETECT-ONLY in
#                          every previous throughput run -- bail=1 has never been
#                          allowed to change behaviour. Counter ivh_cs_head_bailed.
#   hb   head bypass     -- the SUCCESSOR of the head clears the pending bit when
#                          the head is stale, reopening the unfair-steal path.
#                          Needs BOTH _enable and _probe: the bypass code lives
#                          inside ivh_head_observe(), whose only call site is
#                          gated on _probe. runs=1/hold=0 is the setting measured
#                          at 99.7% taken.
#   skip lock skipping   -- the HOLDER promotes the first LIVE waiter instead of
#                          the next one. evict_enable + node_stamp + lookahead +
#                          nosteal + hop_cap=2 + requeue_max=4.
#
# ARMS
#   PHASE A: mig / mig_heh / mig_heh_hb / mig_heh_hb_sk / mig_heh_hb_sk_t1
#   PHASE B: mig / mig_t1 / mig_t1_heh / mig_t1t2_heh
# Arm `mig` runs in BOTH phases so each carries its own drift-matched baseline;
# host load moves over hours and a cross-phase comparison would inherit that.
#
# SHARED-THRESHOLD COMPROMISE, stated because it is not free: tier 2 and head
# bypass BOTH read ivh_pv_beat_threshold (G-LOCK-47's "three predicates, three
# clocks" was only partly fixed -- eviction got its own knob, these two did not).
# At the shipped 11,000,000 (5 ms) NEITHER fires. This sweep uses 2,200,000
# (1 ms) whenever either is on: the value at which bypass was validated ("at a
# 1 ms staleness threshold bypass still fires 1,069 times/run ... genuine
# ~857 us+ absences") and at which tier 2 fires ~64,000 times/run. head early
# halt is unaffected -- it has its own clock (ivh_cs_noise_cycles / criterion).
#
# WORKLOAD SIZING: the registry is 16-vCPU sized; this box has 36. Left as-is
# deliberately -- the preemption these mechanisms react to comes from the HOST
# (measured 20.1% of vCPU time, 305 us mean involuntary wait), not from guest
# task count. The one mechanism this understates is `skip`, whose opportunity
# depends on QUEUE DEPTH; treat a skip null here as scale-limited, not refuted.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
# PARSEC direct-invocation override (eval_final 15.1): parsecmgmt is ~87% harness
# and must not be traced. EXTRA=/root/ivh_tools/parsec_direct.sh replaces the
# three parsecmgmt entries with the native.runconf commands.
[ -n "${EXTRA:-}" ] && { source "$EXTRA"; echo "  OVERRIDE: $EXTRA"; }
R="python3 /root/ivh_tools/read_ivh_counters.py"

BENCHES="${BENCHES:-ebizzy_mmap hackbench_pipe_thr dbench_16 wis_mmap2 fsmark_tmpfs schbench parsec_vips parsec_dedup parsec_bodytrack}"
REPS="${REPS:-3}"
PHASE="${PHASE:-A}"
OUT="${OUT:-/root/ivh_tools/finalcombo_${PHASE}_$(date +%m%d-%H%M%S).csv}"

CTRS="ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_node_halt_cycles ivh_head_halt_cycles ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_cs_head_bailed ivh_head_bypass_fired ivh_evict_marked ivh_evict_lookahead_refused"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  local A="$1" t1=0 t2=0 heh=0 hb=0 sk=0 beat=11000000
  case "$A" in
    mig)              t1=0; t2=0; heh=0; hb=0; sk=0 ;;
    mig_heh)          t1=0; t2=0; heh=1; hb=0; sk=0 ;;
    mig_heh_hb)       t1=0; t2=0; heh=1; hb=1; sk=0 ;;
    mig_heh_hb_sk)    t1=0; t2=0; heh=1; hb=1; sk=1 ;;
    mig_heh_hb_sk_t1) t1=1; t2=0; heh=1; hb=1; sk=1 ;;
    mig_t1)           t1=1; t2=0; heh=0; hb=0; sk=0 ;;
    mig_t1_heh)       t1=1; t2=0; heh=1; hb=0; sk=0 ;;
    mig_t1t2_heh)     t1=1; t2=1; heh=1; hb=0; sk=0 ;;
    *) echo "FATAL: unknown arm $A"; exit 1 ;;
  esac
  if [ "$t2" = 1 ] || [ "$hb" = 1 ]; then beat=2200000; fi

  # spin_mode FIRST: it sets tier1_enable=1, tier2_enable=1 and forces
  # beat_threshold=11000000, so every feature write must come after it.
  /root/spin_mode 2 >/dev/null 2>&1

  # ---- migration ON in every arm (it is the baseline mechanism) ----
  echo 2 > $S/ivh_pv_preempt_src
  echo 2 > $S/ivh_preempt_event_source        # without this Gate 2 reads a dead field
  echo 4000000 > $S/ivh_time_left_threshold_ns
  echo 8 > $S/ivh_max_concurrent
  echo 1 > $S/ivh_universal_eligible

  # ---- the five factors ----
  echo "$beat" > $S/ivh_pv_beat_threshold
  echo "$t1"   > $S/ivh_pv_tier1_enable
  echo "$t2"   > $S/ivh_pv_tier2_enable

  if [ "$heh" = 1 ]; then
    echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_cs_owner_enable
    echo 1 > $S/ivh_cs_owner_clear;   echo 0 > $S/ivh_cs_owner_fast
    echo 1 > $S/ivh_cs_scan;          echo 1 > $S/ivh_cs_criterion
    echo 1 > $S/ivh_cs_head_probe;    echo 1 > $S/ivh_cs_head_bail
  else
    echo 0 > $S/ivh_cs_head_bail;     echo 0 > $S/ivh_cs_head_probe
    echo 0 > $S/ivh_cs_criterion;     echo 0 > $S/ivh_cs_owner_enable
    echo 0 > $S/ivh_cs_scan
  fi

  if [ "$hb" = 1 ]; then
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
    echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
    echo 4 > $S/ivh_head_bypass_max
  else
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  fi

  if [ "$sk" = 1 ]; then
    echo 1 > $S/ivh_pv_evict_enable;    echo 1 > $S/ivh_pv_evict_node_stamp
    echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
    echo 2 > $S/ivh_pv_evict_hop_cap;   echo 4 > $S/ivh_pv_requeue_max
    echo 0 > $S/ivh_pv_skip_point;      echo 1100000 > $S/ivh_pv_evict_threshold
  else
    echo 0 > $S/ivh_pv_evict_enable
  fi

  echo 1 > $S/ivh_slowpath_wait_measure

  # ---- assert EVERY factor, on and off alike ----
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
  chk ivh_adaptive_mode 2
  chk ivh_universal_eligible 1
  chk ivh_preempt_event_source 2
  chk ivh_pv_tier1_enable "$t1"
  chk ivh_pv_tier2_enable "$t2"
  chk ivh_cs_head_bail "$heh"
  chk ivh_cs_head_probe "$heh"
  chk ivh_head_bypass_probe "$hb"
  chk ivh_pv_evict_enable "$sk"
  chk ivh_pv_beat_threshold "$beat"
  chk ivh_slowpath_wait_measure 1
  if [ "$sk" = 1 ]; then chk ivh_pv_evict_node_stamp 1; chk ivh_pv_evict_hop_cap 2; fi
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

case "$PHASE" in
  A) ARMS=(mig mig_heh mig_heh_hb mig_heh_hb_sk mig_heh_hb_sk_t1) ;;
  # Phase B omits `mig` by request -- Phase A's mig rows are the baseline for
  # arms 5-7 as well. COST OF THIS: arms 5-7 are then compared across the ~1 h
  # gap between phases, so host drift in between is charged to them instead of
  # cancelling. Phase A's own 3 mig reps per workload bound that risk (check
  # their spread before trusting a small Phase-B delta) but do not remove it.
  B) ARMS=(mig_t1 mig_t1_heh mig_t1t2_heh) ;;
  # PHASE C: the `mig` baseline alone, run AFTER phase B so arms 5-7 have a
  # same-era baseline. Host contention moved 25.2%% -> 33.18%% between A and B,
  # which is why A's mig rows cannot serve as B's baseline.
  C) ARMS=(mig) ;;
  *) echo "FATAL: PHASE must be A or B"; exit 1 ;;
esac
N=${#ARMS[@]}

echo "workload,arm,rep,perf,dur_s,wall_ns,wait_events,halt_cyc,halt_t1,halt_t2,migs,t1_fired,t2_fired,heh_bailed,bypass_fired,evict_marked,evict_la_ref" > "$OUT"
echo "FINAL COMBO phase $PHASE: $N arms x $(echo $BENCHES|wc -w) workloads x $REPS reps -> $OUT"
echo "  baseline = arm 'mig' (migration only). improvements are vs mig, NOT vs PV."

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  setarm mig; runbench "$w" >/dev/null 2>&1      # warmup, discarded
  for r in $(seq 1 "$REPS"); do
    off=$(( (r-1) % N ))
    for i in $(seq 0 $((N-1))); do
      a=${ARMS[$(( (i+off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      $R $CTRS > /tmp/fc0.$$ 2>/dev/null
      m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
      $R $CTRS > /tmp/fc1.$$ 2>/dev/null
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" "$((m1-m0))" \
               /tmp/fc0.$$ /tmp/fc1.$$ "$OUT" <<'PY'
import sys, re
w,a,r,v,dr,t0,t1,migs,f0,f1,out = sys.argv[1:12]
def load(p):
    d={}
    for line in open(p):
        m=re.match(r'^(\S+)\s+\[(\S+)\s*\]\s*=\s*(\d+)', line)
        if m: d[f"{m.group(1)}.{m.group(2)}"]=int(m.group(3)); continue
        m=re.match(r'^(\S+)\s*=\s*(\d+)', line)
        if m: d[m.group(1)]=int(m.group(2))
    return d
b,e=load(f0),load(f1); D=lambda k: e.get(k,0)-b.get(k,0)
dur=float(t1)-float(t0); val=float(v)
perf=(1000.0/val if val>0 else 0.0) if dr=="lo" else val
wall=D('ivh_slowpath_wait_ns'); ev=D('ivh_slowpath_wait_events')
ht=D('ivh_node_halt_cycles.TOTAL')+D('ivh_head_halt_cycles.TOTAL')
open(out,'a').write(f"{w},{a},{r},{perf:.4f},{dur:.3f},{wall},{ev},{ht},"
  f"{D('ivh_node_halt_cycles.TIER1')},{D('ivh_node_halt_cycles.TIER2')},{migs},"
  f"{D('ivh_beat_tier1_fired')},{D('ivh_beat_tier2_fired')},{D('ivh_cs_head_bailed')},"
  f"{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')},{D('ivh_evict_lookahead_refused')}\n")
per=wall/max(ev,1); onc=(wall-ht/2.2)/1e9
print(f"  {w:18} {a:>17} r{r} perf={perf:10,.1f} w/acq={per:8,.0f}ns onCPU={onc:6.2f}s "
      f"mig={int(migs):6,} t1={D('ivh_beat_tier1_fired'):8,} t2={D('ivh_beat_tier2_fired'):7,} "
      f"heh={D('ivh_cs_head_bailed'):6,} hb={D('ivh_head_bypass_fired'):5,} sk={D('ivh_evict_marked'):5,}")
PY
      rm -f /tmp/fc0.$$ /tmp/fc1.$$
    done
  done
  echo "  >> $w done"
done
echo "WROTE $OUT"; echo "FINALCOMBO-${PHASE}-DONE"
