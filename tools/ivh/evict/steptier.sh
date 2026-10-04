#!/bin/bash
# POINTER 2: does tier1+tier2 beat stock PV in the PV boot? Never measured
# cleanly at 36 vCPU -- every arm in today's campaigns had tier1/tier2 OFF.
#   pv    stock PV (spin_mode 1)
#   pub   adaptive_mode 2, src=2, tier1 OFF tier2 OFF  <- pedestal control
#   t12   adaptive_mode 2, src=2, tier1 ON  tier2 ON   <- the mechanism
# t12-vs-pub is the clean single-variable contrast (pedestal held constant).
# t12-vs-pv is the shipping-config answer.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; B=/root/linux-6.17
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}; T=${T:-$(nproc)}; HG=${HG:-4}
[ "$(cat $S/ivh_pv_tas)" = 0 ] && [ "$(cat $S/ivh_pv_allow)" = 1 ] || { echo "WRONG BOOT: need the normal PV boot"; exit 1; }
OUT=$D/tier_$(date +%m%d-%H%M%S).csv
echo "blk,arm,wl,metric" > $OUT
setarm(){
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null; return 0; fi
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp \
           ivh_pv_evict_debug ivh_pv_evict_enable; do echo 0 > $S/$k; done
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask
  case $1 in
    pub) echo 0 > $S/ivh_pv_tier1_enable; echo 0 > $S/ivh_pv_tier2_enable ;;
    t12) echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable ;;
  esac
  local a b; a=$(cat $S/ivh_pv_tier1_enable); b=$(cat $S/ivh_pv_tier2_enable)
  case $1 in pub) [ "$a$b" = 00 ]||exit 1;; t12) [ "$a$b" = 11 ]||exit 1;; esac
}
run_wl(){
  case $1 in
    qlock)     timeout -k 5 $((DUR+50)) $B/qlockbench -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2 ;;
    dbench)    cd /root/dbench_test 2>/dev/null||cd /root
               timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}' ;;
    hackbench) timeout -k 5 300 hackbench -T -g$HG -f8 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
    ebizzy)    timeout -k 5 180 $B/ebizzy -t $T -S 10 2>&1|awk '/records\/s/{print $1}' ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for wl in qlock dbench hackbench ebizzy; do
    for a in $(printf "pv\npub\nt12\n"|shuf); do
      setarm "$a"; M=$(run_wl $wl); echo "$b,$a,$wl,${M:-0}" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT   (NOTE: hackbench is RAW SECONDS -- lower is better)"
