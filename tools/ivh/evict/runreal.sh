#!/bin/bash
# First clean test of lock skipping: eviction with false evictions removed
# (1 ms threshold) under GENUINE host contention. Tier 1 OFF in both IVH arms.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel; OUT=$D/real_$(date +%H%M%S).csv
echo "blk,arm,ops,capidle,evicted,requeued,live" > $OUT
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_marked ivh_evict_requeued 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
setarm(){
  case $1 in
    pv)        /root/spin_mode 1 >/dev/null 2>&1; ns=0; thr=220000 ;;
    evict_off) $D/arm.sh nt1_only >/dev/null || return 1; ns=0; thr=220000 ;;
    evict_1ms) HOPCAP=1 REQMAX=1 $D/arm.sh evict_nt1 >/dev/null || return 1; ns=1; thr=2200000 ;;
  esac
  echo 0 > $S/ivh_pv_evict_quiet; echo 0 > $S/ivh_pv_evict_age_hist; echo 4095 > $S/ivh_pv_beat_publish_mask
  echo $ns > $S/ivh_pv_evict_node_stamp; echo $thr > $S/ivh_pv_beat_threshold
  [ "$(cat $S/ivh_pv_evict_node_stamp)" = "$ns" ] && [ "$(cat $S/ivh_pv_beat_threshold)" = "$thr" ]
}
ARMS=(pv evict_off evict_1ms)
for b in $(seq 1 12); do
  n=${#ARMS[@]}; ORDER=(); for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
  for a in "${ORDER[@]}"; do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    sleep 8; C=$(cap); B=$(ctr)
    R=$(timeout -k 5 60 /root/linux-6.17/qlockbench -t 16 -d 15 -q 2>&1|tail -1)
    A=$(ctr); L=$(python3 $D/gauge.py|grep -o 'live=[0-9]*'|cut -d= -f2)
    read ma rq <<< "$(python3 -c "b='$B'.split(); a='$A'.split(); print(int(a[0])-int(b[0]), int(a[1])-int(b[1]))")"
    echo "$b,$a,$R,$C,$ma,$rq,$L" >> $OUT
    printf "blk%-2s %-10s ops=%-9s idlecap=%-4s evicted=%-6s gap=%-3s live=%s\n" "$b" "$a" "$R" "$C" "$ma" "$((ma-rq))" "$L"
  done
done
echo 220000 > $S/ivh_pv_beat_threshold; echo 0 > $S/ivh_pv_evict_node_stamp; /root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
