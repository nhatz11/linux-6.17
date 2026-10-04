#!/bin/bash
# rej_probe.sh -- how often does the DESTINATION-quality test reject under
# uniform starvation, and is there headroom to make it stricter?
# reject_reasons is a PERCPU_ARRAY (REJ_MAX=12) in MY_ivh_atc; sum across CPUs.
set -u
N=(CPUMASK CLAIMED LOCKHOLDER SPINNER CAPACITY_LOW NOT_BETTER PREEMPTED BURST_ORDER BURST_BUDGET ACC_T1_ACTIVE ACC_T2_IDLE USER_LOCKHOLDER)
dump(){ for k in $(seq 0 11); do
    printf "%s " "$(bpftool map lookup name reject_reasons key hex $(printf '%02x 00 00 00' $k) 2>/dev/null \
      | grep -oP '"values":.*' | grep -oP '0x[0-9a-f]+' | python3 -c "
import sys;print(sum(int(x,16) for x in sys.stdin.read().split()))" 2>/dev/null || echo 0)"
  done; echo; }
bash /root/ivh_tools/pvbase.sh >/dev/null 2>&1
echo 2200000 > /proc/sys/kernel/ivh_cs_tick_period; echo 2 > /proc/sys/kernel/ivh_cs_owed_ticks
bash /root/ivh_tools/p7v2_arm.sh 2500000 >/dev/null 2>&1
for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > /proc/sys/kernel/$k; done
sleep 1
echo "cap_mean=$(awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats)  margin_min=20 margin=1/3 of src..scan_max"
B=($(dump)); M0=$(python3 /root/ivh_tools/migcount.py)
timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1
A=($(dump)); M1=$(python3 /root/ivh_tools/migcount.py)
echo; echo "  migrations landed: $((M1-M0))"
TOT=0; for i in $(seq 0 11); do D=$(( ${A[$i]:-0} - ${B[$i]:-0} )); TOT=$((TOT+D)); done
printf "  %-18s %12s %8s\n" "outcome" "count" "share"
for i in $(seq 0 11); do
  D=$(( ${A[$i]:-0} - ${B[$i]:-0} ))
  [ "$D" -eq 0 ] && continue
  printf "  %-18s %12d %7.2f%%\n" "${N[$i]}" "$D" "$(python3 -c "print(100*$D/max($TOT,1))")"
done
echo "  TOTAL decisions: $TOT"
echo REJ_DONE
