#!/bin/bash
# HOW OFTEN IS THE IMMEDIATE SUCCESSOR PREEMPTED, IN VANILLA PV QSPINLOCK?
# Pure observation. ivh_rot_probe_walk() increments, in the SAME function under
# the SAME `probe` gate, exactly once per handoff:
#   ivh_rot_handoffs  -- every handoff observed          (denominator)
#   ivh_rot_preempted -- successor classified PREEMPTED  (numerator)
# No attribution, no sampling, no dropped population, no halted-only restriction.
# Lock runs as VANILLA PV (adaptive_mode 0, no mechanism acts). preempt_src=2 is
# required or the classifier falls back to vcpu_is_preempted(), hardwired false
# on TDX, which would report zero by construction.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
REPS=${REPS:-3}; T=${T:-$(nproc)}
OUT=$D/vanilla_preempt_$(date +%m%d-%H%M%S).csv
echo "rep,wl,thr_us,handoffs,preempted,stop_halted,tail_stop,steals" > $OUT
C="ivh_rot_handoffs ivh_rot_preempted ivh_rot_stop_halted ivh_rot_tail_stop ivh_rot_steals"
ctr(){ timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setup(){  # $1 = threshold in cycles
  /root/spin_mode 1 >/dev/null 2>&1          # VANILLA PV
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
           ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
  echo 2 > $S/ivh_pv_preempt_src               # classifier needs the heartbeat
  echo "$1" > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask
  echo 1 > $S/ivh_pv_rot_probe                 # OBSERVE ONLY - acts on nothing
  [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "not vanilla"; exit 1; }
  [ "$(cat $S/ivh_pv_rot_enable)" = 0 ] || exit 1
}
wl(){ case $1 in
  hackbench) timeout -k 5 300 hackbench -T -g4 -f8 -l50000 >/dev/null 2>&1 ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t 15s >/dev/null 2>&1 ;;
  dbench)    (cd /root/dbench_test; timeout -k 5 90 dbench -F -t 15 $T -D /root/dbench_test >/dev/null 2>&1) ;;
 esac; }
for r in $(seq 1 $REPS); do
  for thr in 220000 1100000; do
    for w in hackbench dentry dbench; do
      setup $thr; read -r h0 p0 s0 t0 st0 <<< "$(ctr)"
      wl $w; read -r h1 p1 s1 t1 st1 <<< "$(ctr)"
      echo "$r,$w,$((thr/2200)),$((h1-h0)),$((p1-p0)),$((s1-s0)),$((t1-t0)),$((st1-st0))" >> $OUT
    done
  done
  printf "  rep%-3s done\n" "$r"
done
echo 0 > $S/ivh_pv_rot_probe; /root/spin_mode 1 >/dev/null 2>&1
echo 220000 > $S/ivh_pv_beat_threshold
echo "DONE -> $OUT"
