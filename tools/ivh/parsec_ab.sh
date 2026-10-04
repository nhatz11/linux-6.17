#!/bin/bash
# PARSEC under migration, interleaved A/B. Metric: wall seconds (lower better).
# Both arms spin_mode 1 (STOCK_PV) so ivh_universal_eligible is the ONLY variable.
# Uses parsecmgmt so input extraction/teardown is identical in both arms.
set -u
S=/proc/sys/kernel; T=/root/ivh_tools
export PARSECDIR=/root/parsec-benchmark
cd $PARSECDIR
PKGS=${PKGS:-swaptions}; PAIRS=${PAIRS:-6}; NTH=${NTH:-16}; INPUT=${INPUT:-native}; CFG=${CFG:-gcc}
source /root/ivh_tools/bench_guard.sh
OUT=$T/parsec_ab_$(date +%m%d-%H%M%S).csv
echo "pkg,pair,arm,seconds,migrations" > $OUT
/root/spin_mode 1 > /dev/null
arm(){ [ "$1" = on ] && echo 1 > $S/ivh_universal_eligible || echo 0 > $S/ivh_universal_eligible; }
run1(){ # $1 pkg -> "seconds migrations"
  # Drop caches before EVERY run. Without this the second arm of each pair reads
  # a page cache warmed by the first, which on an I/O-heavy package (dedup reads
  # a large ISO) dwarfs any scheduling effect -- the first version of this
  # harness produced a bogus +88% on dedup that way.
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
  m0=$(python3 $T/migcount.py); s=$(date +%s.%N)
  timeout 2400 ./bin/parsecmgmt -a run -p $1 -c $CFG -i $INPUT -n $NTH > /tmp/parsec_run.log 2>&1
  e=$(date +%s.%N); m1=$(python3 $T/migcount.py)
  echo "$(echo "$e - $s" | bc) $((m1-m0))"
}
for pkg in $PKGS; do
  echo "### $pkg"
  arm off; run1 $pkg > /dev/null    # warmup, discarded
  for p in $(seq $PAIRS); do
    # Alternate arm order every pair so neither arm is systematically second.
    if [ $((p % 2)) -eq 1 ]; then ORDER="off on"; else ORDER="on off"; fi
    for a in $ORDER; do
      arm $a; read sec mig <<< "$(run1 $pkg)"
      echo "$pkg,$p,$a,$sec,$mig" >> $OUT
      printf "  %-14s pair %s %-3s %8.2fs  migrations=%s\n" "$pkg" "$p" "$a" "$sec" "$mig"
    done
  done
done
arm on
echo; echo "=== results (seconds: LOWER is better) ==="
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
from collections import defaultdict
rows=list(csv.DictReader(open(sys.argv[1])))
by=defaultdict(lambda: defaultdict(list)); mg=defaultdict(list)
for r in rows:
    by[r['pkg']][r['arm']].append(float(r['seconds']))
    if r['arm']=='on': mg[r['pkg']].append(int(r['migrations']))
for pkg,d in by.items():
    off,on=d['off'],d['on']
    if not off or not on: continue
    delta=[(o-n)/o*100 for o,n in zip(off,on)]   # +ve = ON faster
    line=f"  {pkg:<14} off {st.mean(off):7.2f}s  on {st.mean(on):7.2f}s  delta {st.mean(delta):+6.2f}%  ON faster {sum(1 for x in delta if x>0)}/{len(delta)}"
    if len(delta)>1:
        se=st.stdev(delta)/len(delta)**0.5
        line+=f"  t={st.mean(delta)/se:+6.2f}"
    line+=f"  mig={st.mean(mg[pkg]):.0f}/run"
    print(line)
PY
echo "PARSEC-DONE"
