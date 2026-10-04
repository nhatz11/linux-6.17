#!/bin/bash
# ebizzy -m (mmap), full contention: PV vs IVH vs IVH-skip. Higher records/s is better.
#   PV   = migration OFF, spin_mode 1 (stock PV qspinlock)
#   IVH  = migration ON, spin_mode 2: tier1 + tier2 + exhaustion (32768), no skipping
#   SKIP = migration ON, tier1 + exhaustion (32768) + lock skipping, NO tier2
#          (adaptive_mode 0 disables tier2; preempt_src 2 keeps heartbeats for skipping)
# Order PV IVH SKIP SKIP IVH PV, ROUNDS each, capacity-settled wait before each arm.
set -u
ROUNDS=${ROUNDS:-3}; S=/proc/sys/kernel
CMD="/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304"
OUT=/root/ivh_tools/ebizzy_skip_$(date +%H%M%S); mkdir -p $OUT
log() { echo "$*" | tee -a $OUT/log; }
ROT="ivh_rot_splice_ok ivh_rot_splice_done ivh_rot_splice_blocked_tail ivh_rot_splice_blocked_starve"
rot() { python3 /root/ivh_tools/read_ivh_counters.py $ROT | awk '{printf "%s ", $3}'; }
cleanup() { echo 0 > $S/ivh_pv_rot_enable; echo 32768 > $S/ivh_pv_spin_threshold; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; log "restored IVH+AS defaults"; }
trap cleanup EXIT
for f in ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_owner_fast ivh_cs_head_probe ivh_cs_head_bail ivh_pv_rot_probe; do echo 0 > $S/$f 2>/dev/null; done
dmesg -n 1; D0=$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task|BUG:|WARNING:')
set_arm() {
    echo 0 > $S/ivh_pv_rot_enable
    case $1 in
      PV)   echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null ;;
      IVH)  echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null ;;
      SKIP) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
            echo 0 > $S/ivh_adaptive_mode; echo 1 > $S/ivh_pv_tier1_enable; echo 2 > $S/ivh_pv_preempt_src
            echo 1 > $S/ivh_pv_rot_enable ;;
    esac
    echo 32768 > $S/ivh_pv_spin_threshold
}
show() { for f in ivh_universal_eligible ivh_adaptive_mode ivh_pv_tier1_enable ivh_pv_preempt_src ivh_pv_beat_threshold ivh_pv_spin_threshold ivh_pv_rot_enable; do printf "%s=%s " ${f#ivh_} "$(cat $S/$f)"; done; }
i=0
for arm in PV IVH SKIP SKIP IVH PV; do
    i=$((i+1)); set_arm $arm
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh >/dev/null
    log "arm $i:$arm  $(show)"
    r0=$(rot)
    for r in $(seq 1 $ROUNDS); do
        v=$(cd /root && timeout 60 $CMD 2>&1 | grep -oP '^\K[0-9]+(?= records/s)'); v=${v:-FAIL}
        log "  $arm round $r records/s=$v"; echo "$arm,$i,$r,$v" >> $OUT/results.csv
    done
    log "  rot counters before: $r0 | after: $(rot)  (splice_ok splice_done blocked_tail blocked_starve)"
    [ "$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task|BUG:|WARNING:')" != "$D0" ] && { log "EARLY EXIT: kernel warning"; exit 1; }
done
log "done -> $OUT"
