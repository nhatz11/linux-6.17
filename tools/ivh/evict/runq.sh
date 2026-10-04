#!/bin/bash
set -u; D=/root/ivh_tools/evict; OUT=$D/qlock_$(date +%H%M%S).csv
echo "blk,arm,ops,cap0,cap1,acted,stop_halted,gap,live" > $OUT
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_walks_acted ivh_evict_stop_halted ivh_evict_marked ivh_evict_requeued 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
ARMS=(pv evict evict_nt1 nt1_only)
for b in $(seq 1 8); do
  n=${#ARMS[@]}; ORDER=(); for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
  for a in "${ORDER[@]}"; do
    $D/arm.sh $a >/dev/null || { echo "ARM FAIL $a"; exit 1; }
    sleep 12; C0=$(cap); B=$(ctr)
    R=$(timeout -k 5 90 /root/linux-6.17/qlockbench -t 16 -d 20 -q 2>&1|tail -1)
    C1=$(cap); A=$(ctr); L=$(python3 $D/gauge.py|grep -o 'live=[0-9]*'|cut -d= -f2)
    read ac sh ma rq <<< "$(python3 -c "
b='$B'.split(); a='$A'.split(); print(' '.join(str(int(x)-int(y)) for x,y in zip(a,b)))")"
    echo "$b,$a,$R,$C0,$C1,$ac,$sh,$((ma-rq)),$L" >> $OUT
    printf "blk%-2s %-10s ops=%-9s cap %s->%-4s acted=%-7s halted=%-7s gap=%s live=%s\n" "$b" "$a" "$R" "$C0" "$C1" "$ac" "$sh" "$((ma-rq))" "$L"
  done
done
/root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
