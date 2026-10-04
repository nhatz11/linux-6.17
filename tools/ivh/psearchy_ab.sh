#!/bin/bash
# psearchy (MOSBench pedsort) under migration, interleaved A/B.
# Metric: the "throughput: N jobs/hour/core" line pedsort prints -- higher better.
# Both arms run spin_mode 1 (STOCK_PV) so ivh_universal_eligible is the ONLY variable.
set -u
S=/proc/sys/kernel; T=/root/ivh_tools; P=/root/mosbench/psearchy
PAIRS=${PAIRS:-8}; CORES=${CORES:-16}; MEM=${MEM:-512}; LIST=${LIST:-files_6x}
source /root/ivh_tools/bench_guard.sh
OUT=$T/psearchy_ab_$(date +%m%d-%H%M%S).csv
echo "pair,arm,throughput,seconds,migrations" > $OUT
/root/spin_mode 1 > /dev/null

arm(){ [ "$1" = on ] && echo 1 > $S/ivh_universal_eligible || echo 0 > $S/ivh_universal_eligible; }
run(){
  rm -rf /root/psearchy_db; for i in $(seq 0 $((CORES-1))); do mkdir -p /root/psearchy_db/db$i; done
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
  m0=$(python3 $T/migcount.py); s=$(date +%s.%N)
  (cd $P && timeout 1200 ./mkdb/pedsort -t /root/psearchy_db/db -c $CORES -m $MEM < $LIST) > /tmp/psab.log 2>&1
  e=$(date +%s.%N); m1=$(python3 $T/migcount.py)
  tp=$(grep -o "throughput: [0-9.]*" /tmp/psab.log | tail -1 | awk '{print $2}')
  echo "${tp:-0} $(echo "$e - $s" | bc) $((m1-m0))"
}
echo "warmup (discarded)..."; run > /dev/null
for p in $(seq $PAIRS); do
  # 2026-09-25: alternate arm order per pair (see tinyconfig_ab.sh comment).
  if [ $((p % 2)) -eq 1 ]; then ORDER="off on"; else ORDER="on off"; fi
  for a in $ORDER; do
    arm $a; read tp sec mig <<< "$(run)"
    echo "$p,$a,$tp,$sec,$mig" >> $OUT
    printf "  pair %2s  %-3s  %8s jobs/hour/core  %6.1fs  migrations=%s\n" $p $a "$tp" "$sec" "$mig"
  done
done
arm on
echo; echo "=== result (throughput: higher is better) ==="
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
r=list(csv.DictReader(open(sys.argv[1])))
off=[float(x['throughput']) for x in r if x['arm']=='off']
on =[float(x['throughput']) for x in r if x['arm']=='on']
mig=[int(x['migrations'])  for x in r if x['arm']=='on']
d=[(n-o)/o*100 for o,n in zip(off,on)]
print(f"  migration OFF : {st.mean(off):.3f} +- {st.stdev(off):.3f} jobs/hour/core")
print(f"  migration ON  : {st.mean(on):.3f} +- {st.stdev(on):.3f}")
print(f"  paired delta  : {st.mean(d):+.2f}%   median {st.median(d):+.2f}%   ON better in {sum(1 for x in d if x>0)}/{len(d)}")
if len(d)>1:
    se=st.stdev(d)/len(d)**0.5
    print(f"  t = {st.mean(d)/se:+.2f}")
print(f"  migrations fired: {st.mean(mig):.0f}/run (ON arm)")
if st.mean(mig)<1: print("  *** migration never fired -- A/B tested nothing ***")
PY
echo "DONE"
