#!/bin/bash
# Can is_cs_preempted() replace threshold exhaustion? Migration ON, IVH+AS.
#   D = default threshold 32768, all ivh_cs off            (today)
#   F = default threshold, fast stamp on, probe off        (cost of stamping every lock)
#   X = threshold max, all ivh_cs off                      (exhaustion removed, no replacement)
#   B = threshold max, stamp+clear+fast+probe+BAIL on      (exhaustion replaced by is_cs_preempted)
# Order D F X B B X F D, ROUNDS each, capacity-settled wait before each arm.
set -u
ROUNDS=${ROUNDS:-2}; S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"; MAX=16777216
OUT=/root/ivh_tools/replace_exhaust_$(date +%H%M%S); mkdir -p $OUT
log() { echo "$*" | tee -a $OUT/log; }
cs_off() { echo 0 > $S/ivh_cs_head_bail; echo 0 > $S/ivh_cs_head_probe; echo 0 > $S/ivh_cs_owner_fast; echo 0 > $S/ivh_cs_owner_clear; echo 0 > $S/ivh_cs_owner_enable; }
cleanup() { cs_off; echo 32768 > $S/ivh_pv_spin_threshold; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; log "restored defaults"; }
trap cleanup EXIT
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; dmesg -n 1
D0=$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task|BUG:|WARNING:')
set_arm() {
    cs_off
    case $1 in
      D) echo 32768 > $S/ivh_pv_spin_threshold ;;
      F) echo 32768 > $S/ivh_pv_spin_threshold; echo 1 > $S/ivh_cs_owner_enable; echo 1 > $S/ivh_cs_owner_clear; echo 1 > $S/ivh_cs_owner_fast ;;
      X) echo $MAX > $S/ivh_pv_spin_threshold ;;
      B) echo $MAX > $S/ivh_pv_spin_threshold; echo 1 > $S/ivh_cs_owner_enable; echo 1 > $S/ivh_cs_owner_clear
         echo 1 > $S/ivh_cs_owner_fast; echo 1 > $S/ivh_cs_head_probe; echo 1 > $S/ivh_cs_head_bail ;;
    esac
}
i=0
for arm in D F X B B X F D; do
    i=$((i+1)); set_arm $arm
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh >/dev/null
    log "arm $i:$arm thr=$(cat $S/ivh_pv_spin_threshold) fast=$(cat $S/ivh_cs_owner_fast) probe=$(cat $S/ivh_cs_head_probe) bail=$(cat $S/ivh_cs_head_bail)"
    python3 /root/ivh_tools/phase0b_dump.py $OUT/${i}_${arm}.before.json
    for r in $(seq 1 $ROUNDS); do
        v=$(timeout 180 $W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+'); v=${v:-TIMEOUT}
        log "  $arm round $r time=${v}s"; echo "$arm,$i,$r,$v" >> $OUT/times.csv
        [ "$v" = TIMEOUT ] && { log "EARLY EXIT: timeout"; exit 1; }
    done
    python3 /root/ivh_tools/phase0b_dump.py $OUT/${i}_${arm}.after.json
    [ "$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task|BUG:|WARNING:')" != "$D0" ] && { log "EARLY EXIT: kernel warning"; exit 1; }
done
log "done -> $OUT"
