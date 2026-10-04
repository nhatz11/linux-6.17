#!/bin/bash
# CROSS-BOOT lock comparison. Detects which boot it is from the read-only
# boot params and runs the arms that boot can reach.
#   ivh_pv_tas=1              -> mode 3 STOCK_TAS only (our code is INERT)
#   ivh_pv_allow=0, tas=0     -> mode 1 + mode 4 IVH_NOPV
#   tas=0, allow=1 (normal)   -> mode 1 STOCK_PV reference
# Every boot also runs an identical CANARY (uncontended qlockbench -t 1) so
# cross-boot host drift can be detected and normalised.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; Q=/root/linux-6.17/qlockbench
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}; T=${T:-$(nproc)}
TAS=$(cat $S/ivh_pv_tas); ALLOW=$(cat $S/ivh_pv_allow)
if   [ "$TAS" = 1 ];   then BOOT=tas;    ARMS="tas"
elif [ "$ALLOW" = 0 ]; then BOOT=nopv;   ARMS="van nopv"
else                        BOOT=normal; ARMS="pv"
fi
OUT=$D/tas_${BOOT}_$(date +%m%d-%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "boot=$BOOT kernel=$(uname -r) ncpu=$(nproc) T=$T blocks=$BLOCKS dur=$DUR"
  echo "cmdline=$(cat /proc/cmdline)"
  echo "ivh_pv_tas=$TAS ivh_pv_allow=$ALLOW unhalt=$(cat $S/ivh_pv_unhalt_avail)"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "boot,blk,arm,wl,metric" > $OUT
setarm(){
  case $1 in
    tas)  /root/spin_mode 3 >/dev/null || { echo "MODE 3 FAIL"; exit 1; } ;;
    van)  /root/spin_mode 1 >/dev/null || { echo "MODE 1 FAIL"; exit 1; } ;;
    pv)   /root/spin_mode 1 >/dev/null || { echo "MODE 1 FAIL"; exit 1; } ;;
    nopv) /root/spin_mode 4 >/dev/null || { echo "MODE 4 FAIL"; exit 1; } ;;
  esac
}
canary(){ timeout -k 5 $((DUR+40)) $Q -t 1 -d 5 -Q 2>&1|tail -1|cut -d, -f2; }
echo "boot=$BOOT arms='$ARMS' T=$T"
echo "$BOOT,0,canary,pre,$(canary)" >> $OUT
for b in $(seq 1 $BLOCKS); do
  for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
    setarm "$a"
    M=$(timeout -k 5 $((DUR+50)) $Q -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2)
    echo "$BOOT,$b,$a,qlock,${M:-0}" >> $OUT
  done
  for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
    setarm "$a"
    cd /root/dbench_test 2>/dev/null || cd /root
    M=$(timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}')
    echo "$BOOT,$b,$a,dbench,${M:-0}" >> $OUT
  done
  for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
    setarm "$a"
    M=$(timeout -k 5 300 hackbench -T -g${HG:-4} -f8 -l50000 2>&1|awk '/^Time:/{printf "%.4f\n", 1000/$2}')
    echo "$BOOT,$b,$a,hackbench,${M:-0}" >> $OUT
  done
  for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
    setarm "$a"
    M=$(timeout -k 5 180 /root/linux-6.17/ebizzy -t $T -S 10 2>&1|awk '/records\/s/{print $1}')
    echo "$BOOT,$b,$a,ebizzy,${M:-0}" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo "$BOOT,99,canary,post,$(canary)" >> $OUT
echo "DONE -> $OUT"
