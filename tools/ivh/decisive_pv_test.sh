#!/bin/bash
# Decisive test (per Opus round-5 investigation): confirm the 2.93% tier-2
# fire rate is real preemption, not an artifact, by capturing tier1/tier2
# counters AND independent LOC (timer-tick) shortfall from the SAME run.
set -u
SYSDIR=/proc/sys/kernel
COUNTER_PY=/root/ivh_tools/read_ivh_counters.py
WORKLOAD="hackbench -T -g 1 -f 8 -l 400000"

echo "=== Boot-time facts (must confirm PV mode) ==="
for f in ivh_pv_tas ivh_pv_allow ivh_pv_unhalt_avail; do
    printf "  %-24s %s\n" "$f" "$(cat "$SYSDIR/$f" 2>/dev/null || echo MISSING)"
done
if [ "$(cat "$SYSDIR/ivh_pv_tas" 2>/dev/null)" = "1" ]; then
    echo "ABORT: still in TAS mode (ivh_pv_tas=1) -- this test needs a PV boot." >&2
    exit 1
fi
echo

echo "=== Configuring IVH_PV (spin_mode 2) ==="
/root/spin_mode 2
echo

loc_sum() {
    grep '^LOC:' /proc/interrupts | tr -s ' ' | cut -d: -f2 | \
        awk '{s=0; for(i=1;i<=NF;i++) s+=$(i); print s}'
}

echo "=== Snapshot BEFORE ==="
BEFORE_LOC=$(loc_sum)
echo "LOC sum (before): $BEFORE_LOC"
python3 "$COUNTER_PY" > /tmp/before_counters.txt
cat /tmp/before_counters.txt
echo

echo "=== Running workload: $WORKLOAD ==="
T0=$(date +%s.%N)
OUT=$($WORKLOAD 2>&1)
T1=$(date +%s.%N)
HB_TIME=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
WALL=$(echo "$T1 - $T0" | bc)
echo "hackbench_time=${HB_TIME}s  wall=${WALL}s"
echo

echo "=== Snapshot AFTER ==="
AFTER_LOC=$(loc_sum)
echo "LOC sum (after): $AFTER_LOC"
python3 "$COUNTER_PY" > /tmp/after_counters.txt
cat /tmp/after_counters.txt
echo

echo "=== Deltas ==="
NCPU=$(nproc)
LOC_DELTA=$((AFTER_LOC - BEFORE_LOC))
EXPECTED_LOC=$(awk -v w="$WALL" -v n="$NCPU" 'BEGIN{printf "%d", w*1000*n}')  # HZ=1000
LOC_PCT=$(awk -v got="$LOC_DELTA" -v exp="$EXPECTED_LOC" 'BEGIN{if(exp>0) printf "%.1f", 100.0*got/exp; else print "n/a"}')
echo "LOC ticks delivered   : $LOC_DELTA"
echo "LOC ticks expected    : $EXPECTED_LOC  (HZ=1000 x ${WALL}s x ${NCPU} cpus)"
echo "LOC delivery rate     : ${LOC_PCT}% of expected  (shortfall = $((100 - ${LOC_PCT%.*}))%-ish)"
echo

paste /tmp/before_counters.txt /tmp/after_counters.txt | awk '{
    name=$1; before=$3; after=$6;
    if (name=="") next;
    delta=after-before;
    printf "%-32s before=%-14s after=%-14s delta=%s\n", name, before, after, delta;
}'
echo

TIER2_CHECKED_BEFORE=$(awk '$1=="ivh_beat_tier2_checked"{print $3}' /tmp/before_counters.txt)
TIER2_CHECKED_AFTER=$(awk '$1=="ivh_beat_tier2_checked"{print $3}' /tmp/after_counters.txt)
TIER2_FIRED_BEFORE=$(awk '$1=="ivh_beat_tier2_fired"{print $3}' /tmp/before_counters.txt)
TIER2_FIRED_AFTER=$(awk '$1=="ivh_beat_tier2_fired"{print $3}' /tmp/after_counters.txt)
TIER1_FIRED_BEFORE=$(awk '$1=="ivh_beat_tier1_fired"{print $3}' /tmp/before_counters.txt)
TIER1_FIRED_AFTER=$(awk '$1=="ivh_beat_tier1_fired"{print $3}' /tmp/after_counters.txt)

CHECKED_D=$((TIER2_CHECKED_AFTER - TIER2_CHECKED_BEFORE))
FIRED_D=$((TIER2_FIRED_AFTER - TIER2_FIRED_BEFORE))
TIER1_D=$((TIER1_FIRED_AFTER - TIER1_FIRED_BEFORE))

echo "=== Verdict inputs ==="
echo "tier1_fired delta        : $TIER1_D  (nonzero required -- confirms tier1 was actually enabled)"
echo "tier2_checked delta      : $CHECKED_D"
echo "tier2_fired delta        : $FIRED_D"
if [ "$CHECKED_D" -gt 0 ]; then
    FIRE_PCT=$(awk -v f="$FIRED_D" -v c="$CHECKED_D" 'BEGIN{printf "%.3f", 100.0*f/c}')
    echo "tier2 fire rate          : ${FIRE_PCT}%"
fi
echo
echo "Read this together: if LOC delivery rate is well under 100% (e.g. ~88%,"
echo "matching the ~12% shortfall found earlier under 16-thread load) AND"
echo "tier1_fired is nonzero AND tier2 fire rate is elevated (~2-3%), that is"
echo "airtight: real host preemption, independently confirmed two ways in the"
echo "same run. If LOC stays near 100% while tier2 still fires elevated, the"
echo "fire rate is likely an artifact -- check ivh_pv_tier1_enable was really 1."
