#!/bin/bash
# ivh_probe.sh -- generic IVH knob prober.
#
#   KNOB=ivh_time_left_threshold_ns VALUES="500000 1000000 2000000 4000000 8000000 16000000" \
#   BENCHES="hackbench_thr fsmark dbench" REPS=5 bash ivh_probe.sh
#
# Also drives Gate 4  (KNOB=ivh_max_concurrent VALUES="1 2 4 8 16")
# and later lock-skip staleness (KNOB=ivh_pv_evict_threshold).
#
# Per (bench, arm) it records three families, plus controls:
#   PERF      bench throughput, higher = better, normalised to the pv arm
#   COHERENCE (RES+CAL+TLB) IPI deltas per 1000 units of WORK COMPLETED,
#             where work = throughput x wall-clock. Dividing by throughput
#             alone is WRONG: hackbench does fixed work (-l250000), so a
#             faster arm finishes sooner and a throughput-normalised IPI
#             figure double-counts the speedup (it printed 21,505,036 for
#             the PV arm in the 2026-09-27 smoke test). work = thr x dur is
#             correct for fixed-work AND fixed-time (-t 15) benchmarks alike.
#             Per 1000 units of work. The PMU is NOT
#             virtualised on this TDX guest -- cache-misses and LLC-load-misses
#             read <not supported>, and resctrl/RDT is absent -- so this is
#             coherence TRAFFIC, not a miss rate. Labelled as such everywhere.
#   RATIO     (preempted_cs_pv - preempted_cs_arm) / migrations_arm
#             "preempted critical sections avoided per migration performed",
#             i.e. benefit per unit of work done. preempted_cs = holds in
#             ivh_cs_prev_hold_hist buckets >=20 (>=476us at 2200MHz), the
#             same definition used for eval point 5.3.
#
# TRAP THIS SCRIPT EXISTS TO AVOID: /root/spin_mode CLEARS ivh_cs_owner_enable
# and ivh_cs_owner_clear. Arming CS tracking once at startup is not enough --
# every setarm() calls spin_mode and would silently disarm stamping, making
# the preempted-CS column read a flat zero. arm_cs() therefore runs AFTER
# spin_mode on every single arm. The Gate 2 sweep of 2026-09-27 lost its
# entire long% column to exactly this.
set -u
S=/proc/sys/kernel
R="python3 /root/ivh_tools/read_ivh_counters.py"
KNOB="${KNOB:-ivh_time_left_threshold_ns}"
VALUES="${VALUES:-500000 1000000 2000000 4000000 8000000 16000000}"
BENCHES="${BENCHES:-hackbench_thr fsmark}"
REPS="${REPS:-5}"
RAW="${RAW:-/tmp/ivh_probe.raw}"

M(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
HIST(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' || echo "[]"; }
IPI(){ grep -E "^[[:space:]]*(RES|CAL|TLB):" /proc/interrupts \
       | awk '{for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i} END{print s+0}'; }
arm_cs(){ for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do
            echo 1 > $S/$k 2>/dev/null; done; }

# ---- workloads come from the REGISTRY, not from a table here ---------------
# ivh_benchmarks.sh Set A is generated verbatim from campaign/benchmarks.tsv,
# and the two sub-second workloads carry scaled variants. Hand-writing configs
# here is what produced 5 wrong invocations on 2026-09-27.
source /root/ivh_tools/ivh_benchmarks.sh
lookup(){ # $1=name -> sets B_DIR B_DIR2 B_CMD B_EXT
	local e n dir dr c x rec
	for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
		IFS='|' read -r n dir dr c x rec <<< "$e"
		if [ "$n" = "$1" ]; then B_DIR="$dir"; B_DIR2="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
	done
	return 1
}
bench_run(){ # $1=name -> one number on stdout
	lookup "$1" || { echo 0; return; }
	if [ "$B_EXT" = TIME ]; then
		# PARSEC and the kernel build have no parseable metric: the metric IS
		# wall seconds (direction lo). parsecmgmt is kept deliberately --
		# input extraction is part of the workload and is constant across arms.
		local t0 t1
		t0=$(date +%s.%N)
		( cd "$B_DIR" 2>/dev/null || cd /root; eval "$B_CMD" ) >/dev/null 2>&1
		t1=$(date +%s.%N)
		python3 -c "print(f'{$t1-$t0:.4f}')"
	else
		( cd "$B_DIR" 2>/dev/null || cd /root; eval "$B_CMD" 2>&1 ) | eval "$B_EXT" | tail -1
	fi
}

setarm(){
	# Arms match campaign/fullstack.sh arm() EXACTLY, so results are
	# comparable to every recorded campaign number. Setting only
	# spin_mode + universal_eligible is NOT the campaign's IVH: it leaves
	# head bypass, eviction, lookahead and nosteal off, and head bypass
	# alone is worth +5.73% vs stock PV pooled.
	if [ "$1" = pv ]; then
		echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
		echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
		[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: pv arm did not take"; exit 1; }
	else
		echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
		echo 2 > $S/ivh_pv_preempt_src            # AS tier-2 source
		echo 2 > $S/ivh_preempt_event_source      # GATE 2 LIVE (TSC path)
		echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
		echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs
		echo 0 > $S/ivh_head_bypass_hold
		echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
		echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
		[ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: ivh arm did not take"; exit 1; }
		[ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "FATAL: gate2 not live"; exit 1; }
		echo "$1" > $S/$KNOB
		[ "$(cat $S/$KNOB)" = "$1" ] || { echo "FATAL: $KNOB rejected $1"; exit 1; }
	fi
	arm_cs                       # AFTER spin_mode -- see header
	[ "$(cat $S/ivh_cs_owner_enable)" = 1 ] || { echo "FATAL: CS stamping disarmed"; exit 1; }
}

run(){ # $1=bench $2=arm
	setarm "$2"; sleep 1
	local ma mb ha hb ia ib g1a g1b g2a g2b val
	ma=$(M); ha=$(HIST); ia=$(IPI)
	g1a=$(G ivh_steal_imminent_capacity_reject); g2a=$(G ivh_steal_imminent_time_left_reject)
	local t0 t1
	t0=$(date +%s.%N)
	val=$(bench_run "$1")
	# registry direction: lo = wall seconds. Invert so higher is always better,
	# and report both conventions in the report.
	if [ "${B_DIR2:-hi}" = lo ] && [ -n "$val" ] && [ "$val" != 0 ]; then
		val=$(python3 -c "print(1000.0/$val)")
	fi
	t1=$(date +%s.%N)
	mb=$(M); hb=$(HIST); ib=$(IPI)
	g1b=$(G ivh_steal_imminent_capacity_reject); g2b=$(G ivh_steal_imminent_time_left_reject)
	python3 - "$1" "$2" "${val:-0}" "$((mb-ma))" "$((ib-ia))" "$((g1b-g1a))" "$((g2b-g2a))" "$ha" "$hb" \
	  "$(python3 -c "print($t1-$t0)")" >> "$RAW" <<'PY'
import re,sys
bench,arm,val,migs,ipi,g1,g2,ha,hb,dur=sys.argv[1:11]
d=lambda s:{int(a):int(b) for a,b in re.findall(r'\((\d+),\s*(\d+)\)',s)}
A,B=d(ha),d(hb); D={k:max(B.get(k,0)-A.get(k,0),0) for k in set(A)|set(B)}
tot=sum(D.values()); lng=sum(v for k,v in D.items() if k>=20)
print(f"{bench}\t{arm}\t{val}\t{migs}\t{ipi}\t{g1}\t{g2}\t{lng}\t{tot}\t{dur}")
PY
	tail -1 "$RAW"
}

ARMS=(pv $VALUES); N=${#ARMS[@]}
: > "$RAW"
echo "KNOB=$KNOB  VALUES=$VALUES"
echo "BENCHES=$BENCHES  REPS=$REPS  arms=$N"
echo
for b in $BENCHES; do
	echo "########## $b ##########"
	# WARMUP, DISCARDED. Without it the first rep of a workload carries the
	# previous workload's residue; with arm order rotating, that biases
	# whichever arm happens to run first. psearchy_ab.sh has always done this.
	setarm "${ARMS[0]}"; bench_run "$b" >/dev/null 2>&1
	for i in $(seq 0 $((REPS-1))); do
		for j in $(seq 0 $((N-1))); do run "$b" "${ARMS[$(( (i+j) % N ))]}"; done
	done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 2 > $S/ivh_preempt_event_source; arm_cs
echo 4000000 > $S/ivh_time_left_threshold_ns; echo 8 > $S/ivh_max_concurrent
echo; echo "=== REPORT ==="
KNOB="$KNOB" python3 /root/ivh_tools/ivh_probe_report.py "$RAW"
echo IVH-PROBE-DONE
