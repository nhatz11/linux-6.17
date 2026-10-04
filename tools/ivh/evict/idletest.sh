#!/bin/bash
# DOES EVICTION KEEP THE LOCK MOVING?
# ivh_rot_idle_* measures release -> next acquisition: how long the lock sat
# FREE BUT UNHELD -- precisely the stall lock skipping exists to prevent.
#   base  tier1+tier2, NO eviction
#   skip  tier1+tier2 + best eviction (lookahead + nosteal, hop_cap=2)
# If eviction keeps the lock moving, idle cycles/event MUST fall. If unchanged,
# the lock was never stuck: a preempted successor never reaches set_pending()
# (:3271), so the steal valve stays OPEN and the next arrival takes the lock.
# rot_probe adds an rdtsc at acquisition -> absolute THROUGHPUT here is void;
# it is ON IN BOTH ARMS so the idle ratio is the comparable quantity.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-6}; T=${T:-$(nproc)}
OUT=$D/idlecls_$(date +%m%d-%H%M%S).csv
echo "blk,wl,arm,metric,live_cyc,live_ev,stale_cyc,stale_ev,imp_cyc,imp_ev,skip_cyc,skip_ev,unknown,backward,marked" > $OUT
ctr(){ timeout -k 5 180 python3 /root/ivh_tools/evict/idlecls.py 2>/dev/null; }
setarm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
           ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo 2 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  echo 1 > $S/ivh_pv_rot_probe            # the idle instrument, ON IN BOTH ARMS
  [ "$1" = skip ] && { echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
                       echo 1 > $S/ivh_pv_requeue_nosteal; }
}
run_wl(){ case $1 in
  hackbench) timeout -k 5 300 hackbench -T -g4 -f8 -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
 esac; }
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench dentry; do
    for a in $(printf "base\nskip\n"|shuf); do
      setarm "$a"; B=$(ctr); M=$(run_wl $wl); A=$(ctr)
      python3 - "$b" "$wl" "$a" "${M:-0}" "$B" "$A" >> $OUT <<'PY'
import sys
b,wl,a,m,B,A=sys.argv[1:7]
d=[int(float(x))-int(float(y)) for x,y in zip(A.split(),B.split())]
print(",".join([b,wl,a,m]+[str(x) for x in d]))
PY
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_pv_evict_enable
echo 0 > $S/ivh_pv_evict_lookahead; echo 0 > $S/ivh_pv_requeue_nosteal
echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
