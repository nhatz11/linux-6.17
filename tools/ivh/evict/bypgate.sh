#!/bin/bash
# Does head bypass help once its gate actually lets it fire?
#   pv        stock PV
#   t12       tier1+tier2
#   t12byp    t12 + bypass, SHIPPED gate (runs=3 hold=220000)  -> fires ~0
#   t12bypL   t12 + bypass, OPEN gate   (runs=1 hold=0)        -> fires ~2200
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-10}; T=${T:-$(nproc)}
OUT=$D/bypgate2_$(date +%m%d-%H%M%S).csv
echo "blk,wl,arm,metric,fired,actionable" > $OUT
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py ivh_head_bypass_fired ivh_head_obs_actionable 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  # ARM HYGIENE: zero EVERY mechanism knob FIRST, including for pv. spin_mode 1's
  # reset_skip_knobs does NOT touch ivh_head_bypass_*, so a bare `spin_mode 1;
  # return` leaks the previous arm's bypass settings into the pv arm. That bug
  # voided the pv column of bypgate_2226.
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
           ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
  echo 3 > $S/ivh_head_bypass_runs; echo 220000 > $S/ivh_head_bypass_hold
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
      local by; by=$(cat $S/ivh_head_bypass_enable)
      [ "$by" = 0 ] || { echo "PV ARM LEAK by=$by"; exit 1; }
      return 0; fi
  $D/arm.sh nt1_only >/dev/null || exit 1
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  case $1 in
    t12byp)  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
             echo 1 > $S/ivh_head_bypass_enable
             echo 3 > $S/ivh_head_bypass_runs; echo 220000 > $S/ivh_head_bypass_hold ;;
    t12bypL) echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
             echo 1 > $S/ivh_head_bypass_enable
             echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold ;;
  esac
}
run_wl(){ case $1 in
  hackbench) timeout -k 5 300 hackbench -T -g4 -f8 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
  dbench)    (cd /root/dbench_test; timeout -k 5 90 dbench -F -t 15 $T -D /root/dbench_test 2>&1|grep -oP 'Throughput\s+\K[0-9.]+') ;;
 esac; }
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench dentry dbench; do
    for a in $(printf "pv\nt12\nt12byp\nt12bypL\n"|shuf); do
      setarm "$a"; read -r f0 c0 <<< "$(ctr)"
      M=$(run_wl $wl); read -r f1 c1 <<< "$(ctr)"
      echo "$b,$wl,$a,${M:-0},$((f1-f0)),$((c1-c0))" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 3 > $S/ivh_head_bypass_runs; echo 220000 > $S/ivh_head_bypass_hold
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_pv_trylock_relaxed
echo "DONE -> $OUT"
