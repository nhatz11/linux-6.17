#!/bin/bash
# 72 vCPUs, 72 threads. Each mechanism ALONE vs stock PV: no tier 1, no tier 2,
# no is_cs_preempted.
#
# ARM HYGIENE: ivh_head_bypass_* are NEW knobs and NOTHING resets them --
# `grep -c head_bypass /root/spin_mode /root/ivh_tools/evict/arm.sh` == 0,0.
# A previous version of this script returned early for the pv arm and leaked
# bypass_enable=1 into pv and skip. Every arm now writes the COMPLETE vector and
# verifies it before the run.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-15}; DUR=${DUR:-10}; THREADS=${THREADS:-72}
OUT=$D/step72c_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) threads=$THREADS blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,evicts,fired" > $OUT
C="ivh_evict_marked ivh_head_bypass_fired"
ctr(){ timeout -k 5 90 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }

setarm(){
  case $1 in
    pv)     /root/spin_mode 1 >/dev/null 2>&1 ;;
    nt1)    $D/arm.sh nt1_only >/dev/null || exit 1 ;;
    skip)   HOPCAP=4 REQMAX=4 $D/arm.sh evict_nt1 >/dev/null || exit 1 ;;
    bypass) $D/arm.sh nt1_only >/dev/null || exit 1 ;;
  esac
  # --- complete vector, written for EVERY arm ---
  echo 0 > $S/ivh_head_bypass_enable
  echo 0 > $S/ivh_pv_trylock_relaxed
  echo 0 > $S/ivh_pv_rot_probe
  echo 1 > $S/ivh_head_bypass_probe        # observer cost constant across arms
  echo 511 > $S/ivh_pv_beat_publish_mask
  echo 220000 > $S/ivh_pv_beat_threshold
  [ "$1" = bypass ] && { echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable; }
  # --- verify ---
  local ev by
  ev=$(cat $S/ivh_pv_evict_enable); by=$(cat $S/ivh_head_bypass_enable)
  case $1 in
    pv|nt1) [ "$ev" = 0 ] && [ "$by" = 0 ] || { echo "ARM FAIL $1 ev=$ev by=$by"; exit 1; } ;;
    skip)   [ "$ev" = 1 ] && [ "$by" = 0 ] || { echo "ARM FAIL skip ev=$ev by=$by"; exit 1; } ;;
    bypass) [ "$ev" = 0 ] && [ "$by" = 1 ] || { echo "ARM FAIL bypass ev=$ev by=$by"; exit 1; } ;;
  esac
  return 0
}

for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nnt1\nskip\nbypass\n"|shuf); do
    setarm "$a"; B0=$(ctr)
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $THREADS -d $DUR -Q 2>&1|tail -1)
    A0=$(ctr)
    DD=$(python3 -c "
b='$B0'.split(); a='$A0'.split()
print(','.join(str(int(a[i])-int(b[i])) for i in range(2)))")
    echo "$b,$a,$Q,$DD" >> $OUT
    printf "  blk%-3s %-7s iters=%-9s >1ms=%-7s ev=%-6s fired=%s\n" "$b" "$a" \
      "$(echo $Q|cut -d, -f2)" "$(echo $Q|cut -d, -f9)" "$(echo $DD|cut -d, -f1)" "$(echo $DD|cut -d, -f2)"
  done
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
echo "DONE -> $OUT"
