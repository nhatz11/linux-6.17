#!/bin/bash
# POINT 8: Gate 4 concurrency-budget sweep (ivh_max_concurrent).
# Arms 2 / 4 / 8 / 16 = 1/8, 1/4, 1/2, 1x nproc on this 16-vCPU guest.
# Gate 4 is fair.c:13942  if (atomic_read(&ivh_in_schedule) >= ivh_max_concurrent)
# and has NO reject counter, so the knob's effect is observed by sampling
# ivh_in_schedule directly per arm (max, and the fraction of samples AT the cap)
# rather than inferred from throughput alone -- the gap point 7 had at its low end.
# NOTE ivh_in_schedule is held across the WHOLE set_cpus_allowed_ptr call,
# including the ~1.5 ms target-runqueue wait, so occupancy is dominated by that. Four measurements per run, kept SEPARATE
# (no ratios -- they get combined downstream by hand):
#   perf         workload metric, direction-corrected so higher is always better
#   ipi          RES+CAL+TLB delta, cache-coherence traffic proxy (no PMU on TDX)
#   wait_ns      aggregate spinlock wait, ivh_slowpath_wait_ns (in-kernel counter)
#   mig_cost_ns  MIGRATION COST = stopper dispatch + the actual move.
#                EXCLUDES the target-runqueue wait entirely.
#
# Migration cost is measured directly, not by subtraction:
#   dispatch = cpu_stop_queue_work -> migration_cpu_stop entry   (~1 us)
#   move     = migration_cpu_stop entry -> return                (~3 us modal,
#                                                                 7.2 us mean)
# Cross-check: an independent method (total set_cpus_allowed_ptr time minus the
# sched_info.run_delay delta) gave 6.1 us where dispatch+move gives ~8 us --
# two unrelated instruments agreeing to ~15%.
#
# mig_rqwait_ns is ALSO logged, but is NOT part of mig_cost_ns. It is the time
# the migrated thread waits to be scheduled on the target (~1.5 ms modal,
# ~= the 2.8 ms EEVDF base_slice_ns), i.e. a property of how loaded the target
# is, not a cost of the migration mechanism.
#
# g1_reject is logged per run to detect capacity-unsettled arms: the PV arm
# perturbs ivh_uc_capacity for ~2-3 runs afterwards, during which Gate 1 passes
# everything and the arm is NOT comparable. Filter on it before averaging.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
M(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
IPI(){ grep -E "^[[:space:]]*(RES|CAL|TLB):" /proc/interrupts \
       | awk '{for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i} END{print s+0}'; }

VALUES="${VALUES:-2 4 8 16}"
BENCHES="${BENCHES:-hackbench_pipe_thr ebizzy_mmap parsec_dedup dbench_16 sysbench_mutex parsec_vips}"
REPS="${REPS:-3}"
OUT="${OUT:-/root/ivh_tools/p8full_$(date +%m%d-%H%M%S).csv}"

cat > /tmp/p8split.bt <<'BT'
/* MIGRATION COST = dispatch + move. No runqueue wait. */
kprobe:cpu_stop_queue_work { @q[arg0] = nsecs; }
kprobe:migration_cpu_stop {
  @s[tid] = nsecs;
  if (@q[cpu]) { @disp_ns = sum(nsecs - @q[cpu]); @disp_n = count(); delete(@q[cpu]); }
}
kretprobe:migration_cpu_stop /@s[tid]/ {
  $d = nsecs - @s[tid];
  @stop_ns = sum($d); @stop_n = count();
  delete(@s[tid]);
}
/* context only -- target runqueue wait, NOT counted as migration cost */
kprobe:set_cpus_allowed_ptr { @sq[tid]=(@sq[tid]+1)%2; @t0[tid]=nsecs; @rd0[tid]=curtask->sched_info.run_delay; }
kretprobe:set_cpus_allowed_ptr /@t0[tid]/ {
  $rq = curtask->sched_info.run_delay - @rd0[tid];
  $tot = nsecs - @t0[tid];
  if ($rq > $tot) { $rq = $tot; }
  if (@sq[tid] == 1) { @rqwait_ns = sum($rq); @sca_n = count(); }
  delete(@t0[tid]); delete(@rd0[tid]);
}
END { clear(@q); clear(@s); clear(@t0); clear(@rd0); clear(@sq); }
BT

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
    /root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
    echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
    for k in ivh_pv_tier1_enable ivh_pv_tier2_enable ivh_head_bypass_enable \
             ivh_head_bypass_runs ivh_pv_evict_enable ivh_pv_evict_lookahead \
             ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
    echo 0 > $S/ivh_head_bypass_hold; echo 2 > $S/ivh_pv_evict_hop_cap
    echo 4000000 > $S/ivh_time_left_threshold_ns   # point 7 verdict; hold fixed
    echo "$1" > $S/ivh_max_concurrent
    [ "$(cat $S/ivh_max_concurrent)" = "$1" ] || { echo "FATAL: max_concurrent $1 rejected"; exit 1; }
    [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: ivh arm did not take"; exit 1; }
  fi
  echo 1 > $S/ivh_slowpath_wait_measure
  [ "$(cat $S/ivh_slowpath_wait_measure)" = 1 ] || { echo "FATAL: wait_measure off"; exit 1; }
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

echo "workload,arm_ns,rep,perf,dur_s,ipi,wait_ns,wait_events,migs,mig_n,mig_disp_ns,mig_move_ns,mig_cost_ns,mig_rqwait_ns,g1_reject,sc_max,sc_at_cap,sc_samples" > "$OUT"
ARMS=(pv $VALUES)
N=${#ARMS[@]}
echo "point 8: $N arms x $REPS reps x $(echo $BENCHES | wc -w) workloads -> $OUT"
echo "  migration cost = stopper dispatch + move.  runqueue wait logged separately."

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
      BTF=/tmp/p8bt.$$
      : > "$BTF"
      BP=""
      SCF=/tmp/p8sc.$$
      : > "$SCF"
      SC=""
      if [ "$a" != pv ]; then
        timeout 300 bpftrace /tmp/p8split.bt > "$BTF" 2>&1 &
        BP=$!
        sleep 3
        # sample the concurrency Gate 4 actually sees. reopen-per-sample is
        # mandatory: a held-open /proc/kcore fd reports a FROZEN value and
        # previously faked "ivh_in_schedule is always 0" twice.
        python3 /root/ivh_tools/sample_atomic.py ivh_in_schedule 600 0.0005 > "$SCF" 2>&1 &
        SC=$!
      fi
      w0=$(G ivh_slowpath_wait_ns); e0=$(G ivh_slowpath_wait_events); m0=$(M)
      g0=$(G ivh_steal_imminent_capacity_reject); i0=$(IPI)
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      w1=$(G ivh_slowpath_wait_ns); e1=$(G ivh_slowpath_wait_events); m1=$(M)
      g1=$(G ivh_steal_imminent_capacity_reject); i1=$(IPI)
      SCMAX=0; SCCAP=0; SCN=0
      if [ -n "$SC" ]; then
        kill -TERM "$SC" 2>/dev/null; wait "$SC" 2>/dev/null
        SCMAX=$(grep -oP 'max=\K[0-9]+' "$SCF" 2>/dev/null | head -1); SCMAX=${SCMAX:-0}
        SCN=$(grep -oP 'samples=\K[0-9]+' "$SCF" 2>/dev/null | head -1); SCN=${SCN:-0}
        SCCAP=$(python3 - "$SCF" "$a" <<'PZ'
import re,sys
try: txt=open(sys.argv[1]).read()
except Exception: print(0); raise SystemExit
m=re.search(r'distribution: (\{.*\})',txt)
cap=int(sys.argv[2]) if sys.argv[2].isdigit() else 0
if not m or not cap: print(0); raise SystemExit
d=eval(m.group(1))
print(sum(v for k,v in d.items() if k>=cap))
PZ
); SCCAP=${SCCAP:-0}
      fi
      rm -f "$SCF"
      MN=0; MD=0; MV=0; MQ=0
      if [ -n "$BP" ]; then
        kill -INT "$BP" 2>/dev/null
        wait "$BP" 2>/dev/null
        MN=$(grep -oP '^@stop_n: \K[0-9]+' "$BTF" 2>/dev/null | head -1); MN=${MN:-0}
        MD=$(grep -oP '^@disp_ns: \K[0-9]+' "$BTF" 2>/dev/null | head -1); MD=${MD:-0}
        MV=$(grep -oP '^@stop_ns: \K[0-9]+' "$BTF" 2>/dev/null | head -1); MV=${MV:-0}
        MQ=$(grep -oP '^@rqwait_ns: \K[0-9]+' "$BTF" 2>/dev/null | head -1); MQ=${MQ:-0}
      fi
      rm -f "$BTF"
      python3 - "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" "$((i1-i0))" "$((w1-w0))" \
               "$((e1-e0))" "$((m1-m0))" "$MN" "$MD" "$MV" "$MQ" "$((g1-g0))" \
               "$SCMAX" "$SCCAP" "$SCN" "$OUT" <<'PY'
import sys
w,a,r,v,dr,t0,t1,ipi,wn,we,mg,mn,md,mv,mq,g1,scmax,sccap,scn,out = sys.argv[1:21]
dur = float(t1) - float(t0)
val = float(v)
perf = (1000.0/val if val > 0 else 0.0) if dr == "lo" else val
cost = int(md) + int(mv)          # dispatch + move. NO runqueue wait.
an = "0" if a == "pv" else a
open(out, 'a').write(f"{w},{an},{r},{perf:.4f},{dur:.3f},{ipi},{wn},{we},{mg},{mn},{md},{mv},{cost},{mq},{g1},{scmax},{sccap},{scn}\n")
lab = 'PV' if a == 'pv' else f"cap{a}"
per = cost/int(mn) if int(mn) else 0
print(f"  {w:20} {lab:>7} r{r} perf={perf:11,.1f} dur={dur:6.2f}s wait={int(wn)/1e9:7.2f}s "
      f"migs={int(mg):7,} cost={cost/1e6:7.1f}ms ({per/1000:5.1f}us/mig) "
      f"sc_max={scmax} at_cap={int(sccap):5,}/{int(scn):6,} g1={int(g1):9,}")
PY
    done
  done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 2 > $S/ivh_preempt_event_source
echo 8 > $S/ivh_max_concurrent
echo 4000000 > $S/ivh_time_left_threshold_ns
echo "WROTE $OUT"
echo P8FULL-DONE
