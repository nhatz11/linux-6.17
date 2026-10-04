#!/bin/bash
# IS THE NOPV WIN REAL, OR THE BACKOFF PEDESTAL AGAIN?
#   van   mode 1: adaptive_mode 0, src 0, no early bail
#   pub   mode 4 then tier1 OFF + tier2 OFF  -> publish ON, NO consumer
#   nopv  mode 4 full: tier1 + tier2 ON      -> publish CONSUMED
# pub-vs-van isolates backoff; nopv-vs-pub isolates the early bail.
set -u; S=/proc/sys/kernel; Q=/root/linux-6.17/qlockbench; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}; T=${T:-$(nproc)}
[ "$(cat $S/ivh_pv_allow)" = 0 ] || { echo "WRONG BOOT: need ivh_pv_allow=0"; exit 1; }
OUT=$D/nopv2_$(date +%m%d-%H%M%S).csv
echo "blk,arm,wl,metric,src,t1,t2" > $OUT
setarm(){
  case $1 in
    van)  /root/spin_mode 1 >/dev/null || exit 1 ;;
    pub)  /root/spin_mode 4 >/dev/null || exit 1
          echo 0 > $S/ivh_pv_tier1_enable; echo 0 > $S/ivh_pv_tier2_enable ;;
    nopv) /root/spin_mode 4 >/dev/null || exit 1
          echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable ;;
  esac
}
st(){ echo "$(cat $S/ivh_pv_preempt_src),$(cat $S/ivh_pv_tier1_enable),$(cat $S/ivh_pv_tier2_enable)"; }
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "van\npub\nnopv\n"|shuf); do
    setarm "$a"; V=$(st)
    M=$(timeout -k 5 $((DUR+50)) $Q -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2)
    echo "$b,$a,qlock,${M:-0},$V" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo "DONE -> $OUT"
