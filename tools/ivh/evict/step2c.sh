#!/bin/bash
# Step 2, WITH kernel counters -- settles the attribution the first run could not.
# Claim under test: relaxed lets the HEAD win acquisitions it used to lose to
# stealers, and head-won acquisitions are the expensive path. If true, head
# acquisitions rise and head exhaustions/halts fall in the relaxed arm.
# Also resolves the hash_head=0 contradiction.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}
OUT=$D/step2c_$(date +%H%M%S).csv
C="ivh_head_spin_enter ivh_head_spin_attempts ivh_head_arm ivh_halt_from_head ivh_hash_ins_head ivh_node_spin_attempts ivh_rot_steals"
echo "blk,relaxed,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,head_enter,head_exhaust,head_arm,head_halt,hash_head,node_att,steals" > $OUT
c(){ timeout -k 5 60 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){ $D/arm.sh control >/dev/null || exit 1
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 0 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_rot_probe
  echo "$1" > $S/ivh_pv_trylock_relaxed
  [ "$(cat $S/ivh_pv_trylock_relaxed)" = "$1" ] || exit 1; }
for b in $(seq 1 $BLOCKS); do
  for r in $(printf "0\n1\n"|shuf); do
    setarm $r; B=$(c)
    Q=$(timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    A=$(c)
    D7=$(python3 -c "
b='$B'.split(); a='$A'.split()
print(','.join(str(int(a[i])-int(b[i])) for i in range(7)))")
    echo "$b,$r,$Q,$D7" >> $OUT
    printf "  blk%-3s relaxed=%s iters=%-9s head_enter=%-8s exhaust=%-7s halt=%-7s hash=%-7s\n" \
      "$b" "$r" "$(echo $Q|cut -d, -f2)" "$(echo $D7|cut -d, -f1)" "$(echo $D7|cut -d, -f2)" "$(echo $D7|cut -d, -f4)" "$(echo $D7|cut -d, -f5)"
  done
done
echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
echo "DONE -> $OUT"
