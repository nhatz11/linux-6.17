#!/bin/bash
# dbench_confirm.sh -- confirm the winning dbench config at 16 clients.
#
# WINNER from the 2026-10-02 sweeps: drop -F. Everything else is the shipped
# command (`-t 15 16 -D /root/dbench_test`).
#   disk  -F  : PV 375.4  MIG 371.4  = -1.06%   iowait 45.8%  spin 2.96%
#   disk  noF : PV 1993.5 MIG 2340.3 = +17.39%  iowait 58.7%  spin 2.33%
#   tmpfs -F  : PV 14771  MIG 12156  = -17.71%  iowait  1.0%  spin 6.13%
#   tmpfs noF : losing too
#
# MECHANISM, and it is NOT lock wait: with -F every write is a device
# round-trip, so dbench blocks and leaves vCPUs idle (iowait 58.7%). Migration
# then packs the runnable threads onto the 8 HEALTHY vCPUs (uc_cap ~1024) and
# off the 8 starved ones (uc_cap ~430-500) at no cost, because the workload was
# never using all 16. On tmpfs dbench is CPU-saturated at 1% iowait, the same
# packing halves its parallelism, and it loses 18% -- exactly what ebizzy does.
# This is the "blocking structure predicts the win" rule, not a lock-rate rule.
set -u
T=/root/ivh_tools
exec 9>/var/lock/ivh_clean_check.lock; flock 9 || exit 1   # BLOCKING: queue
REPS="${1:-4}"; THRESH="${THRESH:-2500000}"
OUT=/root/ivh_logs/dbench_confirm_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns"
printf "arm\trep\tmbps\tspin_pct\tiowait_pct\tmigs\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
echo "### dbench CONFIRM: dbench -t 15 16 -D /root/dbench_test (no -F), reps=$REPS, THRESH=$THRESH"
for rep in $(seq 1 $REPS); do
  if [ $((rep % 2)) -eq 1 ]; then ORDER="pv mig"; else ORDER="mig pv"; fi
  for arm in $ORDER; do
    if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1
    else bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1; fi
    sleep 1
    rm -rf /root/dbench_test; mkdir -p /root/dbench_test
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    python3 $T/read_ivh_counters.py $C > /tmp/c0.$$ 2>&1; m0=$(mig)
    vmstat 1 16 > /tmp/cv.$$ 2>&1 & VP=$!
    v=$(timeout 300 dbench -t 15 16 -D /root/dbench_test 2>&1 9>&- | grep -oP 'Throughput\s+\K[0-9.]+' | head -1)
    wait $VP 2>/dev/null
    m1=$(mig); python3 $T/read_ivh_counters.py $C > /tmp/c1.$$ 2>&1
    sp=$(python3 -c "
import re
p=lambda f:{m.group(1):int(m.group(2)) for m in re.finditer(r'^(ivh_\w+)\s*=\s*(\d+)\s*\$',open(f).read(),re.M)}
a,b=p('/tmp/c0.$$'),p('/tmp/c1.$$')
s=(b.get('ivh_slowpath_wait_ns',0)-a.get('ivh_slowpath_wait_ns',0))-(b.get('ivh_slowpath_halt_ns',0)-a.get('ivh_slowpath_halt_ns',0))
print(f'{100*s/(15e9*16):.2f}')")
    io=$(awk 'NR>3{w+=$16;n++} END{if(n)printf "%.1f",w/n}' /tmp/cv.$$)
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$arm" "$rep" "${v:-NA}" "$sp" "${io:-NA}" "$((m1-m0))" >> "$OUT"
    echo "  rep$rep $arm = ${v:-NA} MB/s  spin=${sp}%  iowait=${io}%  migs=$((m1-m0))"
    rm -f /tmp/c0.$$ /tmp/c1.$$ /tmp/cv.$$
  done
done
python3 - "$OUT" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
pv=[float(r[2]) for r in rows if r[0]=='pv'  and r[2] not in('NA','')]
mg=[float(r[2]) for r in rows if r[0]=='mig' and r[2] not in('NA','')]
mi=[int(r[5])   for r in rows if r[0]=='mig']
p,g=st.mean(pv),st.mean(mg)
cvp=100*st.stdev(pv)/p if len(pv)>1 else 0
cvg=100*st.stdev(mg)/g if len(mg)>1 else 0
b=100*(g-p)/p
# paired t over rep-matched pairs
n=min(len(pv),len(mg)); dif=[mg[i]-pv[i] for i in range(n)]
t=(st.mean(dif)/(st.stdev(dif)/n**0.5)) if n>1 and st.stdev(dif)>0 else float('nan')
flag="OK" if max(cvp,cvg)<5 else ("MARGINAL" if max(cvp,cvg)<10 else "DIRTY")
print(f"\n==> PV {p:.1f} (CV {cvp:.1f}%)  MIG {g:.1f} (CV {cvg:.1f}%)  benefit {b:+.2f}%"
      f"  t={t:+.2f} (n={n})  migs {st.mean(mi):.0f}  [{flag}]")
print(f"    PV reps  {[round(x,1) for x in pv]}")
print(f"    MIG reps {[round(x,1) for x in mg]}")
print(f"    documented (eval_final:52, dbench -F, PV 236): +19.1%")
PY
echo "DONE $OUT"
