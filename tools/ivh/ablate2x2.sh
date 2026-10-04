#!/bin/bash
# ablate2x2.sh -- the 2x2: userspace lock x kernel migration, off ONE baseline.
#
# The mistake this fixes: AFL is a USERSPACE env var, so the spotlight harness
# set it identically in both arms. Its "pv" arm therefore had the adaptive lock
# ON and only migration off -- a baseline that already contained half the
# mechanism. PV read 6029 instead of ~5300, and migration's share looked small
# because its headroom had already been taken.
#
#   none  AFL off (pure-spin lock) + stock PV kernel   <- nothing on, the real baseline
#   afl   AFL on, csmin + 1ms       + stock PV kernel   <- lock only
#   mig   AFL off (pure-spin lock)  + IVH migration     <- migration only
#   both  AFL on, csmin + 1ms       + IVH migration     <- do they compose?
#
# Same binary throughout (NHextend-csmin), NHEXTEND_CS_MIN=1 so Gate 2's
# time-left term is csmin in the migration arms. delta = 1 ms = the project's
# delta, matching ivh_time_left_threshold_ns = csmin + 1 ms.
set -u
R="${1:-5}"
T=/root/ivh_tools
B=/root/linux-6.17/NHextend-csmin
OUT=/root/ivh_logs/ablate2x2_$(date +%m%d-%H%M%S).tsv
exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "arm\trep\tops\twait_s\tmigdone\n" > "$OUT"
echo "### 2x2 ablation, base_slice=$(cat /sys/kernel/debug/sched/base_slice_ns), THRESH=1900000"
snapm() { python3 $T/read_ivh_counters.py ivh_migrations_done 2>/dev/null | awk -F= '{gsub(/ /,"",$2);print $2}'; }
for rep in $(seq 1 "$R"); do
  ORD=$(python3 -c "a='none afl mig both'.split(); k=($rep-1)%4; print(' '.join(a[k:]+a[:k]))")
  for a in $ORD; do
    case $a in
      none|afl) bash $T/pvbase.sh >/dev/null 2>&1 ;;
      mig|both) bash $T/p7v2_arm.sh 1900000 >/dev/null 2>&1 ;;
    esac
    case $a in
      none|mig) E="IVH_AFL_DISABLE=1" ;;
      afl|both) E="" ;;
    esac
    sleep 1
    m0=$(snapm)
    o=$(timeout 60 env $E NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 NHEXTEND_CS_MIN=1 "$B" -l -n 16 2>&1)
    m1=$(snapm)
    ops=$(printf '%s' "$o" | grep -oP 'Ran for \K[0-9]+')
    w=$(printf '%s' "$o" | grep -oP 'Total wait time: \K[0-9.]+')
    printf "%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "${ops:-NA}" "${w:-NA}" "$(( m1 - m0 ))" >> "$OUT"
    echo "  rep$rep $a ops=${ops:-NA} migdone=$(( m1 - m0 ))"
  done
done
bash $T/pvbase.sh >/dev/null 2>&1
echo "ABLATE2X2_DONE $OUT"
