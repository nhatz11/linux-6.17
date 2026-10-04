#!/bin/bash
# TRUE stock-PV baseline vs the fixed IVH config. Every arm verifies its
# sysctls by readback -- arm D of the 2026-09-27 tierab.sh run silently
# inherited tier1_enable=0 and so was PV with upstream's pv_wait_early()
# deleted, not stock PV.
S=/proc/sys/kernel
echo 0 > $S/ivh_tks_sampler_ns          # MANDATORY for throughput
R="python3 /root/ivh_tools/read_ivh_counters.py"
setarm(){
  case $1 in
    PV)  echo 0 > $S/ivh_adaptive_mode; echo 1 > $S/ivh_pv_tier1_enable
         echo 1 > $S/ivh_pv_tier2_enable;;                 # t2 inert at mode 0
    IVH) echo 2 > $S/ivh_adaptive_mode; echo 1 > $S/ivh_pv_tier1_enable
         echo 1 > $S/ivh_pv_tier2_enable; echo 11000000 > $S/ivh_pv_beat_threshold;;
  esac
}
run(){
  setarm $1; sleep 1
  local m=$(cat $S/ivh_adaptive_mode) t1=$(cat $S/ivh_pv_tier1_enable) t2=$(cat $S/ivh_pv_tier2_enable)
  # hard gate: refuse to report an arm that did not take
  if [ "$1" = "PV" ]  && { [ "$m" != "0" ] || [ "$t1" != "1" ]; }; then echo "$1 ARM-FAILED m=$m t1=$t1"; return; fi
  if [ "$1" = "IVH" ] && { [ "$m" != "2" ] || [ "$t1" != "1" ] || [ "$t2" != "1" ]; }; then echo "$1 ARM-FAILED"; return; fi
  local t0=$(date +%s%N); hackbench -T -g1 -f8 -l100000 >/dev/null 2>&1; local t1s=$(date +%s%N)
  printf "%-4s %.3f  [mode=%s t1=%s t2=%s thr=%s]\n" "$1" "$(python3 -c "print(($t1s-$t0)/1e9)")" \
     "$m" "$t1" "$t2" "$(cat $S/ivh_pv_beat_threshold)"
}
echo "stock PV vs IVH(tier1+tier2, threshold=5ms) -- alternating, n=5"
for r in 1 2 3 4 5; do
  if [ $((r%2)) -eq 1 ]; then run PV; run IVH; else run IVH; run PV; fi
done
setarm IVH
echo PVIVH-DONE
