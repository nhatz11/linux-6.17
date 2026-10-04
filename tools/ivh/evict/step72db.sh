#!/bin/bash
# dbench, 72 clients, 72 vCPUs. bypass+tier1 (shipping config) vs stock PV.
# Expected VOID: dbench head-stale is 2.86% vs qlockbench's 32.60% on the same
# machine, and actionable windows are 527/10s vs 43328/10s. 30s runs to
# accumulate fires. Pre-registered: <1000 fires in the bypass arm = VOID, no
# conclusion in either direction.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-30}; CLIENTS=${CLIENTS:-72}
OUT=$D/step72db_$(date +%H%M%S).csv
echo "blk,arm,mbps,fired,actionable,samples" > $OUT
C="ivh_head_bypass_fired ivh_head_obs_actionable ivh_head_obs_samples"
ctr(){ timeout -k 5 90 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  case $1 in
    pv)        /root/spin_mode 1 >/dev/null 2>&1 ;;
    bypass_t1) $D/arm.sh control >/dev/null || exit 1 ;;
  esac
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
  echo 0 > $S/ivh_pv_rot_probe; echo 1 > $S/ivh_head_bypass_probe
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  [ "$1" = bypass_t1 ] && { echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable; }
  local by; by=$(cat $S/ivh_head_bypass_enable)
  case $1 in
    pv)        [ "$by" = 0 ] || { echo "ARM FAIL pv by=$by"; exit 1; } ;;
    bypass_t1) [ "$by" = 1 ] || { echo "ARM FAIL bypass by=$by"; exit 1; } ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nbypass_t1\n"|shuf); do
    setarm "$a"; B0=$(ctr)
    T=$(timeout -k 5 $((DUR+90)) dbench -t $DUR $CLIENTS 2>&1|awk '/^Throughput/{print $2}')
    A0=$(ctr)
    DD=$(python3 -c "
b='$B0'.split(); a='$A0'.split()
print(','.join(str(int(a[i])-int(b[i])) for i in range(3)))")
    echo "$b,$a,${T:-0},$DD" >> $OUT
    printf "  blk%-3s %-10s %9s MB/s  fired=%-6s actionable=%s\n" "$b" "$a" "${T:-ERR}" \
      "$(echo $DD|cut -d, -f1)" "$(echo $DD|cut -d, -f2)"
  done
done
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_head_bypass_probe
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
echo "DONE -> $OUT"
