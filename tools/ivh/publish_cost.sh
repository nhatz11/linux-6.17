#!/bin/bash
# publish_cost.sh -- is the damage on vips/ebizzy the HALTING or the PUBLISHING?
#
# Observed at 50us: tier2 fired 0 in 10 of 12 vips reps, head-bail and eviction 0
# in all 12, yet AS node iterations ran +79.8%. A mechanism that never fires
# cannot cause that by halting. The remaining difference between the arms is that
# the AS arm has ivh_pv_preempt_src=2, which turns on ivh_node_publish_in_spin()
# -- a stamp store every (mask+1) spin iterations. If that store shares a
# cacheline with the prev->state field the successor polls, every publish
# invalidates the line the next waiter is spinning on.
#
# THREE ARMS isolate it:
#   pv      stock PV                           (no publishing, no AS)
#   pub     stock PV + preempt_src=2           (publishing ON, every AS mechanism OFF)
#   as      full AS @50us                      (publishing ON + mechanisms ON)
# If pub ~= as and both > pv, the cost is the PUBLISH, not the halting.
# The mask sweep then follows directly: 255 publishes 16x more often than 4095.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${REPS:-6}"; MASK="${MASK:-255}"
OUT=/root/ivh_logs/pubcost_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
arm(){ case "$1" in
  pv)  bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       [ "$(cat $S/ivh_pv_preempt_src)" = 0 ] || return 1 ;;
  pub) bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       echo 2 > $S/ivh_pv_preempt_src                 # publishing ON
       echo "$MASK" > $S/ivh_pv_beat_publish_mask
       # every AS consumer stays OFF
       for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe \
                ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
       [ "$(cat $S/ivh_pv_preempt_src)" = 2 ] && [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1 ;;
  as)  IVH_MASK=$MASK bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
       echo 0 > $S/ivh_universal_eligible ;;
  esac; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 9000 9 || { echo FATAL; exit 1; }
printf "wl\tarm\trep\tval\tnode\thead\tentries\tt2f\n" > "$OUT"
echo "### publish_cost mask=$MASK reps=$REPS -> $OUT"
for wl in vips ebizzy; do
 echo "########## $wl ##########"
 case $wl in
  vips)   ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 ) ;;
  ebizzy) ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
 esac
 for rep in $(seq 1 $REPS); do
  case $((rep % 3)) in 1) O="pv pub as";; 2) O="pub as pv";; 0) O="as pv pub";; esac
  for a in $O; do
   arm "$a" || { echo "  ARMFAIL $a"; continue; }
   sync; [ "$wl" = ebizzy ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
   b=($(snap))
   case $wl in
    vips)   t0=$(date +%s%N); ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 ); t1=$(date +%s%N); v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')") ;;
    ebizzy) v=$( cd /root && timeout 180 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 | grep -oP '^\K[0-9]+(?= records/s)' | head -1 ) ;;
   esac
   f=($(snap))
   printf "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\n" "$wl" "$a" "$rep" "${v:-NA}" \
     "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" \
     "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" >> "$OUT"
   echo "  rep$rep $a ${v:-NA} t2f=$(( ${f[5]}-${b[5]} ))"
  done
 done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 - "$OUT" <<'PY'
import sys, statistics as st
NS=26e-9
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
for wl in ('vips','ebizzy'):
    wr=[x for x in r if x[0]==wl and x[3] not in ('NA','')]
    if not wr: continue
    print(f"\n=== {wl}: node+head spin iterations by arm ===")
    base=None
    for a in ('pv','pub','as'):
        v=[(int(x[4])+int(x[5])) for x in wr if x[1]==a]
        t2=[int(x[7]) for x in wr if x[1]==a]
        if not v: continue
        m=st.mean(v)
        if a=='pv': base=m
        print(f"  {a:<4s} n={len(v):2d}  spin {m:12,.0f} iters ({m*NS*1000:6.1f} ms)"
              f"  vs pv {100*(m-base)/base:+7.1f}%   t2 fires {st.mean(t2):8.0f}")
    print("  -> if 'pub' is as bad as 'as', the cost is PUBLISHING, not halting")
PY
echo "PUBCOST_DONE $OUT"
