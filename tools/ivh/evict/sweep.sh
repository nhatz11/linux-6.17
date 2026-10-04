#!/bin/bash
# Dial the eviction RATE via ivh_pv_beat_threshold (live sysctl) and see whether
# throughput loss scales linearly with evictions (per-event cost) or faster
# (contention amplification). All arms have tier1 OFF so eviction is unblocked.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel; OUT=$D/sweep_$(date +%H%M%S).csv
echo "blk,arm,thr,ops,cap,acted,steal_ok,requeued,stop_halted,spin_iters,spin_attempts" > $OUT
C="ivh_evict_walks_acted ivh_evict_steal_ok ivh_evict_requeued ivh_evict_stop_halted ivh_node_spin_iters_sum ivh_node_spin_attempts"
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py $C 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
THRS=(55000 220000 660000 2200000 6600000)
for b in 1 2 3; do
  ARMS=(none 55000 220000 660000 2200000 6600000)
  m=${#ARMS[@]}; ORDER=(); for i in $(seq 0 $((m-1))); do ORDER+=("${ARMS[$(( (i + b) % m ))]}"); done
  for a in "${ORDER[@]}"; do
    if [ "$a" = none ]; then $D/arm.sh nt1_only >/dev/null; TH=0
    else $D/arm.sh evict_nt1 >/dev/null; echo "$a" > $S/ivh_pv_beat_threshold; TH=$a; fi
    sleep 12; C0=$(cap); B=$(ctr)
    R=$(timeout -k 5 90 /root/linux-6.17/qlockbench -t 16 -d 20 -q 2>&1|tail -1)
    A=$(ctr)
    read ac st rq sh si sa <<< "$(python3 -c "
b='$B'.split(); a='$A'.split(); print(' '.join(str(int(x)-int(y)) for x,y in zip(a,b)))")"
    echo "$b,$a,$TH,$R,$C0,$ac,$st,$rq,$sh,$si,$sa" >> $OUT
    printf "blk%-2s thr=%-9s ops=%-9s cap=%-4s acted=%-7s steal=%-7s halted=%-7s\n" "$b" "$a" "$R" "$C0" "$ac" "$st" "$sh"
  done
done
echo 220000 > $S/ivh_pv_beat_threshold; /root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
