#!/bin/bash
# Does a SHORTER publish interval cut false staleness, and does that help?
# Apparent staleness = (time into current publish window) + (real preemption).
# publish every 4096 iters ~= 48us vs a 100us threshold = only ~2x headroom, so a
# briefly-preempted-but-live waiter reads as stale. The project's own rule is
# threshold >= 10x publish interval; mask=511 gives ~16x.
# Arms: t12 + head bypass (OPEN gate) at each publish mask, vs stock PV.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-8}; T=${T:-$(nproc)}; MASKS=${MASKS:-"4095 1023 255"}
OUT=$D/pubsweep_$(date +%m%d-%H%M%S).csv
echo "blk,wl,arm,mask,metric,fired,obs_samples,obs_stale" > $OUT
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py \
        ivh_head_bypass_fired ivh_head_obs_samples ivh_head_obs_stale 2>/dev/null|awk '{printf "%s ",$3}'; }
zero(){ for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
        ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
        ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
        echo 3 > $S/ivh_head_bypass_runs; echo 220000 > $S/ivh_head_bypass_hold; }
setarm(){  # $1 arm  $2 mask
  zero
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
      [ "$(cat $S/ivh_head_bypass_enable)" = 0 ] || { echo "PV LEAK"; exit 1; }; return 0; fi
  $D/arm.sh nt1_only >/dev/null || exit 1
  zero
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo "$2" > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
  echo 1 > $S/ivh_head_bypass_enable
  echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
  [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$2" ] || { echo "MASK FAIL"; exit 1; }
}
run_wl(){ case $1 in
  hackbench) timeout -k 5 300 hackbench -T -g4 -f8 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
  dbench)    (cd /root/dbench_test; timeout -k 5 90 dbench -F -t 15 $T -D /root/dbench_test 2>&1|grep -oP 'Throughput\s+\K[0-9.]+') ;;
 esac; }
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench dentry dbench; do
    SPECS="pv:4095"
    for m in $MASKS; do SPECS="$SPECS byp:$m"; done
    for spec in $(echo $SPECS | tr ' ' '\n' | shuf); do
      arm=${spec%%:*}; mask=${spec##*:}
      setarm "$arm" "$mask"; read -r f0 s0 t0 <<< "$(ctr)"
      M=$(run_wl $wl); read -r f1 s1 t1 <<< "$(ctr)"
      echo "$b,$wl,$arm,$mask,${M:-0},$((f1-f0)),$((s1-s0)),$((t1-t0))" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; zero; echo 4095 > $S/ivh_pv_beat_publish_mask
echo "DONE -> $OUT"
