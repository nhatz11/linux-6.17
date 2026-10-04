#!/bin/bash
# WHY DOES A STEAL ATTEMPT EXIT? -- across workloads, on STOCK PV.
# This is the test of the claim "skipping is redundant because a stealer is
# always ready". Instruments pv_hybrid_queued_unfair_trylock()'s exits:
#   exit_win     valve OPEN, stealer took the lock  -> skipping is redundant here
#   exit_pending valve CLOSED by _Q_PENDING         -> the ONLY case skipping could help
#   exit_empty   no queue to jump
# Plus xchg_tail_calls = queue joins, so queue share = xchg_tail / camp_entries.
set -u; S=/proc/sys/kernel; B=/root/linux-6.17; D=/root/ivh_tools/evict
REPS=${REPS:-3}; T=${T:-$(nproc)}; HL=${HL:-100000}; DUR=${DUR:-10}
OUT=$D/stealshare_$(date +%m%d-%H%M%S).csv
echo "rep,wl,metric,camp_entries,camp_win,camp_empty,camp_pending,camp_trips,xt_calls,xt_nonempty" > $OUT
C="ivh_camp_entries ivh_camp_exit_win ivh_camp_exit_empty ivh_camp_exit_pending ivh_camp_trips ivh_xchg_tail_calls ivh_xchg_tail_nonempty"
ctr(){ timeout -k 5 120 python3 /root/ivh_tools/read_ivh_counters.py $C 2>/dev/null|awk '{printf "%s ",$3}'; }
/root/spin_mode 1 >/dev/null 2>&1          # STOCK PV -- the real-world configuration
echo 1 > $S/ivh_pv_camp_probe
run_wl(){
  case $1 in
    qlock)     timeout -k 5 $((DUR+50)) $B/qlockbench -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2 ;;
    hackbench) timeout -k 5 600 hackbench -T -g4 -f8 -l$HL 2>&1|awk '/^Time:/{print $2}' ;;
    dbench)    cd /root/dbench_test 2>/dev/null||cd /root
               timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}' ;;
    ebizzy)    timeout -k 5 180 $B/ebizzy -t $T -S 10 2>&1|awk '/records\/s/{print $1}' ;;
    idle)      sleep $DUR; echo 0 ;;
  esac
}
for r in $(seq 1 $REPS); do
  for wl in qlock hackbench dbench ebizzy; do
    read -r a0 b0 c0 d0 e0 f0 g0 <<< "$(ctr)"
    M=$(run_wl $wl)
    read -r a1 b1 c1 d1 e1 f1 g1 <<< "$(ctr)"
    echo "$r,$wl,${M:-0},$((a1-a0)),$((b1-b0)),$((c1-c0)),$((d1-d0)),$((e1-e0)),$((f1-f0)),$((g1-g0))" >> $OUT
  done
  printf "  rep%-3s done\n" "$r"
done
echo 0 > $S/ivh_pv_camp_probe
echo "DONE -> $OUT"
