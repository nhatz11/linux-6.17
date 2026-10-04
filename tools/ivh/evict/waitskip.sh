#!/bin/bash
# DOES LOCK SKIPPING REDUCE ACQUISITION WAIT TIME?
# Metric: ivh_slowpath_wait_ns / ivh_slowpath_wait_events (qspinlock.c:61-88),
# the successor to ivh_exec's ivh_obs_wait_*. Denominator = CONTENDED
# acquisitions only; uncontended fastpath never enters the slowpath and is
# excluded rather than averaged in as zero.
#
# Run at two spin thresholds:
#   32768   stock -- waiters exhaust and HALT, so wait time is dominated by the
#                    pv_wait/kick round trip, which masks queue-ordering effects
#   1048576 ~37ms tenure -- nothing halts, so wait time is PURE QUEUEING DELAY
#
# tier1 and tier2 OFF in both arms (per the request): the only difference
# between base and skip is eviction.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-8}; T=${T:-$(nproc)}; HG=${HG:-9}; HF=${HF:-4}; THRS=${THRS:-"32768 1048576"}
OUT=$D/waitskip_$(date +%m%d-%H%M%S).csv
echo "blk,wl,spin,arm,metric,wait_ns,wait_ev,halt_ev,marked" > $OUT
rd(){ timeout -k 5 150 python3 - <<'PY'
import subprocess,re
C=["ivh_slowpath_wait_ns","ivh_slowpath_wait_events","ivh_node_halt_events","ivh_evict_marked"]
o=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+C,
                 capture_output=True,text=True,timeout=140).stdout
d={"ivh_node_halt_events":0}
for ln in o.splitlines():
    m=re.match(r'\s*(\S+)\s*\[\s*(\S+)\s*\]\s*=\s*(\d+)',ln)
    if m and m.group(2)=="TOTAL": d[m.group(1)]=int(m.group(3)); continue
    m=re.match(r'\s*(\S+)\s*=\s*(\d+)\s*$',ln)
    if m: d[m.group(1)]=int(m.group(2))
print(" ".join(str(d.get(k,0)) for k in C))
PY
}
setarm(){  # $1 arm  $2 spin_threshold
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_enable ivh_pv_evict_debug \
           ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_camp_probe; do echo 0 > $S/$k; done
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask
  echo "$2" > $S/ivh_pv_spin_threshold
  echo 2 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  echo 1 > $S/ivh_slowpath_wait_measure
  [ "$1" = skip ] && { echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
                       echo 1 > $S/ivh_pv_requeue_nosteal; }
  [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] && [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "TIER FAIL"; exit 1; }
  [ "$(cat $S/ivh_pv_spin_threshold)" = "$2" ] || { echo "SPIN FAIL"; exit 1; }
}
wl(){ case $1 in
  hackbench) timeout -k 10 400 hackbench -T -g$HG -f$HF -l50000 2>&1|awk '/^Time:/{print $2}' ;;
  dentry)    timeout -k 10 120 stress-ng --dentry $T -t 15s --metrics-brief 2>&1|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' ;;
  dbench)    (cd /root/dbench_test; timeout -k 10 150 dbench -F -t 15 $T -D /root/dbench_test 2>&1|grep -oP 'Throughput\s+\K[0-9.]+') ;;
 esac; }
for b in $(seq 1 $BLOCKS); do
  for sp in $THRS; do
    for w in hackbench dentry dbench; do
      for a in $(printf "base\nskip\n"|shuf); do
        setarm "$a" "$sp"; read -r n0 e0 h0 m0 <<< "$(rd)"
        M=$(wl $w); read -r n1 e1 h1 m1 <<< "$(rd)"
        echo "$b,$w,$sp,$a,${M:-0},$((n1-n0)),$((e1-e0)),$((h1-h0)),$((m1-m0))" >> $OUT
      done
    done
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_slowpath_wait_measure; echo 32768 > $S/ivh_pv_spin_threshold
echo 0 > $S/ivh_pv_evict_enable; echo 0 > $S/ivh_pv_evict_lookahead; echo 0 > $S/ivh_pv_requeue_nosteal
echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
