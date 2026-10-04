#!/bin/bash
# lock3arm.sh -- ONE variable at a time: the lock design, nothing else.
#
# Everything held fixed: stock PV (no migration), default base_slice_ns,
# AFL ENABLED in all three, loop_spin 600000, 16 threads, no NHEXTEND_CS_MIN.
#
#   fin         heartbeat republished every 1024 iters INSIDE the CS;
#               waiter sleeps on heartbeat STALENESS (50 us).
#   csmin-ctl   csmin code path, but delta=100ms so the predicate NEVER fires.
#               Differs from `fin` ONLY by the missing in-CS republish, so
#               (ctl - fin) isolates the store+sfence tax.
#   csmin       delta=10us, the real predicate: sleep on elapsed CS > cmin+delta.
#               (csmin - ctl) is then the predicate's own effect.
#
# Rotated each rep so position cannot alias onto an arm.
set -u
R="${1:-5}"
OUT=/root/ivh_logs/lock3arm_$(date +%m%d-%H%M%S).tsv
exec 9>/var/lock/ivh_clean_check.lock
flock -w 1800 9 || { echo "FATAL: bench lock"; exit 1; }
bash /root/ivh_tools/pvbase.sh >/dev/null 2>&1
echo "### stock PV, no migration: tier2=$(cat /proc/sys/kernel/ivh_pv_tier2_enable) univ=$(cat /proc/sys/kernel/ivh_universal_eligible)"
echo "### base_slice_ns=$(cat /sys/kernel/debug/sched/base_slice_ns)  (untouched)"
printf "arm\trep\tops\twait_s\n" > "$OUT"
for rep in $(seq 1 "$R"); do
  case $((rep % 3)) in
    1) ORD="fin csmin-ctl csmin" ;;
    2) ORD="csmin-ctl csmin fin" ;;
    0) ORD="csmin fin csmin-ctl" ;;
  esac
  for a in $ORD; do
    case $a in
      fin)       B=/root/linux-6.17/NHextend-fin ;;
      csmin-ctl) B=/root/linux-6.17/NHextend-csmin-ctl ;;
      csmin)     B=/root/linux-6.17/NHextend-csmin ;;
    esac
    o=$(timeout 60 env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 "$B" -l -n 16 2>&1)
    ops=$(printf '%s' "$o" | grep -oP 'Ran for \K[0-9]+')
    w=$(printf '%s' "$o" | grep -oP 'Total wait time: \K[0-9.]+')
    printf "%s\t%s\t%s\t%s\n" "$a" "$rep" "${ops:-NA}" "${w:-NA}" >> "$OUT"
    echo "  rep$rep $a ops=${ops:-NA} wait=${w:-NA}"
  done
done
echo "LOCK3ARM_DONE $OUT"
