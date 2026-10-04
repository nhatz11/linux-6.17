#!/bin/bash
# vips_thrprobe.sh -- find a staleness threshold at which AS does not HURT vips.
#
# MECHANISM: AS can only add iterations via the re-arm penalty -- `threshold` is
# re-read inside the outer for(;;) (qspinlock_paravirt.h:1919/:3595), so a
# false-positive halt makes the waiter spin a FULL fresh budget. vips acquires at
# ~138 iters/entry, so one misfire costs 32768/138 = 237x normal spinning. With
# only ~6,400 entries a handful of misfires dominates the whole measurement.
#
# THEREFORE: on vips the goal is for AS to FIRE LESS, not more. A higher
# staleness threshold fires only on genuinely long waits. The earlier sweep shows
# hackbench (0.808) and memtier (0.399) keep most of their win at 800us, so this
# costs the high-spin workloads little.
#
# One shared threshold for all three TSC mechanisms, as required. vips only here
# because it is the blocker; the winner gets re-verified on all five.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${REPS:-8}"; THRS="${THRS:-50 200 800 1500}"
OUT=/root/ivh_logs/vipsthr_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh "$2" >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            CYC=$(python3 -c "print(int(round($2*2200)))")
            [ "$(cat $S/ivh_pv_beat_threshold)" = "$CYC" ] || return 1
            [ "$(cat $S/ivh_cs_noise_cycles)" = "$CYC" ] || return 1; fi
  sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 5400 9 || { echo FATAL; exit 1; }
printf "thr\tarm\trep\tsec\tnode\thead\tentries\tt2f\tcsb\tev\tcap\n" > "$OUT"
echo "### vips threshold probe thrs=[$THRS] reps=$REPS cap=$(capm) -> $OUT"
( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
for TH in $THRS; do
 echo "########## threshold=${TH}us ##########"
 for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
   arm "$a" "$TH" || { echo "  ARMFAIL"; continue; }
   sync; sleep 1; CM=$(capm); b=($(snap)); t0=$(date +%s%N)
   ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
   t1=$(date +%s%N); f=($(snap))
   printf "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n" "$TH" "$a" "$rep" \
     "$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")" \
     "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" \
     "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" "$(( ${f[6]}-${b[6]} ))" "$(( ${f[7]}-${b[7]} ))" "$CM" >> "$OUT"
   echo "  ${TH}us rep$rep $a t2f=$(( ${f[5]}-${b[5]} )) csb=$(( ${f[6]}-${b[6]} )) ev=$(( ${f[7]}-${b[7]} ))"
  done
 done
 python3 $T/vipsthr_report.py "$OUT" "$TH" || true
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 $T/vipsthr_report.py "$OUT"
echo "VIPSTHR_DONE $OUT"
