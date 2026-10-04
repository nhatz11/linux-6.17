#!/bin/bash
# Why did ebizzy fall from +46% to +0.57%? Isolate tier2 x csmin x threshold.
# Baseline is pv+t1 (pvbase already sets tier1=1). Warmup discarded: ebizzy's
# first run in a fresh arm is unrepresentative.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
REPS="${1:-3}"
CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
X="grep -oP '^\K[0-9]+(?= records/s)'"
OUT=/root/ivh_logs/ebizzy_rescue_$(date +%m%d-%H%M%S).tsv
printf "arm\trep\trecs\tmigs\twait_ns\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$'; }
wns(){ python3 $T/read_ivh_counters.py ivh_slowpath_wait_ns 2>/dev/null | awk -F= '{gsub(/ /,"",$2);print $2}'; }

arm(){ case $1 in
  pv_t1)          bash $T/pvbase.sh >/dev/null 2>&1 ;;
  t1_lastcs_25)   bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1; echo 0 > $S/ivh_pv_tier2_enable; echo 0 > $S/ivh_cs_gate2_reference ;;
  t1_csmin_25)    bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1; echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
  t12_lastcs_25)  bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1; echo 1 > $S/ivh_pv_tier2_enable; echo 0 > $S/ivh_cs_gate2_reference ;;
  t12_csmin_25)   bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1; echo 1 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
  t12_lastcs_40)  bash $T/p7v2_arm.sh 4000000 >/dev/null 2>&1; echo 1 > $S/ivh_pv_tier2_enable; echo 0 > $S/ivh_cs_gate2_reference ;;
  t1_lastcs_40)   bash $T/p7v2_arm.sh 4000000 >/dev/null 2>&1; echo 0 > $S/ivh_pv_tier2_enable; echo 0 > $S/ivh_cs_gate2_reference ;;
  esac
  echo 1 > $S/ivh_pv_tier1_enable; sleep 1; }

ARMS="pv_t1 t1_lastcs_25 t1_csmin_25 t12_lastcs_25 t12_csmin_25 t12_lastcs_40 t1_lastcs_40"
for a in $ARMS; do
  arm $a
  ( cd /root && timeout 120 $CMD >/dev/null 2>&1 )   # warmup, discarded
  for rep in $(seq 1 "$REPS"); do
    m0=$(mig); w0=$(wns)
    v=$( cd /root && timeout 120 $CMD 2>&1 | eval "$X" | head -1 )
    m1=$(mig); w1=$(wns)
    printf "%s\t%d\t%s\t%d\t%d\n" "$a" "$rep" "${v:-NA}" "$((m1-m0))" "$((w1-w0))" >> "$OUT"
  done
  printf "  %-16s t1=%s t2=%s csmin=%s thr=%-7s  %s\n" "$a" \
    "$(cat $S/ivh_pv_tier1_enable)" "$(cat $S/ivh_pv_tier2_enable)" \
    "$(cat $S/ivh_cs_gate2_reference)" "$(cat $S/ivh_time_left_threshold_ns)" \
    "$(awk -F'\t' -v A="$a" '$1==A{s+=$3;n++;m+=$4} END{printf "recs=%.0f migs=%.0f n=%d", s/n, m/n, n}' "$OUT")"
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference
echo "EBIZZY_RESCUE_DONE $OUT"
