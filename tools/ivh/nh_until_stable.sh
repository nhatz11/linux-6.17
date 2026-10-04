#!/bin/bash
# nh_until_stable.sh -- keep running NHextend-fin pv/mig pairs until the RUNNING
# MEAN of migrations/run settles near TARGET (default 8000).
#
# HONESTY NOTE: stopping when a statistic reaches a target is OPTIONAL STOPPING
# and biases that statistic. So this prints the FULL per-rep series, and the
# summary reports mean/median/CV over ALL reps -- not just the stopped value --
# so the bias is visible rather than hidden. A hard cap guarantees termination.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
THRESH="${THRESH:-1500000}"; TARGET="${TARGET:-8000}"; TOL="${TOL:-0.15}"
MINREP="${MINREP:-6}"; MAXREP="${MAXREP:-20}"
CMD='IVH_AFL_DISABLE=1 NHEXTEND_MIDSPIN_ITERS=10000 NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 /root/linux-6.17/NHextend-fin -l -n 16'
OUT=/root/ivh_logs/nh_stable_$(date +%m%d-%H%M%S).tsv
printf "rep\tarm\titers\tmigs\tuwait\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
echo "### THRESH=$THRESH  target migs/run ~${TARGET} (+/-$(python3 -c "print(int($TOL*100))")%)  min=$MINREP max=$MAXREP -> $OUT"
for rep in $(seq 1 $MAXREP); do
  [ $((rep % 2)) -eq 1 ] && ORDER="pv mig" || ORDER="mig pv"
  for arm in $ORDER; do
    if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1
    else bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1; fi
    sleep 1
    m0=$(mig)
    out=$(timeout 300 bash -c "$CMD" 2>&1 9>&-)
    m1=$(mig)
    it=$(echo "$out" | grep -oP 'Ran for \K[0-9]+' | head -1)
    uw=$(echo "$out" | grep -oP 'Total wait time: \K[0-9.]+' | head -1)
    printf "%s\t%s\t%s\t%s\t%s\n" "$rep" "$arm" "${it:-NA}" "$((m1-m0))" "${uw:-NA}" >> "$OUT"
    [ "$arm" = mig ] && echo "  rep$rep mig  iters=${it:-NA}  migs=$((m1-m0))  uwait=${uw:-NA}" \
                     || echo "  rep$rep pv   iters=${it:-NA}  uwait=${uw:-NA}"
  done
  # running mean of migration counts so far
  read n mean <<<"$(awk -F'\t' '$2=="mig"{s+=$4;c++} END{printf "%d %.0f", c, (c?s/c:0)}' "$OUT")"
  echo "      running mean migs = $mean  over n=$n"
  if [ "$n" -ge "$MINREP" ]; then
    ok=$(python3 -c "print(1 if abs($mean-$TARGET)/$TARGET <= $TOL else 0)")
    [ "$ok" = 1 ] && { echo "  >>> running mean within tolerance of $TARGET at n=$n; stopping"; break; }
  fi
done
echo "=== SUMMARY (all reps, not just the stopping point) ==="
python3 - "$OUT" "$THRESH" <<'PY'
import sys,statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
pv=[x for x in r if x[1]=='pv']; mg=[x for x in r if x[1]=='mig']
n=min(len(pv),len(mg))
ipv=[float(x[2]) for x in pv[:n]]; img=[float(x[2]) for x in mg[:n]]
upv=[float(x[4]) for x in pv[:n]]; umg=[float(x[4]) for x in mg[:n]]
mi=[int(x[3]) for x in mg[:n]]
d=[img[i]-ipv[i] for i in range(n)]
t=st.mean(d)/(st.stdev(d)/n**0.5) if n>1 and st.stdev(d)>0 else float('nan')
crit={2:12.71,3:4.30,4:3.18,5:2.78,6:2.57,7:2.45,8:2.36,9:2.31,10:2.26}.get(n,2.1)
norm=st.mean(img)/st.mean(ipv); saved=st.mean(upv)*norm-st.mean(umg)
print(f"n={n} pairs   THRESH={int(sys.argv[2])/1000:.0f} us")
print(f"  migrations  mean {st.mean(mi):8.0f}  median {st.median(mi):8.0f}  CV {100*st.stdev(mi)/st.mean(mi):5.1f}%")
print(f"              series {mi}")
print(f"  iters  PV {st.mean(ipv):7.1f} (CV {100*st.stdev(ipv)/st.mean(ipv):4.2f}%)  G {st.mean(img):7.1f} (CV {100*st.stdev(img)/st.mean(img):4.2f}%)")
print(f"  benefit {100*(norm-1):+.2f}%   paired t={t:+.2f} (crit {crit:.2f})  {'SIG' if abs(t)>crit else 'NOT SIG'}")
print(f"  uwait  PV {st.mean(upv):.2f}s  G {st.mean(umg):.2f}s   raw diff {st.mean(upv)-st.mean(umg):+.2f}s")
print(f"  SAVED = {st.mean(upv):.2f} x {norm:.4f} - {st.mean(umg):.2f} = {saved:+.2f} s")
PY
echo "DONE $OUT"
