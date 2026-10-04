#!/bin/bash
# powerup.sh -- resolve the two underpowered workloads with reps.
#
# cleanspin n=5 left both unresolved rather than failed:
#   ebizzy spin +6.71% CI [-8.61,+22.03]  (point estimate POSITIVE, CI too wide)
#   vips   perf -10.89%                   (read -0.58% and +0.85% in other sittings)
# Their spin bases are 1.58s and 0.01s, so percentage CIs are wide by construction.
# The fix for an underpowered measurement is n, not a different metric.
#
# Same arms as cleanspin: stock PV vs AS @50us, mask 255, migration OFF, one
# shared threshold on beat/evict/noise. Inputs unchanged.
# Metric: node + counted-head iterations (both loops, no stolen time).
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${REPS:-25}"
OUT=/root/ivh_logs/powerup_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1; fi; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 3600 9 || { echo FATAL; exit 1; }
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
printf "wl\tarm\trep\tval\tnode\thead\tentries\tt2f\tcap\n" > "$OUT"
echo "### powerup n=$REPS cap=$(capm) -> $OUT"
for wl in ebizzy vips; do
  echo "########## $wl ##########"
  case $wl in
   ebizzy) ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
   vips)   ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 ) ;;
  esac
  for rep in $(seq 1 $REPS); do
    case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
    for a in $O; do
      arm "$a" || { echo "  ARMFAIL"; continue; }
      sync; [ "$wl" = ebizzy ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capm); b=($(snap))
      case $wl in
       ebizzy) v=$( cd /root && timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 | grep -oP '^\K[0-9]+(?= records/s)' | head -1 ) ;;
       vips)   t0=$(date +%s%N); ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 ); t1=$(date +%s%N); v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')") ;;
      esac
      f=($(snap))
      printf "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\n" "$wl" "$a" "$rep" "${v:-NA}" \
        "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" \
        "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" "$CM" >> "$OUT"
      echo "  rep$rep $a $wl val=${v:-NA} cap=$CM"
    done
  done
  python3 $T/powerup_report.py "$OUT" "$wl" || true
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 $T/powerup_report.py "$OUT"
echo "POWERUP_DONE $OUT"
