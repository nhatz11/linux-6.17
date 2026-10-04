#!/bin/bash
# hackbench + ebizzy, no-PV boot. Arms paired within the boot:
#   van   MCS queue + busy-spin, src=0, NO pause, no IVH
#   pub   van + the arrival rdtsc store (the pause), tier1+tier2 OFF
#   nopv  pub + tier1 + tier2 ON (the heartbeat actually consumed)
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; B=/root/linux-6.17
BLOCKS=${BLOCKS:-10}; T=${T:-$(nproc)}; HG=${HG:-4}
[ "$(cat $S/ivh_pv_allow)" = 0 ] || { echo "WRONG BOOT: need ivh_pv_allow=0"; exit 1; }
OUT=$D/wl2_$(date +%m%d-%H%M%S).csv
echo "blk,arm,wl,metric" > $OUT
setarm(){
  case $1 in
    van)  /root/spin_mode 1 >/dev/null || exit 1 ;;
    pub)  /root/spin_mode 4 >/dev/null || exit 1
          echo 0 > $S/ivh_pv_tier1_enable; echo 0 > $S/ivh_pv_tier2_enable ;;
    nopv) /root/spin_mode 4 >/dev/null || exit 1
          echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable ;;
  esac
}
run_wl(){   # ONE number, higher = better
  case $1 in
    hackbench) timeout -k 5 300 hackbench -T -g$HG -f8 -l50000 2>&1 |
                 awk '/^Time:/{printf "%.4f\n", 1000/$2}' ;;
    ebizzy)    timeout -k 5 180 $B/ebizzy -t $T -S 10 2>&1 |
                 awk '/records\/s/{print $1}' ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench ebizzy; do
    for a in $(printf "van\npub\nnopv\n"|shuf); do
      setarm "$a"; M=$(run_wl $wl)
      echo "$b,$a,$wl,${M:-0}" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT"
