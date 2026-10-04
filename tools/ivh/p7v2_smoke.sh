#!/bin/bash
set -u
source /root/ivh_tools/suite6.sh
REPS=${REPS:-2}
halt(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_node_halt_cycles 2>/dev/null | grep -oP 'TOTAL\s*\] = \K[0-9]+'; }
ctr(){ python3 /root/ivh_tools/read_ivh_counters.py "$1" 2>/dev/null | awk '{print $NF}'; }
declare -A R
for arm in pv 4000000; do
  bash /root/ivh_tools/p7v2_arm.sh "$arm" || exit 1; sleep 3
  for entry in "${SUITE6[@]}"; do
    IFS='|' read -r name dir kind cmd ext <<< "$entry"
    [ "$cmd" = MEMTIER_CMD ] && cmd="$MEMTIER_CMD"
    for r in $(seq "$REPS"); do
      prep6 "$name" >/dev/null 2>&1 || true
      w0=$(ctr ivh_slowpath_wait_ns); h0=$(halt); t0=$(date +%s.%N)
      raw=$(cd "$dir" && timeout 400 bash -c "$cmd" 2>&1); t1=$(date +%s.%N)
      w1=$(ctr ivh_slowpath_wait_ns); h1=$(halt)
      if [ "$kind" = TIME ] && [ "$ext" = x ]; then v=$(echo "$t1 - $t0" | bc)
      else v=$(echo "$raw" | eval "$ext" | head -1); fi
      R[$arm,$name,$r]="$v"; R[$arm,$name,w$r]="$((w1-w0))"; R[$arm,$name,h$r]="$((h1-h0))"
      echo "  $arm $name rep$r val=$v  wait=$(python3 -c "print(f'{($w1-$w0)/1e9:.3f}')")s  halt=$(python3 -c "print(f'{($h1-$h0)/2.2/1e9:.3f}')")s"
    done
  done
done
echo
echo "================ SMOKE: 4ms migration-only vs PV ================"
printf "%-20s %-11s %-11s %-9s %-9s %-9s %-9s %-7s\n" "bench" "PV" "IVH" "perf" "PVwait" "IVHwait" "waitRed" "B ok?"
for entry in "${SUITE6[@]}"; do
  IFS='|' read -r name dir kind cmd ext <<< "$entry"
  python3 - "$name" "$kind" "${R[pv,$name,1]}" "${R[4000000,$name,1]}" \
            "${R[pv,$name,w1]}" "${R[4000000,$name,w1]}" "${R[pv,$name,h1]}" "${R[4000000,$name,h1]}" <<'PY'
import sys
n,k=sys.argv[1],sys.argv[2]
pv,iv=float(sys.argv[3]),float(sys.argv[4])
pw,iw=float(sys.argv[5])/1e9,float(sys.argv[6])/1e9
ph,ih=float(sys.argv[7])/2.2/1e9,float(sys.argv[8])/2.2/1e9
perf=(pv-iv)/pv*100 if k=="TIME" else (iv-pv)/pv*100
red=(pw-iw)/pw*100 if pw>0 else 0
bok="yes" if (pw>ph and iw>ih) else "NEG"
print(f"{n:<20} {pv:<11.2f} {iv:<11.2f} {perf:>+8.2f}% {pw:<9.3f} {iw:<9.3f} {red:>+8.1f}% {bok:>7}")
PY
done
