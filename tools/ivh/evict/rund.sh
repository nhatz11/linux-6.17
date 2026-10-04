#!/bin/bash
# Design D differential: does judging staleness from the node's own stamp recover
# the eviction walk's fixed per-handoff cost? Tier 1 OFF in all arms.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel; OUT=$D/designD_$(date +%H%M%S).csv
echo "blk,arm,ops,cap0,cap1,marked,requeued,live" > $OUT
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_marked ivh_evict_requeued 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
setarm(){
  case $1 in
    evict_off)    $D/arm.sh nt1_only >/dev/null || return 1; ns=0 ;;
    evict_percpu) HOPCAP=1 REQMAX=1 $D/arm.sh evict_nt1 >/dev/null || return 1; ns=0 ;;
    evict_node)   HOPCAP=1 REQMAX=1 $D/arm.sh evict_nt1 >/dev/null || return 1; ns=1 ;;
  esac
  echo 0 > $S/ivh_pv_evict_quiet; echo $ns > $S/ivh_pv_evict_node_stamp
  [ "$(cat $S/ivh_pv_evict_node_stamp)" = "$ns" ] && [ "$(cat $S/ivh_pv_evict_quiet)" = 0 ]
}
ARMS=(evict_off evict_percpu evict_node)
for b in $(seq 1 10); do
  n=${#ARMS[@]}; ORDER=(); for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
  for a in "${ORDER[@]}"; do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    sleep 12; C0=$(cap); B=$(ctr)
    R=$(timeout -k 5 90 /root/linux-6.17/qlockbench -t 16 -d 20 -q 2>&1|tail -1)
    C1=$(cap); A=$(ctr); L=$(python3 $D/gauge.py|grep -o 'live=[0-9]*'|cut -d= -f2)
    read ma rq <<< "$(python3 -c "
b='$B'.split(); a='$A'.split(); print(' '.join(str(int(x)-int(y)) for x,y in zip(a,b)))")"
    echo "$b,$a,$R,$C0,$C1,$ma,$rq,$L" >> $OUT
    printf "blk%-2s %-13s ops=%-9s cap %s->%-4s evicted=%-6s gap=%-3s live=%s\n" "$b" "$a" "$R" "$C0" "$C1" "$ma" "$((ma-rq))" "$L"
  done
done
echo 0 > $S/ivh_pv_evict_node_stamp; echo 0 > $S/ivh_pv_evict_quiet; /root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
