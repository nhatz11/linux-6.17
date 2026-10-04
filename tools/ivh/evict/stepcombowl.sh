#!/bin/bash
# Lock skipping + the two G-LOCK-38 fixes, on REAL workloads.
# qlockbench is the worst case for skipping: only ~0.1% of contended
# acquisitions there ever touch the MCS queue. hackbench is scheduler-heavy
# with genuine waiter preemption -- the regime where this mechanism family
# actually has something to repair.
#   base      no eviction
#   skip      eviction, unfixed
#   lookahead evict only when a LIVE replacement is confirmed
#   combo     lookahead + requeue_nosteal
# hackbench/dbench/ebizzy. hackbench is RAW SECONDS -- LOWER IS BETTER.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; B=/root/linux-6.17
BLOCKS=${BLOCKS:-10}; T=${T:-$(nproc)}; HOP=${HOP:-2}; HG=${HG:-4}; HL=${HL:-100000}; DUR=${DUR:-10}
WLS=${WLS:-"hackbench"}; ARMS=${ARMS:-"base skip lookahead combo"}
OUT=$D/cwl${HOP}_$(date +%m%d-%H%M%S).csv
echo "blk,hop,arm,wl,metric,marked,la_ref,steal_ok" > $OUT
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py \
        ivh_evict_marked ivh_evict_lookahead_refused ivh_evict_steal_ok 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
           ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none \
           ivh_pv_evict_lookahead ivh_pv_camp_probe; do echo 0 > $S/$k 2>/dev/null; done
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo $HOP > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  case $1 in
    skip)      echo 1 > $S/ivh_pv_evict_enable ;;
    lookahead) echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead ;;
    combo)     echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
               echo 1 > $S/ivh_pv_requeue_nosteal ;;
  esac
  [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] && [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "ARM FAIL"; exit 1; }
}
run_wl(){
  case $1 in
    hackbench) timeout -k 5 600 hackbench -T -g$HG -f8 -l$HL 2>&1|awk '/^Time:/{print $2}' ;;
    dbench)    cd /root/dbench_test 2>/dev/null||cd /root
               timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}' ;;
    ebizzy)    timeout -k 5 180 $B/ebizzy -t $T -S 10 2>&1|awk '/records\/s/{print $1}' ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for wl in $WLS; do
    for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
      setarm "$a"; read -r m0 l0 s0 <<< "$(ctr)"
      M=$(run_wl $wl); read -r m1 l1 s1 <<< "$(ctr)"
      echo "$b,$HOP,$a,$wl,${M:-0},$((m1-m0)),$((l1-l0)),$((s1-s0))" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_evict_lookahead; do echo 0 > $S/$k; done
echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
