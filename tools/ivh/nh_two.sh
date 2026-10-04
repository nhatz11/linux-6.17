#!/bin/bash
# two.tsv -- EXP1 (tlt=1ms, post-sleep ON) vs EXP2 (tlt=4ms, post-sleep OFF)
# loop_spin=50000, 10s, 3 reps per arm. This is the -41% / -58% pair.
set -u; cd /root; . /root/ivh_tools/nh_common.sh
OUT=${OUT:-/root/ivh_logs/two.tsv}; : > "$OUT"
nhl(){ env NHEXTEND_POST_SLEEP=$2 IVH_AFL_DISABLE=1 NHEXTEND_DURATION=10 \
       NHEXTEND_LOOP_SPIN=50000 timeout 120 /root/linux-6.17/NHextend-full -l -n 16 2>&1; }
row(){ printf "%s\t%s\t%s\t%s\n" "$1" "$2" "$(g "$3" 'Ran for \K[0-9]+')" \
       "$(g "$3" 'Total wait time: \K[0-9.]+')" >> "$OUT"; }
for r in 1 2 3; do
  setpp(){ :; }
  setpv;          row e1 pv  "$(nhl 1000000 1)"
  setivh 1000000; row e1 ivh "$(nhl 1000000 1)"
  setpv;          row e2 pv  "$(nhl 4000000 0)"
  setivh 4000000; row e2 ivh "$(nhl 4000000 0)"
  echo "  round $r done"
done
