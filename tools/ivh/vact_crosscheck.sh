#!/bin/bash
# Cross-check ivh_vact TSC-native preemption detection against the
# independent LOC (local APIC timer tick) ground-truth method.
# Usage: vact_crosscheck.sh <label> <nthreads> <seconds>
set -u
LABEL="$1"; NTHR="$2"; SECS="$3"

snap_loc() { grep "^LOC" /proc/interrupts | awk '{for(i=2;i<=NF-3;i++) print $i}'; }
snap_vact() { python3 /root/ivh_tools/read_vact_rq.py ivh_vact_jumps ivh_vact_idle_explained ivh_vact_last_active_c; }

echo "=== $LABEL: nthreads=$NTHR secs=$SECS ==="
LOC_B=$(snap_loc)
VACT_B=$(snap_vact)
T0=$(date +%s.%N)

/root/ivh_tools/spinner "$NTHR" "$SECS"

T1=$(date +%s.%N)
LOC_A=$(snap_loc)
VACT_A=$(snap_vact)
WALL=$(echo "$T1 - $T0" | bc)

echo "wall=${WALL}s"

# Per-cpu LOC delta and expected-vs-actual tick loss (CONFIG_HZ=1000)
paste <(echo "$LOC_B") <(echo "$LOC_A") | awk -v wall="$WALL" '
{
  d=$2-$1
  expected = wall*1000
  loss = 1 - d/expected
  printf "cpu%02d loc_delta=%d expected=%.0f loss_frac=%.4f\n", NR-1, d, expected, loss
  sum_d+=d; sum_exp+=expected
}
END {
  printf "TOTAL loc_delta=%d expected=%.0f overall_loss_frac=%.4f\n", sum_d, sum_exp, 1-sum_d/sum_exp
}'

echo "--- vact before ---"
echo "$VACT_B"
echo "--- vact after ---"
echo "$VACT_A"
echo
