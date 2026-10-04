#!/bin/bash
# IS HEAD BYPASS FIRING ON REAL STRANDED HEADS, OR ON DETECTOR NOISE?
# Same standard that killed lock skipping, applied to head bypass.
# The observer uses the SAME staleness test that gave 27% false positives on the
# eviction path, and publish interval (143us) EXCEEDS the threshold (100us), so a
# running head reads stale for part of every publish window. hold=0 removes the
# duration filter entirely.
#   pv                      stock PV
#   t12                     tier1+tier2 only
#   byp_thr100  bypass open gate, threshold 220000 (100us)   <- current "winner"
#   byp_thr500  bypass open gate, threshold 1100000 (500us)  <- only long preemption
#   byp_thr1ms  bypass open gate, threshold 2200000 (1ms)    <- unambiguous only
# If the gain SURVIVES at 500us/1ms the episodes are genuine.
# If it VANISHES, bypass was firing on false staleness.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-8}; T=${T:-$(nproc)}
OUT=$D/bypreal_$(date +%m%d-%H%M%S).csv
echo "blk,wl,arm,metric,fired,blk_cyc,blk_ev,trunc_cyc,trunc_ev,actionable" > $OUT
C="ivh_head_bypass_fired ivh_head_blocked_cycles ivh_head_blocked_events ivh_head_blocked_trunc_cycles ivh_head_blocked_trunc_events ivh_head_obs_actionable"
ctr(){ timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py $C 2>/dev/null|awk '{printf "%s ",$3}'; }
zero(){ for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
        ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
        ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
        echo 3 > $S/ivh_head_bypass_runs; echo 220000 > $S/ivh_head_bypass_hold
        echo 220000 > $S/ivh_pv_beat_threshold; }
setarm(){
  zero
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
      [ "$(cat $S/ivh_head_bypass_enable)" = 0 ] || exit 1; return 0; fi
  $D/arm.sh nt1_only >/dev/null || exit 1; zero
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 4095 > $S/ivh_pv_beat_publish_mask
  echo 32768 > $S/ivh_pv_spin_threshold
  [ "$1" = t12 ] && return 0
  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
  echo 1 > $S/ivh_head_bypass_enable
  echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
  case $1 in
    byp_thr100) echo 220000  > $S/ivh_pv_beat_threshold ;;
    byp_thr500) echo 1100000 > $S/ivh_pv_beat_threshold ;;
    byp_thr1ms) echo 2200000 > $S/ivh_pv_beat_threshold ;;
  esac
}
run_wl(){ case $1 in
  hackbench) timeout -k 5 300 hackbench -T -g4 -f8 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
  dbench)    (cd /root/dbench_test; timeout -k 5 90 dbench -F -t 15 $T -D /root/dbench_test 2>&1|grep -oP 'Throughput\s+\K[0-9.]+') ;;
 esac; }
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench dentry dbench; do
    for a in $(printf "pv\nt12\nbyp_thr100\nbyp_thr500\nbyp_thr1ms\n"|shuf); do
      setarm "$a"; read -r f0 bc0 be0 tc0 te0 ac0 <<< "$(ctr)"
      M=$(run_wl $wl); read -r f1 bc1 be1 tc1 te1 ac1 <<< "$(ctr)"
      echo "$b,$wl,$a,${M:-0},$((f1-f0)),$((bc1-bc0)),$((be1-be0)),$((tc1-tc0)),$((te1-te0)),$((ac1-ac0))" >> $OUT
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; zero
echo "DONE -> $OUT"
