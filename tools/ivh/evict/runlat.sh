#!/bin/bash
# STEP 1: does successor eviction reduce the >1ms LHP stall count?
#
# Every prior eviction A/B scored mean throughput, which the steal valve
# saturates regardless -- so eviction could only ever look like cost. This
# scores the TAIL, where a queue-reordering mechanism would actually show up.
#
# Two defects in runreal.sh are fixed here:
#  1. runreal.sh moved beat_threshold AND evict_enable between evict_off and
#     evict_1ms, so the result could not separate "eviction helps" from "a 10x
#     staleness threshold helps". Here thr/node_stamp are IDENTICAL in both
#     eviction arms; ONLY ivh_pv_evict_enable differs.
#  2. No provenance was recorded, so a sign flip between sessions could not be
#     investigated. Full sysctl dump + kernel build + commit sha go to .meta.
#
# ivh_pv_rot_probe=1 in every arm enables ivh_rot_steals, which counts steals
# taken WHILE A QUEUE EXISTED -- i.e. a dead head's cost already being
# recovered by the steal valve. Constant across arms, so it cancels.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-15}
OUT=$D/lat_$(date +%H%M%S).csv; META=${OUT%.csv}.meta

{ echo "kernel: $(uname -r)"; echo "build: $(cat /proc/version)"
  echo "docs_sha: $(git -C /root/linux-6.17 rev-parse --short HEAD 2>/dev/null)"
  echo "kern_sha: $(git -C /root/kernels/linux-6.17-vanilla rev-parse --short HEAD 2>/dev/null)"
  echo "blocks=$BLOCKS dur=$DUR"; echo "--- sysctls at start ---"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META

echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,cap,marked,requeued,steals,live" > $OUT
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_marked ivh_evict_requeued ivh_rot_steals 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }

setarm(){
  case $1 in
    pv)        /root/spin_mode 1 >/dev/null 2>&1 ;;
    evict_off) $D/arm.sh nt1_only  >/dev/null || return 1 ;;
    evict)     HOPCAP=1 REQMAX=1 $D/arm.sh evict_nt1 >/dev/null || return 1 ;;
  esac
  # Identical in ALL arms. Only evict_enable (set by arm.sh above) differs.
  echo 0 > $S/ivh_pv_evict_quiet; echo 0 > $S/ivh_pv_evict_age_hist
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 1 > $S/ivh_pv_evict_node_stamp
  echo 2200000 > $S/ivh_pv_beat_threshold; echo 1 > $S/ivh_pv_rot_probe
  [ "$(cat $S/ivh_pv_beat_threshold)" = 2200000 ] &&
  [ "$(cat $S/ivh_pv_rot_probe)" = 1 ] &&
  [ "$(cat $S/ivh_pv_evict_node_stamp)" = 1 ]
}

ARMS=(pv evict_off evict)
for b in $(seq 1 $BLOCKS); do
  n=${#ARMS[@]}; ORDER=()
  for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
  for a in "${ORDER[@]}"; do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    sleep 8; C=$(cap); B=$(ctr)
    R=$(timeout -k 5 $((DUR + 45)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    A=$(ctr); L=$(python3 $D/gauge.py|grep -o 'live=[0-9]*'|cut -d= -f2)
    read ma rq st <<< "$(python3 -c "
b='$B'.split(); a='$A'.split()
print(' '.join(str(int(a[i])-int(b[i])) for i in range(3)))")"
    echo "$b,$a,$R,$C,$ma,$rq,$st,$L" >> $OUT
    IFS=, read -r o it hit p50 p99 p999 p9999 mx ov <<< "$R"
    printf "blk%-2s %-10s ops=%-8s iters=%-8s hit=%s p99.9=%-8s >1ms=%-7s steals=%-9s ev=%s\n" \
           "$b" "$a" "$o" "$it" "$hit" "$p999" "$ov" "$st" "$ma"
  done
done
echo 220000 > $S/ivh_pv_beat_threshold; echo 0 > $S/ivh_pv_evict_node_stamp
echo 0 > $S/ivh_pv_rot_probe; /root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT  (meta: $META)"
