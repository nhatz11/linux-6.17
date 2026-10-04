#!/bin/bash
# IS LOCK STEALING AVAILABLE IN NO-PV, AND DOES SKIPPING GO NULL THERE TOO?
# Structural half already proven from source: ivh_pv_allow appears in
# qspinlock_paravirt.h only at :1268/:1280 (the bail/halt decision). The steal
# valve (#define queued_spin_trylock -> pv_hybrid_queued_unfair_trylock, :183)
# and set_pending() (:3398/:3756) are untouched by allow=0 -- only the wait/wake
# VEHICLE changes (IPI + safe_halt instead of hypercall).
#
# PART 1  observe-only in mode 4: ivh_rot_preempted / ivh_rot_handoffs (the
#         preempted-successor rate, SAME instrument as the vanilla-PV run so the
#         numbers are directly comparable) and ivh_rot_steals / ivh_rot_handoffs
#         (does stealing actually happen without PV?).
# PART 2  A/B base vs skip, tier1+tier2 OFF, so only eviction differs.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-8}; T=${T:-$(nproc)}; REPS=${REPS:-3}
[ "$(cat $S/ivh_pv_allow)" = 0 ] || { echo "WRONG BOOT: need ivh_pv_allow=0"; exit 1; }
[ "$(cat $S/ivh_pv_tas)" = 0 ] || { echo "WRONG BOOT: need ivh_pv_tas=0"; exit 1; }
case "$(uname -r)" in *G-LOCK-38*) ;; *) echo "WRONG KERNEL $(uname -r)"; exit 1;; esac
O1=$D/nopv_rates_$(date +%m%d-%H%M%S).csv
O2=$D/nopv_ab_$(date +%m%d-%H%M%S).csv
echo "rep,wl,handoffs,preempted,steals,stop_halted" > $O1
echo "blk,wl,arm,metric,wait_ns,wait_ev,marked" > $O2
zero(){ for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
        ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
        ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done; }
wl(){ case $1 in
  hackbench) timeout -k 10 400 hackbench -T -g9 -f4 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 10 120 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
  dbench)    (cd /root/dbench_test; timeout -k 10 150 dbench -F -t 15 $T -D /root/dbench_test 2>&1|grep -oP 'Throughput\s+\K[0-9.]+') ;;
 esac; }
# ---------------- PART 1: rates, observe only ----------------
C1="ivh_rot_handoffs ivh_rot_preempted ivh_rot_steals ivh_rot_stop_halted"
r1(){ timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py $C1 2>/dev/null|awk '{printf "%s ",$3}'; }
echo "### PART 1: preempted-successor and steal rates in IVH_NOPV (mode 4)"
for r in $(seq 1 $REPS); do
  for w in hackbench dentry dbench; do
    /root/spin_mode 4 >/dev/null 2>&1 || { echo "mode 4 FAIL"; exit 1; }
    zero; echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
    echo 4095 > $S/ivh_pv_beat_publish_mask; echo 1 > $S/ivh_pv_rot_probe
    read -r h0 p0 s0 x0 <<< "$(r1)"; wl $w >/dev/null; read -r h1 p1 s1 x1 <<< "$(r1)"
    echo "$r,$w,$((h1-h0)),$((p1-p0)),$((s1-s0)),$((x1-x0))" >> $O1
  done
  printf "  part1 rep%-3s done\n" "$r"
done
# ---------------- PART 2: base vs skip ----------------
C2="ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_evict_marked"
r2(){ timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py $C2 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  /root/spin_mode 4 >/dev/null 2>&1 || exit 1
  zero
  echo 0 > $S/ivh_pv_tier1_enable; echo 0 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo 2 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  echo 1 > $S/ivh_slowpath_wait_measure
  [ "$1" = skip ] && { echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
                       echo 1 > $S/ivh_pv_requeue_nosteal; }
  [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] && [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "TIER FAIL"; exit 1; }
}
echo "### PART 2: base vs skip in IVH_NOPV, tier1+tier2 OFF"
for b in $(seq 1 $BLOCKS); do
  for w in hackbench dentry dbench; do
    for a in $(printf "base\nskip\n"|shuf); do
      setarm "$a"; read -r n0 e0 m0 <<< "$(r2)"
      M=$(wl $w); read -r n1 e1 m1 <<< "$(r2)"
      echo "$b,$w,$a,${M:-0},$((n1-n0)),$((e1-e0)),$((m1-m0))" >> $O2
    done
  done
  printf "  part2 blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; zero
echo 0 > $S/ivh_slowpath_wait_measure; echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $O1 , $O2"
