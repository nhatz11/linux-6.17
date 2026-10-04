#!/bin/bash
# nh_migecon.sh -- migration economics vs CS length for NHextend-full.
# Per loop_spin: migrations, cost (decide->attached), delay (attached->running),
# wait saved vs PV, and (cost+delay)/saved.
set -u
S=/proc/sys/kernel
SC=${SC:-/tmp/claude-0/-root-linux-6-17/6a9e0182-bc62-480b-bb98-dae04f6bb698/scratchpad}
OUT=${OUT:-$SC/migecon.tsv}; : > "$OUT"
DUR=${DUR:-10}
SPINS=${SPINS:-"5000 25000 50000 100000 200000 300000 600000"}

nhl(){ env IVH_AFL_DISABLE=1 NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$1 \
       timeout 180 /root/linux-6.17/NHextend-full -l -n 16 2>&1; }
setpv(){ bash /root/ivh_tools/p7v2_arm.sh pv >/dev/null; echo 0 > $S/ivh_cap_writer; echo 0 > $S/ivh_act_writer; sleep 1; }
setivh(){ bash /root/ivh_tools/p7v2_arm.sh 4000000 >/dev/null; echo 0 > $S/ivh_cap_writer; echo 0 > $S/ivh_act_writer; echo 1 > $S/ivh_time_left_source; sleep 1; }
g(){ echo "$1" | grep -oP "$2" | head -1; }

for sp in $SPINS; do
  setpv; o=$(nhl $sp)
  pvi=$(g "$o" 'Ran for \K[0-9]+'); pvw=$(g "$o" 'Total wait time: \K[0-9.]+')
  setivh
  timeout $((DUR+14)) bpftrace /root/ivh_tools/migtime.bt > $SC/mt_$sp.out 2>&1 &
  BT=$!; sleep 3
  o=$(nhl $sp)
  sleep 2; kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
  ivi=$(g "$o" 'Ran for \K[0-9]+'); ivw=$(g "$o" 'Total wait time: \K[0-9.]+')
  nhm=$(g "$o" 'Total migrations\s+: \K[0-9]+')
  f(){ grep -oP "$1: \K[0-9]+" $SC/mt_$sp.out | head -1; }
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$sp" "$pvi" "$pvw" "$ivi" "$ivw" \
    "${nhm:-0}" "$(f @n_mig)" "$(f @total_sum_ns)" "$(f @delay_sum_ns)" "$(f @n_run)" >> "$OUT"
  echo "  loop_spin=$sp done"
done
python3 /root/ivh_tools/nh_migecon_report.py "$OUT"
