#!/bin/bash
# Optimised G-LOCK-33 (#56): is eviction still a regression once its entry cost is cut?
# Tier 1 is OFF in all three IVH arms, so the only variables are eviction and the quiet gate.
set -u; D=/root/ivh_tools/evict; S=/proc/sys/kernel; OUT=$D/opt_$(date +%H%M%S).csv
echo "blk,arm,ops,cap0,cap1,marked,requeued,steal_ok,live" > $OUT
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_marked ivh_evict_requeued ivh_evict_steal_ok 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }
setarm(){
  case $1 in
    pv)          $D/arm.sh pv ;;
    evict_off)   $D/arm.sh nt1_only ;;
    evict_q0)    $D/arm.sh evict_nt1 && echo 0 > $S/ivh_pv_evict_quiet ;;
    evict_quiet) $D/arm.sh evict_nt1 && echo 2200000 > $S/ivh_pv_evict_quiet ;;
  esac >/dev/null || return 1
  [ "$1" != evict_quiet ] && echo 0 > $S/ivh_pv_evict_quiet
  return 0
}
ARMS=(pv evict_off evict_q0 evict_quiet)
for b in $(seq 1 5); do
  n=${#ARMS[@]}; ORDER=(); for i in $(seq 0 $((n-1))); do ORDER+=("${ARMS[$(( (i + b) % n ))]}"); done
  for a in "${ORDER[@]}"; do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    sleep 12; C0=$(cap); B=$(ctr)
    R=$(timeout -k 5 90 /root/linux-6.17/qlockbench -t 16 -d 20 -q 2>&1|tail -1)
    C1=$(cap); A=$(ctr); L=$(python3 $D/gauge.py|grep -o 'live=[0-9]*'|cut -d= -f2)
    read ma rq st <<< "$(python3 -c "
b='$B'.split(); a='$A'.split(); print(' '.join(str(int(x)-int(y)) for x,y in zip(a,b)))")"
    echo "$b,$a,$R,$C0,$C1,$ma,$rq,$st,$L" >> $OUT
    printf "blk%-2s %-12s ops=%-9s cap %s->%-4s marked=%-6s gap=%-3s live=%s\n" "$b" "$a" "$R" "$C0" "$C1" "$ma" "$((ma-rq))" "$L"
  done
done
echo 0 > $S/ivh_pv_evict_quiet; /root/spin_mode 1 >/dev/null 2>&1; echo "DONE -> $OUT"
