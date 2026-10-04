#!/bin/bash
# Replicate the top tune_steal3 candidates. The 4x4 grid was non-monotonic in
# phase_pct at fixed deadband (fine ratio 0.605 -> 2.589 -> 0.909 -> 1.340),
# so single-cell ranking is inside the noise. n reps per candidate, report
# median + spread.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
COARSE=3; FINE=15; IDLE=8; SECS=${SECS:-8}; REPS=${REPS:-5}
OUT=$T/tune_steal4_$(date +%m%d-%H%M%S).csv
echo "phase_pct,deadband,rep,c_ratio,f_ratio,idle_kern_ns" > $OUT
allst(){ python3 $T/read_vact_rq.py ivh_tks_steal_ns 2>/dev/null \
         | sed 's/.*per-cpu=\[//;s/\].*//' | tr -d ' '; }
pick(){ echo "$1" | cut -d, -f$(($2+1)); }
hostns(){ awk '/^hist/{split($3,g,"="); split($5,s,"=");
                if(g[2]>=10000000) next; if(g[2]>=50000) h+=s[2]} END{print h+0}' "$1"; }
for cand in "100 1000" "100 1250" "150 1250"; do
  set -- $cand; P=$1; D=$2
  echo "$P" > $S/ivh_tks_phase_pct; echo "$D" > $S/ivh_tks_deadband_ns
  for r in $(seq $REPS); do
    A=$(allst)
    timeout -k 5 $((SECS+25)) $T/vcpu_gone $COARSE $SECS 1 > /tmp/qc.txt 2>/dev/null & p1=$!
    timeout -k 5 $((SECS+25)) $T/vcpu_gone $FINE   $SECS 1 > /tmp/qf.txt 2>/dev/null & p2=$!
    wait $p1 $p2
    B=$(allst)
    awk -v p=$P -v d=$D -v r=$r \
        -v ch=$(hostns /tmp/qc.txt) -v fh=$(hostns /tmp/qf.txt) \
        -v ck=$(( $(pick "$B" $COARSE) - $(pick "$A" $COARSE) )) \
        -v fk=$(( $(pick "$B" $FINE)   - $(pick "$A" $FINE) )) \
        -v ik=$(( $(pick "$B" $IDLE)   - $(pick "$A" $IDLE) )) \
      'BEGIN{printf "%d,%d,%d,%.4f,%.4f,%d\n",p,d,r,(ch>0?ck/ch:0),(fh>0?fk/fh:0),ik}' >> $OUT
  done
  printf "  pct=%-4s db=%-5s x%s done\n" $P $D $REPS
done
echo; echo "=== median of $REPS reps ==="
awk -F, 'NR>1{c[$1"_"$2]=c[$1"_"$2]" "$4; f[$1"_"$2]=f[$1"_"$2]" "$5}
  END{for(k in c){n=split(c[k],A," "); split(f[k],B," ");
    asort(A); asort(B);
    printf "  %-12s coarse med=%.3f [%.3f-%.3f]   fine med=%.3f [%.3f-%.3f]\n",
      k, A[int((n+1)/2)], A[1], A[n], B[int((n+1)/2)], B[1], B[n]}}' $OUT 2>/dev/null \
 || python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
from collections import defaultdict
d=defaultdict(lambda:([],[]))
for r in csv.DictReader(open(sys.argv[1])):
    k=(r['phase_pct'],r['deadband']); d[k][0].append(float(r['c_ratio'])); d[k][1].append(float(r['f_ratio']))
for k,(c,f) in sorted(d.items()):
    print(f"  pct={k[0]:>4} db={k[1]:>5}  coarse med={st.median(c):.3f} [{min(c):.3f}-{max(c):.3f}]"
          f"   fine med={st.median(f):.3f} [{min(f):.3f}-{max(f):.3f}]")
PY
echo "DONE -> $OUT"
