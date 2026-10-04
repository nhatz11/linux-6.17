#!/bin/bash
# G-LOCK-37 HEAD BYPASS -- 3-arm campaign, ONE boot, sysctl-switched.
#   A baseline   relaxed=0 probe=1 enable=0
#   B prereq     relaxed=1 probe=1 enable=0
#   C combined   relaxed=1 probe=1 enable=1
# probe=1 in ALL arms so A->B is a pure relaxed step and B->C a pure enable
# step; the observer's own cost is held constant and never bundled into C.
#
# PRIMARY METRIC: iters. ops is contaminated -- qlockbench counts only
# SUCCESSFUL syscalls and the hit rate depends on acquisition order, which is
# exactly what the bypass changes.
#
# GATE (pre-registered): C beats BOTH A and B on iters with the 97.5% paired
# CI excluding 0. Prereq costs ~2.10%, so C-vs-A needs roughly +4%.
# VOID if arm C fires < 1000 total: "did not help" and "never ran" are not the
# same thing.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-24}; DUR=${DUR:-10}
OUT=$D/step3_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r)"; echo "blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
C="ivh_head_bypass_fired ivh_head_bypass_fired_exit ivh_head_bypass_capped ivh_head_bypass_raced_locked ivh_head_bypass_raced_clear ivh_head_obs_actionable ivh_rot_steals"
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,fired,fired_exit,capped,raced_lk,raced_cl,actionable,steals" > $OUT
ctr(){ timeout -k 5 60 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  $D/arm.sh control >/dev/null || { echo "ARM FAIL"; exit 1; }
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_rot_probe
  case $1 in
    A) echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_enable ;;
    B) echo 1 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_enable ;;
    C) echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable ;;
  esac
  [ "$(cat $S/ivh_pv_preempt_src)" = 2 ] || { echo "VOID: preempt_src != 2"; exit 1; }
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "A\nB\nC\n" | shuf); do
    setarm $a; B0=$(ctr)
    Q=$(timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    A0=$(ctr)
    DD=$(python3 -c "
b='$B0'.split(); a='$A0'.split()
print(','.join(str(int(a[i])-int(b[i])) for i in range(7)))")
    echo "$b,$a,$Q,$DD" >> $OUT
    printf "  blk%-3s %s iters=%-9s >1ms=%-7s fired=%s\n" "$b" "$a" \
      "$(echo $Q|cut -d, -f2)" "$(echo $Q|cut -d, -f9)" "$(echo $DD|cut -d, -f1)"
  done
done
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_pv_rot_probe
echo "DONE -> $OUT"
