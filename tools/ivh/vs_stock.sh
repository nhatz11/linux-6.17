#!/bin/bash
# vs_stock.sh -- is csmin+stamp@acquire better than the PLAIN SPINLOCK baseline?
#
# For a USERSPACE lock, "stock" is the unadaptive spinlock, two ways:
#   nh3    NHextend3, the original benchmark binary, no adaptive lock at all
#   spin   NHextend-fin with IVH_AFL_DISABLE=1 -- the header says its unbounded
#          lfence spin "matches NHextend3's original busy-wait" (:679), so this
#          is the same baseline reached through the new code path
# against:
#   fin    NHextend-fin, AFL on: in-CS heartbeat, sleep on 50us staleness
#   csmin  NHextend-csmin, AFL on: stamp ONCE at acquire, sleep on
#          elapsed CS > cmin + 10us
#
# Fixed for all four: stock-PV kernel (no migration), base_slice 2.8ms,
# loop_spin 600000, 16 threads, 8s. Rotated so position cannot alias on an arm.
set -u
R="${1:-5}"
OUT=/root/ivh_logs/vsstock_$(date +%m%d-%H%M%S).tsv
exec 9>/var/lock/ivh_clean_check.lock
flock -w 1800 9 || { echo "FATAL: bench lock"; exit 1; }
bash /root/ivh_tools/pvbase.sh >/dev/null 2>&1
echo "### stock PV: tier2=$(cat /proc/sys/kernel/ivh_pv_tier2_enable) univ=$(cat /proc/sys/kernel/ivh_universal_eligible)  base_slice=$(cat /sys/kernel/debug/sched/base_slice_ns)"
printf "arm\trep\tops\twait_s\n" > "$OUT"
ARMS="nh3 spin fin csmin"
for rep in $(seq 1 "$R"); do
  ORD=$(python3 -c "a='''$ARMS'''.split(); k=($rep-1)%len(a); print(' '.join(a[k:]+a[:k]))")
  for a in $ORD; do
    case $a in
      nh3)   E=""                    B=/root/linux-6.17/NHextend3 ;;
      spin)  E="IVH_AFL_DISABLE=1"   B=/root/linux-6.17/NHextend-fin ;;
      fin)   E=""                    B=/root/linux-6.17/NHextend-fin ;;
      csmin) E=""                    B=/root/linux-6.17/NHextend-csmin ;;
    esac
    o=$(timeout 60 env $E NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 "$B" -l -n 16 2>&1)
    ops=$(printf '%s' "$o" | grep -oP 'Ran for \K[0-9]+')
    w=$(printf '%s' "$o" | grep -oP 'Total wait time: \K[0-9.]+')
    printf "%s\t%s\t%s\t%s\n" "$a" "$rep" "${ops:-NA}" "${w:-NA}" >> "$OUT"
    echo "  rep$rep $a ops=${ops:-NA}"
  done
done
echo "VSSTOCK_DONE $OUT"
