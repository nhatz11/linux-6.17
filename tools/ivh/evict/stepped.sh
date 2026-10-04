#!/bin/bash
# WHAT IS THE PEDESTAL? 3 arms, one variable at a time.
#   pv    stock PV (mode 0, tier1 on, src 0)
#   src0  mode 2, tier1 OFF, tier2 OFF, src 0  <- mode+tier1-off, NO publish
#   base  src0 + preempt_src=2                 <- adds ONLY the publish
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}; T=${T:-36}
OUT=$D/stepped_$(date +%H%M%S).csv
echo "blk,arm,iters" > $OUT
setarm(){
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1; return 0; fi
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp \
           ivh_pv_evict_debug ivh_pv_evict_enable ivh_pv_tier2_enable; do echo 0 > $S/$k; done
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  case $1 in src0) echo 0 > $S/ivh_pv_preempt_src ;; base) echo 2 > $S/ivh_pv_preempt_src ;; esac
  local t1 t2 sr; t1=$(cat $S/ivh_pv_tier1_enable); t2=$(cat $S/ivh_pv_tier2_enable); sr=$(cat $S/ivh_pv_preempt_src)
  [ "$t1" = 0 ] && [ "$t2" = 0 ] || { echo "ARM FAIL $1"; exit 1; }
  case $1 in src0) [ "$sr" = 0 ]||exit 1;; base) [ "$sr" = 2 ]||exit 1;; esac
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nsrc0\nbase\n"|shuf); do
    setarm "$a"
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2)
    echo "$b,$a,${Q:-0}" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT"
