#!/bin/bash
# clean_check.sh -- does every suite12 workload give a CLEAN metric under
# MIGRATION ALONE vs PV? Per-arm CV decides whether #7/#8 can be scoped.
# Arms: pvbase.sh vs p7v2_arm.sh $THRESH -- both spin_mode 1, so the ONLY
# variable is ivh_universal_eligible (migration); no adaptive spinning.
#
# 2026-10-01: a lockfile, because two concurrent instances flip the SAME
# sysctl arms and silently interleave each other's measurements.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"
flock -n 9 || { echo "FATAL: another clean_check.sh holds $LOCK"; exit 1; }
source $T/suite12.sh
THRESH=${THRESH:-750000}
REPS=${REPS:-3}
OUT=${OUT:-/root/ivh_logs/clean_check_$(date +%Y%m%d_%H%M%S).tsv}
: > "$OUT"
echo "out=$OUT  arms: pv=pvbase.sh  mig=p7v2_arm.sh $THRESH  reps=$REPS"
for rep in $(seq 1 $REPS); do
  for i in $(shuf -i 0-$((${#SUITE12[@]}-1))); do
    IFS='|' read -r n d m c x <<< "${SUITE12[$i]}"
    [ "$c" = MEMTIER_CMD ] && c="$MEMTIER_CMD"
    d=$(eval echo "$d"); c=$(eval echo "\"$c\"")
    for arm in pv mig; do
      if [ "$arm" = pv ]; then bash $T/pvbase.sh           >/dev/null 2>&1
      else                    bash $T/p7v2_arm.sh $THRESH  >/dev/null 2>&1; fi
      sleep 1
      prep12 "$n" >/dev/null 2>&1 || { printf "%s\t%s\t%s\t%s\tPREPFAIL\n" "$n" "$m" "$arm" "$rep" >>"$OUT"; continue; }
      t0=$(date +%s%N)
      out=$( ( cd "$d" && timeout 900 bash -c "$c" ) 2>&1 )
      t1=$(date +%s%N)
      if [ "$x" = x ]; then val=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
      else val=$(echo "$out" | eval $x | head -1); fi
      printf "%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$arm" "$rep" "${val:-NA}" >> "$OUT"
      echo "[rep$rep] $n/$arm = ${val:-NA}"
    done
  done
  echo "=== rep $rep complete ==="
done
echo "DONE -> $OUT"
