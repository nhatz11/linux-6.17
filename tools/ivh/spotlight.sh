#!/bin/bash
# spotlight.sh <ebizzy_mmap|nhextend_fin> [reps]
#
# Per rep, interleaved pv then mig. For the mig arm, migcost.bt runs alongside
# and decomposes every IVH self-migration of the workload's own threads into
#   COST  = enter bpf_sched_pre_lock_migrate -> on the target rq
#   DELAY = on the target rq -> actually running there
# The kernel qspinlock spin counters bracket the run in BOTH arms, which is
# what "saved" is computed from. ivh_migrations_done is read as an independent
# cross-check on migcost's own migration count.
#
# bpftrace adds two probes to a path taken ~48k times/s. Measured cost: ebizzy
# 2031-2084 records/s instrumented vs 1900-1979 uninstrumented in the same arm,
# i.e. inside run-to-run noise. The headline % comes from the UNINSTRUMENTED
# spotlight_confirm.sh run regardless; this script is for the mechanism terms.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: another run holds $LOCK"; exit 1; }
W="$1"; REPS="${2:-3}"; THRESH="${THRESH:-2500000}"
STAMP=$(date +%m%d-%H%M%S)
OUT=/root/ivh_logs/spot_${W}_${STAMP}.tsv
BTDIR=/root/ivh_logs/spot_${W}_${STAMP}_bt; mkdir -p "$BTDIR"

case "$W" in
  ebizzy_mmap)
    RUNSEC=15; COMM=ebizzy
    CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
    XMETRIC="grep -oP '^\K[0-9]+(?= records/s)'" ;;
  nhextend_fin)
    RUNSEC=8; COMM=NHextend-fin
    # MID-SPIN arm (2026-10-02, user's choice). ITERS=10000 is the efficient
    # operating point of the three measured at DURATION=5: 781 syscalls of
    # which 53.5% landed a CPU move, for 0.384 s of total syscall time.
    # ITERS=1000 burns 7.2M polls for the same 400-odd moves (25.2% landed,
    # 99.0% of danger hits suppressed by the 1 ms cooldown); ITERS=100000
    # lands 73.4% but doubles total syscall time to 0.881 s.
    # -l only enables end-of-run printing (show_last), no behaviour change; it
    # is what exposes the mid-spin counters that cross-check bpftrace.
    # The PV arm runs the SAME binary and the SAME env: verified 2026-10-02 to
    # poll 740,660 times with the DANGER bit set 0 times and 0 syscalls, so it
    # pays the identical polling cost and the arms differ only in sysctls.
    MIDSPIN=${MIDSPIN:-10000}
    CMD="IVH_AFL_DISABLE=1 NHEXTEND_MIDSPIN_ITERS=$MIDSPIN NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 /root/linux-6.17/NHextend-fin -l -n 16"
    XMETRIC="grep -oP 'Ran for \K[0-9]+'" ;;
  *) echo "unknown workload $W"; exit 1 ;;
esac

CTRS="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_slowpath_halt_events"
snap(){ python3 $T/read_ivh_counters.py $CTRS 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);print $1"="$2}'
        printf "ivh_migrations_done=%s\n" "$(python3 $T/migcount.py 2>/dev/null || echo 0)"; }
gv(){ echo "$1" | grep -oP "^$2=\K.*"; }

printf "workload\tarm\trep\tmetric\twait_ns\thalt_ns\tspin_ns\twait_ev\thalt_ev\tmigdone\tuwait_s\n" > "$OUT"
echo "### $W  reps=$REPS  thresh=$THRESH  comm=$COMM  -> $OUT"

# WARM-UP, discarded. ebizzy_mmap rose monotonically 1927 -> 2553 -> 2591 in its
# PV arm on 2026-10-02 (CV 15.7%) purely because rep1 ran cold, which alone
# flipped the verdict from -4.7% to +2.6%. One throwaway run first.
if [ "${WARMUP:-0}" != 0 ]; then
  bash $T/p7v2_arm.sh $THRESH >/dev/null 2>&1; sleep 1
  [ "${DROPC:-0}" = 1 ] && { sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1; }
  echo "  (warm-up, discarded)"
  timeout 300 bash -c "$CMD" >/dev/null 2>&1 9>&-
fi

for rep in $(seq 1 $REPS); do
  for arm in pv mig; do
    if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || { echo "ARMFAIL pv"; exit 1; }
    else bash $T/p7v2_arm.sh $THRESH >/dev/null 2>&1 || { echo "ARMFAIL mig"; exit 1; }; fi
    sleep 1
    if [ "${DROPC:-0}" = 1 ]; then sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1; fi
    BT=""
    BTSCRIPT="${BT_SCRIPT:-$T/migcost.bt}"
    want_bt=0
    [ "${NOBT:-0}" = 0 ] && { [ "$arm" = mig ] && want_bt=1; }
    [ "${NOBT:-0}" = 0 ] && [ "${BTPV:-0}" = 1 ] && want_bt=1
    if [ "$want_bt" = 1 ]; then
      BTF="$BTDIR/rep${rep}_${arm}.txt"
      timeout $((RUNSEC+30)) bpftrace "$BTSCRIPT" $((RUNSEC+3)) > "$BTF" 2>&1 9>&- &
      BT=$!
      sleep 3   # kprobe attach
    fi
    pre=$(snap)
    out=$( timeout 300 bash -c "$CMD" 2>&1 9>&- )
    post=$(snap)
    [ -n "$BT" ] && wait $BT 2>/dev/null
    printf "%s" "$out" > "$BTDIR/rep${rep}_${arm}.out"
    val=$(echo "$out" | eval $XMETRIC | head -1)
    uwait=$(echo "$out" | grep -oP 'Total wait time: \K[0-9.]+' | head -1)
    w=$(( $(gv "$post" ivh_slowpath_wait_ns)  - $(gv "$pre" ivh_slowpath_wait_ns) ))
    h=$(( $(gv "$post" ivh_slowpath_halt_ns)  - $(gv "$pre" ivh_slowpath_halt_ns) ))
    we=$(( $(gv "$post" ivh_slowpath_wait_events) - $(gv "$pre" ivh_slowpath_wait_events) ))
    he=$(( $(gv "$post" ivh_slowpath_halt_events) - $(gv "$pre" ivh_slowpath_halt_events) ))
    md=$(( $(gv "$post" ivh_migrations_done) - $(gv "$pre" ivh_migrations_done) ))
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "$W" "$arm" "$rep" "${val:-NA}" "$w" "$h" "$((w-h))" "$we" "$he" "$md" "${uwait:-NA}" >> "$OUT"
    echo "  rep$rep $arm metric=${val:-NA} spin=$(python3 -c "print(f'{($w-$h)/1e9:.3f}s')") migdone=$md uwait=${uwait:-NA}"
  done
done
echo "DONE $OUT"
