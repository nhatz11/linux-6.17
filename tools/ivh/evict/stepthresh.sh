#!/bin/bash
# Phase B: is lock skipping merely UNDER-TRIGGERED? Sweep the detector
# threshold with the two G-LOCK-38 fixes on. Lower threshold -> more evictions
# fire, but more false positives (27% already come back in <1us at 220000).
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; B=/root/linux-6.17
BLOCKS=${BLOCKS:-8}; T=${T:-$(nproc)}; HOP=${HOP:-2}; HG=${HG:-4}; HL=${HL:-100000}; DUR=${DUR:-10}
WL=${WL:-hackbench}; THRS=${THRS:-"220000 110000 55000 22000"}
OUT=$D/thr_${WL}_$(date +%m%d-%H%M%S).csv
echo "blk,wl,thr,arm,metric,marked,la_ref,steal_ok" > $OUT
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py \
        ivh_evict_marked ivh_evict_lookahead_refused ivh_evict_steal_ok 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){ # $1 arm  $2 threshold
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
           ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none \
           ivh_pv_evict_lookahead ivh_pv_camp_probe; do echo 0 > $S/$k 2>/dev/null; done
  echo 2 > $S/ivh_pv_preempt_src; echo "$2" > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo $HOP > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  [ "$1" = combo ] && { echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
                        echo 1 > $S/ivh_pv_requeue_nosteal; }
  [ "$(cat $S/ivh_pv_beat_threshold)" = "$2" ] || { echo "THR FAIL"; exit 1; }
}
run_wl(){ case $WL in
    hackbench) timeout -k 5 600 hackbench -T -g$HG -f8 -l$HL 2>&1|awk '/^Time:/{print $2}' ;;
    dbench)    cd /root/dbench_test 2>/dev/null||cd /root
               timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}' ;;
  esac; }
for b in $(seq 1 $BLOCKS); do
  for thr in $THRS; do
    for a in $(printf "base\ncombo\n"|shuf); do
      setarm "$a" "$thr"; read -r m0 l0 s0 <<< "$(ctr)"
      M=$(run_wl); read -r m1 l1 s1 <<< "$(ctr)"
      echo "$b,$WL,$thr,$a,${M:-0},$((m1-m0)),$((l1-l0)),$((s1-s0))" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_pv_evict_enable; echo 0 > $S/ivh_pv_evict_lookahead
echo 0 > $S/ivh_pv_requeue_nosteal; echo 220000 > $S/ivh_pv_beat_threshold
echo "DONE -> $OUT"
