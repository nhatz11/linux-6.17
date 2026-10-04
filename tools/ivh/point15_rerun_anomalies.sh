#!/bin/bash
# Point 15 follow-up: give extra reps to the workloads whose first 3 reps were
# unreliable, and fix the attribution flaw that made some of them unreliable.
#
# ANOMALY CRITERIA (any one qualifies):
#   UNSTABLE   max/min contended rate across reps > 3x. parsec_swaptions read
#              685, 40, 62 /s -- a 17x swing on identical runs.
#   NEAR-IDLE  median rate < 2x the idle background. At that level the
#              measurement is mostly other things on the box.
#   SHORT      median run < 1s, so startup dominates (fsmark, sysbench_mutex).
#
# ATTRIBUTION FIX: perf stat -a is SYSTEM-WIDE, so it counts kernel spinlock
# contention from vcap_probe, MY_ivh_atc and any transient -- not just the
# workload. Irrelevant at 567,000/s (dentry), decisive at 40/s (swaptions).
# Each run is now preceded by a 3s idle background sample, and the report
# carries both raw and background-subtracted rates.
#
# This does NOT rescue the userspace-sync workloads (PARSEC, NHextend):
# lock:contention_begin cannot see pthread/futex synchronisation at all, so a
# low reading there is the wrong instrument, not low contention. Extra reps
# tighten the number; they do not make it mean the right thing.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
CSV="${1:?usage: $0 <point15 csv>}"
EXTRA="${EXTRA:-5}"
OUT="${CSV%.csv}_rerun.csv"
R="python3 /root/ivh_tools/read_ivh_counters.py"
HSUM(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'sum=[0-9]+' | cut -d= -f2 || echo 0; }

ANOM=$(python3 - "$CSV" <<'PY'
import csv,statistics as st,collections,sys
rows=collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    try: s=float(r["seconds"]); c=float(r["contended"])
    except Exception: continue
    if s>0: rows[r["workload"]].append((s,c/s))
IDLE=64.0
out=[]
for n,v in rows.items():
    rates=[x[1] for x in v]; secs=[x[0] for x in v]
    why=[]
    if min(rates)>0 and max(rates)/min(rates)>3: why.append("UNSTABLE")
    if st.median(rates)<2*IDLE: why.append("NEAR-IDLE")
    if st.median(secs)<1: why.append("SHORT")
    if why: out.append(f"{n}:{'+'.join(why)}")
print(" ".join(out))
PY
)
[ -z "$ANOM" ] && { echo "no anomalies -- nothing to re-run"; exit 0; }
echo "anomalies needing more reps (+$EXTRA each):"
for a in $ANOM; do printf "   %-24s %s\n" "${a%%:*}" "${a#*:}"; done

echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: PV arm did not take"; exit 1; }
for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do echo 1 > $S/$k; done
[ "$(cat $S/ivh_cs_owner_enable)" = 1 ] || { echo "FATAL: CS stamping disarmed"; exit 1; }
echo 0 > $S/ivh_tks_sampler_ns

echo "workload,rep,seconds,contended,holds,bg_per_s" > "$OUT"
NHX="nhextend_full|hi|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16 2>&1|x|+64.0"
for a in $ANOM; do
	NAME="${a%%:*}"
	CMD=""
	for w in "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}" "$NHX"; do
		IFS='|' read -r n d c rest <<< "$w"
		[ "$n" = "$NAME" ] && CMD="$c" && break
	done
	[ -z "$CMD" ] && { echo "  !! no command for $NAME"; continue; }
	for r in $(seq 1 "$EXTRA"); do
		sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
		bg=$(perf stat -a -e lock:contention_begin -x, -- sleep 3 2>&1 \
		      | awk -F, '/contention_begin/{print $1/3}')
		h0=$(HSUM); t0=$(date +%s.%N)
		cnt=$(perf stat -a -e lock:contention_begin -x, -- \
		       bash -c "$CMD >/dev/null 2>&1" 2>&1 | awk -F, '/contention_begin/{print $1}')
		t1=$(date +%s.%N); h1=$(HSUM)
		python3 - "$NAME" "$r" "$t0" "$t1" "${cnt:-0}" "$h0" "$h1" "${bg:-0}" >> "$OUT" <<'PY'
import sys
n,r,t0,t1,c,h0,h1,bg=sys.argv[1:9]
d=float(t1)-float(t0)
print(f"{n},{r},{d:.3f},{c},{int(h1)-int(h0)},{float(bg):.1f}")
PY
		tail -1 "$OUT" | awk -F, -v n="$NAME" '{printf "  %-22s r%s %7.2fs raw=%9.0f/s bg=%7.0f/s net=%9.0f/s holds=%8.0f/s\n", n, $2, $3, $4/$3, $6, ($4/$3)-$6, $5/$3}'
	done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible; echo 2 > $S/ivh_preempt_event_source
echo "WROTE $OUT"; echo POINT15-RERUN-DONE
