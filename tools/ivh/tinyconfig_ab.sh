#!/bin/bash
# Re-test the tinyconfig kernel build under migration, interleaved A/B.
#
# The 2026-07-20 campaign measured -11.8% at -j16 and the survey then rejected
# tinyconfig as "below the measurement floor" (~15s). A 15s workload is not
# inherently unmeasurable -- it is unmeasurable at n=3. This runs n=10 PAIRS,
# interleaved, which is what a short workload actually needs.
#
# Both arms run spin_mode 1 (STOCK_PV) so the ONLY variable is migration.
# Build is out-of-tree via O= : the docs record a previous session destroying a
# .config and losing three files by building in-tree. Non-negotiable.
set -u
S=/proc/sys/kernel; T=/root/ivh_tools
TREE=/root/kernels/linux-6.14-stock
BUILD=/root/kernels/_tinyab_build
PAIRS=${PAIRS:-10}; J=${J:-16}
source /root/ivh_tools/bench_guard.sh
OUT=$T/tinyconfig_ab_$(date +%m%d-%H%M%S).csv
echo "pair,arm,seconds,migrations" > $OUT

[ -f "$TREE/.config" ] && { echo "*** $TREE has a .config -- refusing (tree must stay pristine) ***" >&2; exit 1; }
/root/spin_mode 1 > /dev/null    # STOCK_PV for both arms

arm(){ # $1 = on|off
  if [ "$1" = on ]; then echo 1 > $S/ivh_universal_eligible; else echo 0 > $S/ivh_universal_eligible; fi
}
run(){ # one clean build, echoes "seconds migrations"
  rm -rf "$BUILD"; mkdir -p "$BUILD"
  make -C "$TREE" O="$BUILD" tinyconfig > /dev/null 2>&1
  m0=$(python3 $T/migcount.py)
  s=$(date +%s.%N)
  make -C "$TREE" O="$BUILD" -j$J vmlinux > /dev/null 2>&1
  e=$(date +%s.%N)
  m1=$(python3 $T/migcount.py)
  echo "$(echo "$e - $s" | bc) $((m1-m0))"
}

echo "warming caches (one discarded build)..."; run > /dev/null
for p in $(seq $PAIRS); do
  # 2026-09-25: alternate arm order per pair. The old fixed `for a in off on`
  # always ran ON second, against whatever state OFF left behind -- the same
  # flaw that produced a bogus +10.46% on PARSEC freqmine (real value +2.03%).
  if [ $((p % 2)) -eq 1 ]; then ORDER="off on"; else ORDER="on off"; fi
  for a in $ORDER; do
    arm $a
    read sec mig <<< "$(run)"
    echo "$p,$a,$sec,$mig" >> $OUT
    printf "  pair %2s  %-3s  %6.2fs  migrations=%s\n" $p $a $sec $mig
  done
done
rm -rf "$BUILD"
arm on
echo; echo "=== result ==="
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
off=[float(r['seconds']) for r in rows if r['arm']=='off']
on =[float(r['seconds']) for r in rows if r['arm']=='on']
mig=[int(r['migrations']) for r in rows if r['arm']=='on']
migoff=[int(r['migrations']) for r in rows if r['arm']=='off']
d=[(o-n)/o*100 for o,n in zip(off,on)]           # +ve = migration FASTER
print(f"  migration OFF : {st.mean(off):.2f}s +- {st.stdev(off):.2f}  (n={len(off)})")
print(f"  migration ON  : {st.mean(on):.2f}s +- {st.stdev(on):.2f}  (n={len(on)})")
print(f"  paired delta  : {st.mean(d):+.2f}%  (+ve = ON faster)  median {st.median(d):+.2f}%")
wins=sum(1 for x in d if x>0)
print(f"  ON faster in {wins}/{len(d)} pairs")
if len(d)>1:
    se=st.stdev(d)/len(d)**0.5
    print(f"  t = {st.mean(d)/se:+.2f}  (|t|>2.26 is p<0.05 at n=10)")
print(f"\n  migrations fired: ON arm {st.mean(mig):.0f}/build, OFF arm {st.mean(migoff):.0f}/build")
if st.mean(mig) < 1: print("  *** WARNING: migration never fired -- the A/B tested nothing ***")
PY
echo "DONE -> $OUT"
