#!/bin/bash
# PV vs EVICT, ABBA-alternating, every run hard-bounded by timeout.
set -u
D=/root/ivh_tools/evict
L=${L:-100000}; TMO=${TMO:-200}; BLKS=${BLKS:-15}; OUT=${OUT:-$D/ab_$(date +%H%M%S).csv}
echo "blk,pos,arm,secs,cap0,cap1,lockups,strand,averted" > "$OUT"
av(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_halt_averted 2>/dev/null|awk '{print $3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
for b in $(seq 1 $BLKS); do
  [ $((b%2)) -eq 1 ] && O="pv evict" || O="evict pv"
  p=0
  for a in $O; do p=$((p+1))
    $D/arm.sh $a || exit 1
    sleep 12
    C0=$(cap); K0=$(dmesg|grep -cE "watchdog: BUG"); A0=$(av)
    T=$( { /usr/bin/time -f %e timeout -k 5 $TMO hackbench -T -g1 -f8 -l$L >/dev/null; } 2>&1|tail -1 )
    sleep 4
    S=$(ps -eo comm|grep -cE '^hackbench$'); K1=$(dmesg|grep -cE "watchdog: BUG"); C1=$(cap); A1=$(av)
    echo "$b,$p,$a,$T,$C0,$C1,$((K1-K0)),$S,$((A1-A0))" >> "$OUT"
    printf "blk%-3s %-6s %-7s cap %s->%s lk+%s strand=%s avert+%s\n" "$b" "$a" "$T" "$C0" "$C1" "$((K1-K0))" "$S" "$((A1-A0))"
    if [ "$S" -gt 0 ]; then echo "  !! STRAND SUSPECTED - aborting"; /root/spin_mode 1>/dev/null 2>&1; exit 2; fi
  done
done
/root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
