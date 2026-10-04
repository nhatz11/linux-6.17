#!/bin/bash
# p11_grid.sh -- publish mask x staleness threshold, on hackbench only.
#
# WHY A GRID: the two knobs interact. The publish mask sets how often the beat is
# written (4095 ~ every 4096 iters ~ 50us; 255 ~ every 256 ~ 3us). A staleness
# threshold only distinguishes "preempted" from "simply hasn't published yet" if
# it is well above the publish interval -- so mask 4095 cannot support thresholds
# below ~150us, and the prediction is that 255's advantage GROWS as the threshold
# falls. That is the hypothesis this grid tests.
#
# WHY HACKBENCH ONLY: it is the only workload with spin dynamic range (82s vs
# 0.1-6.8s elsewhere), so it is the only one that can resolve a threshold effect.
#
# REGIME CLASSIFICATION: hackbench is bimodal in halt volume (low ~4-7s vs high
# 20-50s) in BOTH arms. A pair whose two arms land in different regimes compares
# different operating points, so those pairs are reported separately, never pooled.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -w 5400 9 || { echo "FATAL: lock"; exit 1; }
REPS="${REPS:-6}"
ARMS="${ARMS:-mig 4095:400 255:400 255:200 255:100}"
OUT=/root/ivh_logs/p11grid_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_beat_tier2_checked ivh_cs_head_bailed ivh_evict_marked"
printf "arm\tmask\tthr_us\trep\ttime\tspin_ns\thalt_ns\tt1\tt2f\tt2c\tcsb\tev\tcap\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ bash $T/pvbase.sh >/dev/null 2>&1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  if [ "$1" = mig ]; then
    bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
    for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
    [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1
  else
    M=${1%%:*}; U=${1##*:}
    IVH_MASK=$M bash $T/p11_arm.sh $U >/dev/null 2>&1 || return 1
    [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$M" ] || { echo "  MASKFAIL"; return 1; }
    CYC=$((U*2200))
    [ "$(cat $S/ivh_pv_beat_threshold)" = "$CYC" ] || { echo "  THRFAIL want=$CYC got=$(cat $S/ivh_pv_beat_threshold)"; return 1; }
  fi; sleep 1; }
echo "### p11_grid arms=[$ARMS] reps=$REPS hackbench-only -> $OUT"
for rep in $(seq 1 $REPS); do
  set -- $ARMS; K=$#; ORD=""
  for i in $(seq 0 $((K-1))); do eval "ORD=\"\$ORD \${$(( (i + rep - 1) % K + 1 ))}\""; done
  echo "  -- rep$rep order:$ORD"
  for a in $ORD; do
    arm "$a" || { echo "  ARMFAIL $a"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    CM=$(capm); b=($(snap))
    t0=$(date +%s%N); timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1; t1=$(date +%s%N)
    f=($(snap)); tm=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
    sp=$(( (${f[0]}-${b[0]}) - (${f[1]}-${b[1]}) )); hl=$(( ${f[1]}-${b[1]} ))
    MK=mig; TU=0; [ "$a" != mig ] && { MK=${a%%:*}; TU=${a##*:}; }
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$MK" "$TU" "$rep" "$tm" "$sp" "$hl" \
      "$(( ${f[5]}-${b[5]} ))" "$(( ${f[6]}-${b[6]} ))" "$(( ${f[7]}-${b[7]} ))" "$(( ${f[8]}-${b[8]} ))" "$(( ${f[9]}-${b[9]} ))" "$CM" >> "$OUT"
    echo "    $a time=${tm}s spin=$(python3 -c "print(f'{$sp/1e9:.1f}')")s halt=$(python3 -c "print(f'{$hl/1e9:.1f}')")s t2f=$(( ${f[6]}-${b[6]} )) cap=$CM"
  done
done
python3 $T/p11_grid_report.py "$OUT" || true
echo "P11GRID_DONE $OUT"
