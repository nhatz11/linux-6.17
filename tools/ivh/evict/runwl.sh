#!/bin/bash
# Eviction A/B on REAL workloads at 16 threads (nproc), under host contention.
#
# Motivation: at 16 threads on ONE lock (qlockbench) steal share is 60-70%, so
# the steal valve covers every case eviction targets and eviction measures as a
# null. Real workloads spread contention over many locks, where steal share
# collapsed to 0.6-6% in the depth sweep -- i.e. NO stealer is available and the
# queue order is the only thing that decides who gets the lock.
#
# Fourth arm tests the other hypothesis: eviction's null at 16 threads was a
# RATE problem (139 evictions/s vs 3192 >1ms stalls/s), not an opportunity
# problem. The detector is insensitive because the staleness threshold (1ms) is
# 10x the heartbeat publish interval (~93us). evict_sens publishes 8x more often
# (mask 511 ~= 12us) with a 100us threshold -- still 8x headroom, so still a
# VALID detector, but sensitive to preemptions in the 100us-1ms band we are
# currently blind to. Threshold and publish rate are inseparable by design.
#
# Tier 1 is ON in every IVH arm: lat_144330 showed removing it costs +6.73%
# more >1ms stalls (t=+3.46) with zero throughput compensation.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel; B=/root/linux-6.17
BLOCKS=${BLOCKS:-5}
OUT=$D/wl_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel: $(uname -r)"; echo "kern_sha: $(git -C /root/kernels/linux-6.17-vanilla rev-parse --short HEAD 2>/dev/null)"
  echo "blocks=$BLOCKS"; for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,wl,arm,metric,steals,marked,cap" > $OUT

ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_rot_steals ivh_evict_marked 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }

setarm(){
  case $1 in
    pv)         /root/spin_mode 1 >/dev/null 2>&1 ;;
    evict_off)  $D/arm.sh control >/dev/null || return 1 ;;
    evict)      HOPCAP=1 REQMAX=1 $D/arm.sh evict >/dev/null || return 1 ;;
    evict_sens) HOPCAP=1 REQMAX=1 $D/arm.sh evict >/dev/null || return 1 ;;
  esac
  echo 1 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_pv_evict_quiet
  echo 0 > $S/ivh_pv_evict_age_hist; echo 1 > $S/ivh_pv_evict_node_stamp
  if [ "$1" = evict_sens ]; then
      echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  else
      echo 4095 > $S/ivh_pv_beat_publish_mask; echo 2200000 > $S/ivh_pv_beat_threshold
  fi
  [ "$(cat $S/ivh_pv_rot_probe)" = 1 ]
}

# Each prints ONE number, higher = better.
run_wl(){
  case $1 in
    hackbench) timeout -k 5 180 hackbench -T -g1 -f8 -l200000 2>&1 |
                 awk '/^Time:/{printf "%.4f\n", 1000/$2}' ;;   # 1/time, scaled
    dbench)    timeout -k 5 180 dbench -t 10 16 2>&1 |
                 awk '/^Throughput/{print $2}' ;;
    ebizzy)    timeout -k 5 120 $B/ebizzy -t 16 -S 10 2>&1 |
                 awk '/records\/s/{print $1}' ;;
  esac
}

ARMS=(pv evict_off evict evict_sens)
for b in $(seq 1 $BLOCKS); do
  for wl in hackbench dbench ebizzy; do
    n=${#ARMS[@]}; ORDER=()
    for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
    for a in "${ORDER[@]}"; do
      setarm $a || { echo "ARM FAIL $a"; exit 1; }
      sleep 5; C=$(cap); X=$(ctr)
      M=$(run_wl $wl)
      Y=$(ctr)
      read st ma <<< "$(python3 -c "
x='$X'.split(); y='$Y'.split()
print(' '.join(str(int(y[i])-int(x[i])) for i in range(2)))")"
      [ -z "$M" ] && M=0
      echo "$b,$wl,$a,$M,$st,$ma,$C" >> $OUT
      printf "blk%-2s %-10s %-11s metric=%-12s steals=%-11s evicts=%-8s cap=%s\n" "$b" "$wl" "$a" "$M" "$st" "$ma" "$C"
    done
  done
done
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
echo 0 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_pv_evict_node_stamp; /root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT"
