#!/bin/bash
# Generic IVH gate sweep. Point 7: KNOB=ivh_time_left_threshold_ns.
# Point 8: KNOB=ivh_max_concurrent VALUES="1 2 4 8 16 32".
#
# Gate 2 (fair.c:13832) returns TRUE TO REJECT when time_left > threshold,
# so a HIGHER threshold rejects less and migrates MORE. Both the migration
# count and the Gate 2 reject counter must therefore move monotonically
# across the sweep; if they are flat the knob is inert and the throughput
# column means nothing (cf. the head-bypass shut-gate case).
#
# The PMU is not virtualised on this TDX guest -- cache-misses and
# LLC-load-misses read <not supported> -- so "coherence" here is IPI TRAFFIC
# (RES+CAL+TLB per 1000 files), not a miss rate. Labelled accordingly.
set -u
S=/proc/sys/kernel
KNOB="${KNOB:-ivh_time_left_threshold_ns}"
VALUES="${VALUES:-500000 1000000 2000000 4000000 8000000 16000000}"
REPS="${REPS:-6}"
RAW="${RAW:-/tmp/gate_sweep.raw}"
R="python3 /root/ivh_tools/read_ivh_counters.py"

M(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
G2(){ $R ivh_steal_imminent_time_left_reject 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
HIST(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' || echo "[]"; }
IPI(){ grep -E "^[[:space:]]*(RES|CAL|TLB):" /proc/interrupts \
       | awk '{for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i} END{print s+0}'; }

# CS tracking is gated independently of ivh_adaptive_mode, so it works
# identically in the PV arm.
for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do
	echo 1 > $S/$k 2>/dev/null
done

setarm(){
	if [ "$1" = pv ]; then
		echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
		[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: pv arm did not take"; exit 1; }
		echo 4000000 > $S/ivh_time_left_threshold_ns
	else
		/root/spin_mode 2 >/dev/null 2>&1
		[ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: ivh arm did not take"; exit 1; }
		echo 1 > $S/ivh_universal_eligible
		echo "$1" > $S/$KNOB
		[ "$(cat $S/$KNOB)" = "$1" ] || { echo "FATAL: $KNOB rejected $1"; exit 1; }
	fi
	echo 2 > $S/ivh_preempt_event_source
}

run(){
	setarm "$1"
	rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
	sleep 1
	local ma mb ga gb ha hb ia ib files
	ma=$(M); ga=$(G2); ha=$(HIST); ia=$(IPI)
	files=$(fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1 2>/dev/null \
	        | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1)
	mb=$(M); gb=$(G2); hb=$(HIST); ib=$(IPI)
	python3 - "$1" "${files:-0}" "$((mb-ma))" "$((gb-ga))" "$((ib-ia))" "$ha" "$hb" >> "$RAW" <<'PY'
import re,sys
arm,files,migs,g2,ipi,ha,hb=sys.argv[1:8]
def d(s): return {int(a):int(b) for a,b in re.findall(r'\((\d+),\s*(\d+)\)',s)}
a,b=d(ha),d(hb)
dd={k:max(b.get(k,0)-a.get(k,0),0) for k in set(a)|set(b)}
tot=sum(dd.values()); lng=sum(v for k,v in dd.items() if k>=20)
share=(100.0*lng/tot) if tot else 0.0
f=float(files)
print(f"{arm}\t{files}\t{migs}\t{g2}\t{share:.4f}\t{lng}\t{tot}\t{ipi}")
PY
	tail -1 "$RAW"
}

ARMS=(pv $VALUES); N=${#ARMS[@]}
: > "$RAW"
echo "sweeping $KNOB over: $VALUES   (+pv arm)   reps=$REPS  arms=$N"
printf "%-10s %10s %6s %8s %8s %7s %8s %10s\n" arm files/s migs g2rej long% long tot ipi
for i in $(seq 0 $((REPS-1))); do
	echo "--- rep $((i+1)) ---"
	for j in $(seq 0 $((N-1))); do run "${ARMS[$(( (i+j) % N ))]}"; done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 4000000 > $S/ivh_time_left_threshold_ns; echo 2 > $S/ivh_preempt_event_source
echo "=== SUMMARY ==="
KNOB="$KNOB" python3 /root/ivh_tools/gate_report.py "$RAW"
echo GATE-SWEEP-DONE
