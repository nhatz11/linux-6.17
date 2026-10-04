#!/bin/bash
# THREAD SWEEP: does head bypass's advantage survive more contenders?
#
# Oversubscribing threads on 16 vCPUs reproduces everything a 70-vCPU VM does on
# the COST side -- deeper MCS queues, N-wide test-and-set storms in the unfair
# steal loop, queue transit vs spin threshold, pv_hash pressure -- with no host
# cooperation. It does NOT reproduce host preemption, so read this purely as a
# cost curve.
#
# The concern: head bypass works by reopening pv_hybrid_queued_unfair_trylock(),
# a TEST-AND-SET loop whose handoff cost is O(N) -- the very thing MCS exists to
# eliminate. Benefit is flat in N; cost is superlinear. If the C-vs-A delta is
# falling by t=64, the "more vCPUs is better" theory is dead as stated.
#
# ivh_pv_rot_probe=0 here (it was 1 in the winning campaign -- constant across
# arms so it could not create the delta, but it is an rdtsc + remote read on
# every slowpath acquisition and inflates the baseline).
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-6}; DUR=${DUR:-10}
OUT=$D/step5_$(date +%H%M%S).csv
echo "blk,threads,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,fired,steals,node_att" > $OUT
C="ivh_head_bypass_fired ivh_rot_steals ivh_node_spin_attempts"
ctr(){ timeout -k 5 60 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){ $D/arm.sh control >/dev/null || exit 1
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 0 > $S/ivh_pv_rot_probe; echo 1 > $S/ivh_head_bypass_probe
  case $1 in
    A) echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_enable ;;
    C) echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable ;;
  esac; }
for b in $(seq 1 $BLOCKS); do
  for t in 16 32 64 96 128; do
    for a in $(printf "A\nC\n"|shuf); do
      setarm $a; B0=$(ctr)
      Q=$(timeout -k 5 $((DUR+40)) /root/linux-6.17/qlockbench -t $t -d $DUR -Q 2>&1|tail -1)
      A0=$(ctr)
      DD=$(python3 -c "
b='$B0'.split(); a='$A0'.split()
print(','.join(str(int(a[i])-int(b[i])) for i in range(3)))")
      echo "$b,$t,$a,$Q,$DD" >> $OUT
    done
    printf "  blk%-2s t=%-4s done\n" "$b" "$t"
  done
done
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_probe
echo "DONE -> $OUT"
