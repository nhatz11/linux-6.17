#!/bin/bash
# EVAL POINT 15: lock-acquisition rate per workload, to stratify the
# parameter-sensitivity tests (points 7, 8, 11) across lock behaviour instead
# of running all 19 workloads through every arm.
#
# ONE ARM ONLY: stock PV (spin_mode 1). This characterises the WORKLOAD, not
# IVH, so there is nothing to compare against and no second arm is needed.
#
# TWO INDEPENDENT INSTRUMENTS, both safe:
#   contended acq/s  perf stat -a -e lock:contention_begin. Exact. This is the
#                    primary stratifier: IVH only acts on CONTENDED
#                    acquisitions; an uncontended fastpath acquire is ~20ns and
#                    IVH never touches it.
#   slowpath holds/s sum of ivh_cs_prev_hold_hist. Counts acquisitions that
#                    reached ivh_cs_owner_stamp, i.e. after the MCS queue
#                    formed (qspinlock.c:611 -- "The uncontended fastpath is
#                    NOT touched"). A cross-check on the ranking.
#
# NOT USED: fentry on _raw_spin_lock*. bpftrace warns it is a "dangerous
# function" that risks kernel deadlock AND drops events under its mitigation.
# A lossy counter cannot stratify, and this VM has been hung twice by new lock
# paths. The contention tracepoint gives an exact count with neither risk.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
source /root/ivh_tools/bench_guard.sh 2>/dev/null
REPS="${REPS:-3}"
OUT="${OUT:-/root/ivh_tools/point15_$(date +%m%d-%H%M%S).csv}"
R="python3 /root/ivh_tools/read_ivh_counters.py"
HSUM(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'sum=[0-9]+' | cut -d= -f2 || echo 0; }

# PV arm, and CS stamping armed AFTER spin_mode (spin_mode clears owner_enable)
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: PV arm did not take"; exit 1; }
for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do echo 1 > $S/$k; done
[ "$(cat $S/ivh_cs_owner_enable)" = 1 ] || { echo "FATAL: CS stamping disarmed"; exit 1; }
echo 0 > $S/ivh_tks_sampler_ns

echo "workload,rep,seconds,contended,holds,metric" > "$OUT"
echo "point 15: lock-acquisition rate, PV arm, REPS=$REPS -> $OUT"
printf "%-22s %8s %12s %12s %12s %10s\n" workload sec contended cont_per_s holds holds_per_s

# NHextend lives only in IVH_CORE (separate env-var invocation), so it is
# appended explicitly or it would be silently skipped.
NHX="nhextend_full|hi|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16 2>&1|grep -oP '^Ran for \\K[0-9]+'|+64.0"
for w in "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}" "$NHX"; do
	IFS='|' read -r NAME DIR CMD EXT REC EXTRA <<< "$w|"
	case "$NAME" in
	  hackbench_sock_thr|hackbench_pipe_proc|perf_epoll_wait|perf_syscall_basic) continue ;;
	  stressng_flock|stressng_mmap|stressng_sock|stressng_pipe|stressng_futex) continue ;;
	  wis_mmap1) continue ;;
	esac
	for r in $(seq 1 "$REPS"); do
		sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
		sleep 1
		h0=$(HSUM); t0=$(date +%s.%N)
		cnt=$(perf stat -a -e lock:contention_begin -x, -- \
		       bash -c "$CMD >/dev/null 2>&1" 2>&1 | awk -F, '/contention_begin/{print $1}')
		t1=$(date +%s.%N); h1=$(HSUM)
		python3 - "$NAME" "$r" "$t0" "$t1" "${cnt:-0}" "$h0" "$h1" >> "$OUT" <<'PY'
import sys
n,r,t0,t1,c,h0,h1=sys.argv[1:8]
d=float(t1)-float(t0)
print(f"{n},{r},{d:.3f},{c},{int(h1)-int(h0)},")
PY
		tail -1 "$OUT" | awk -F, -v n="$NAME" '{printf "%-22s %8.2f %12s %12.0f %12s %10.0f\n", n, $3, $4, $4/$3, $5, $5/$3}'
	done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible; echo 2 > $S/ivh_preempt_event_source
echo "WROTE $OUT"; echo POINT15-DONE
