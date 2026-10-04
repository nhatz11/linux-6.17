#!/bin/bash
# POINT 11: lock-skipping staleness threshold (ivh_pv_evict_threshold).
#
# The question: how stale must a waiter's stamp be before the holder skips it?
# Shipped value 1,100,000 cycles = 500 us at 2200 MHz (eval_final.md 5.5).
#
# Metrics, both in-kernel counters -- no probes, no overhead:
#   perf      workload metric, direction-corrected so higher is better
#   wait_ns   ivh_slowpath_wait_ns, aggregate spinlock wait. THE metric here;
#             the expectation is that wait falls, not that throughput rises.
#
# Firing evidence, logged per run so a null cannot be mistaken for a flat knob:
#   evict_marked      waiters marked VCPU_SKIPPED
#   evict_requeued    skips actually committed
#   lookahead_refused walk found no live waiter past the stale one, so refused
#
# TWO PREREQUISITES, BOTH OFF BY DEFAULT -- without them eviction fires ZERO
# times at every threshold from 500 us down to 10 us (measured 2026-09-28):
#   ivh_pv_evict_enable=1
#   ivh_pv_evict_node_stamp=1   <- the real blocker. With it 0, the staleness
#       test at qspinlock_paravirt.h:2524 falls through to the per-CPU
#       HEARTBEAT arm gated on ivh_pv_beat_threshold (11,000,000 = 5 ms), whose
#       noise floor is ~3 ms -- documented in 5.5 as effectively dead. With it
#       1 the per-node stamp is used (refreshed every publish_mask+1 = 4096 spin
#       iterations, ~47-110 us) and eviction fires 1,821-2,189 times per
#       hackbench run.
#
# head bypass is ON in the IVH arm, per the shipped full-stack config.
#
# MIGRATION IS OFF IN EVERY ARM (ivh_universal_eligible=0, asserted).
# Lock skipping is a queue-ORDER decision; vCPU placement is a separate mechanism
# and leaving it on injects variance far larger than the effect being swept. The
# smoke run showed why: with migration on, the arm that happened to follow the PV
# arm did 10,447 migrations while the others did 21-293, because the PV arm
# perturbs ivh_uc_capacity for 2-3 runs afterwards and Gate 1 then passes
# everything. A 500x swing in an uncontrolled mechanism cannot sit underneath a
# sweep whose expected effect is a few percent of wait time.
# Adaptive spinning, tier1, tier2 and head bypass all remain ON -- they are
# spin-side and unaffected by the eligibility gate. This matches the project's
# standing practice of isolating adaptive spinning from migration.
#
# Host contention is UNIFORM here (all 16 vCPUs ~995 capacity), not the
# 8-starved/8-healthy split points 7 and 8 required, so those results are not
# reproducible in this state and these numbers are not comparable to them.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
IPI(){ grep -E "^[[:space:]]*(RES|CAL|TLB):" /proc/interrupts \
       | awk '{for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i} END{print s+0}'; }

# 100us, 250us, 500us (shipped), 1ms, 2ms. Floor is the node stamp's own
# refresh period (~47-110us), so below ~100us the signal is its own noise.
VALUES="${VALUES:-220000 550000 1100000 2200000 4400000}"
BENCHES="${BENCHES:-hackbench_pipe_thr ebizzy_mmap parsec_dedup dbench_16 sysbench_mutex parsec_vips}"
REPS="${REPS:-5}"
OUT="${OUT:-/root/ivh_tools/p11full_$(date +%m%d-%H%M%S).csv}"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  if [ "$1" = pv ]; then
    echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
    [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: pv arm did not take"; exit 1; }
  else
    /root/spin_mode 2 >/dev/null 2>&1; echo 0 > $S/ivh_universal_eligible
    echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
    echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs
    echo 0 > $S/ivh_head_bypass_hold
    echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
    echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
    echo 1 > $S/ivh_pv_evict_node_stamp
    echo 4000000 > $S/ivh_time_left_threshold_ns; echo 8 > $S/ivh_max_concurrent
    echo "$1" > $S/ivh_pv_evict_threshold
    [ "$(cat $S/ivh_pv_evict_threshold)" = "$1" ] || { echo "FATAL: evict_threshold $1 rejected"; exit 1; }
    [ "$(cat $S/ivh_pv_evict_node_stamp)" = 1 ] || { echo "FATAL: node_stamp off -- eviction cannot fire"; exit 1; }
    [ "$(cat $S/ivh_pv_evict_enable)" = 1 ] || { echo "FATAL: evict_enable off"; exit 1; }
    [ "$(cat $S/ivh_head_bypass_enable)" = 1 ] || { echo "FATAL: head_bypass off"; exit 1; }
    [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: ivh arm did not take"; exit 1; }
    [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "FATAL: migration on -- confounds the sweep"; exit 1; }
  fi
  echo 1 > $S/ivh_slowpath_wait_measure
  [ "$(cat $S/ivh_slowpath_wait_measure)" = 1 ] || { echo "FATAL: wait_measure off"; exit 1; }
  echo 0 > $S/ivh_tks_sampler_ns
}

runbench(){
  lookup "$1" || { echo 0; return; }
  if [ "$B_EXT" = TIME ]; then
    local a b
    a=$(date +%s.%N); ( cd "$B_DIR" && eval "$B_CMD" ) >/dev/null 2>&1; b=$(date +%s.%N)
    python3 -c "print(f'{$b-$a:.4f}')"
  else
    ( cd "$B_DIR" && eval "$B_CMD" 2>&1 ) | eval "$B_EXT" | tail -1
  fi
}

echo "workload,arm_cyc,rep,perf,dur_s,ipi,wait_ns,wait_events,ev_marked,ev_requeued,ev_lookahead_ref,ev_halt_averted,migs" > "$OUT"
ARMS=(pv $VALUES)
N=${#ARMS[@]}
echo "point 11: $N arms x $REPS reps x $(echo $BENCHES | wc -w) workloads -> $OUT"
echo "  knob ivh_pv_evict_threshold; prerequisites evict_enable=1 node_stamp=1; head bypass ON"

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  setarm "${ARMS[1]}"; runbench "$w" >/dev/null 2>&1     # warmup, discarded
  for r in $(seq 1 "$REPS"); do
    off=$(( (r - 1) % N ))
    for i in $(seq 0 $((N - 1))); do
      a=${ARMS[$(( (i + off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      w0=$(G ivh_slowpath_wait_ns); e0=$(G ivh_slowpath_wait_events)
      m0=$(G ivh_evict_marked); q0=$(G ivh_evict_requeued)
      l0=$(G ivh_evict_lookahead_refused); h0=$(G ivh_evict_halt_averted)
      g0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0); i0=$(IPI)
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      w1=$(G ivh_slowpath_wait_ns); e1=$(G ivh_slowpath_wait_events)
      m1=$(G ivh_evict_marked); q1=$(G ivh_evict_requeued)
      l1=$(G ivh_evict_lookahead_refused); h1=$(G ivh_evict_halt_averted)
      g1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0); i1=$(IPI)
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" "$((i1-i0))" "$((w1-w0))" \
               "$((e1-e0))" "$((m1-m0))" "$((q1-q0))" "$((l1-l0))" "$((h1-h0))" \
               "$((g1-g0))" "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,ipi,wn,we,mk,rq,lr,ha,mg,out = sys.argv[1:17]
dur = float(t1) - float(t0)
val = float(v)
perf = (1000.0/val if val > 0 else 0.0) if dr == "lo" else val
ac = "0" if a == "pv" else a
open(out, 'a').write(f"{w},{ac},{r},{perf:.4f},{dur:.3f},{ipi},{wn},{we},{mk},{rq},{lr},{ha},{mg}\n")
lab = 'PV' if a == 'pv' else f"{int(a)//2200}us"
print(f"  {w:20} {lab:>7} r{r} perf={perf:11,.1f} dur={dur:6.2f}s wait={int(wn)/1e9:8.3f}s "
      f"ev(mark={int(mk):7,} req={int(rq):7,} la_ref={int(lr):7,})")
PY
    done
  done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 2 > $S/ivh_preempt_event_source; echo 1100000 > $S/ivh_pv_evict_threshold
echo "WROTE $OUT"
echo P11FULL-DONE
