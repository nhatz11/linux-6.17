#!/bin/bash
# ebizzy, paper config: 2.5ms + csmin + tier1 both arms, tier2 off.
# Alternating BLOCKS (not pure interleave) because each arm switch needs a
# discarded warmup -- without it ebizzy's first run in an arm is unrepresentative
# and migrations are suppressed (10,302 vs 33,136). That warmup is the whole
# reason this workload read +0.57% earlier.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
BLOCKS="${1:-3}"; PER="${2:-2}"
CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
X="grep -oP '^\K[0-9]+(?= records/s)'"
COOL="${COOLDOWN:-50000}"
OUT=/root/ivh_logs/ebizzy_final_$(date +%m%d-%H%M%S).tsv
printf "arm\tblock\trep\trecs\tmigs\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$'; }
arm(){ case $1 in
    pv)  bash $T/pvbase.sh >/dev/null 2>&1 ;;
    mig) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
         echo 0 > $S/ivh_pv_tier2_enable
         echo 1 > $S/ivh_cs_gate2_reference
         echo "$COOL" > $S/ivh_eval_cooldown_ns ;;
  esac
  echo 1 > $S/ivh_pv_tier1_enable
  [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || { echo "FATAL tier1"; return 1; }
  [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "FATAL tier2"; return 1; }
  if [ "$1" = mig ]; then
    [ "$(cat $S/ivh_cs_gate2_reference)" = 1 ] || { echo "FATAL csmin"; return 1; }
    [ "$(cat $S/ivh_time_left_threshold_ns)" = 2500000 ] || { echo "FATAL thresh"; return 1; }
    [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "FATAL mig off"; return 1; }
  else
    [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "FATAL pv has mig"; return 1; }
  fi
  sleep 1; }
echo "### ebizzy final: 2.5ms csmin tier1-both tier2-off cooldown=$COOL"
for b in $(seq 1 "$BLOCKS"); do
  if [ $((b % 2)) -eq 1 ]; then ORDER="pv mig"; else ORDER="mig pv"; fi
  for a in $ORDER; do
    arm $a || continue
    ( cd /root && timeout 120 $CMD >/dev/null 2>&1 )      # warmup, DISCARDED
    for r in $(seq 1 "$PER"); do
      m0=$(mig)
      v=$( cd /root && timeout 120 $CMD 2>&1 | eval "$X" | head -1 )
      m1=$(mig)
      printf "%s\t%d\t%d\t%s\t%d\n" "$a" "$b" "$r" "${v:-NA}" "$((m1-m0))" >> "$OUT"
      printf "  block%d %-4s rep%d  %-7s migs=%d\n" "$b" "$a" "$r" "${v:-NA}" "$((m1-m0))"
    done
  done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference; echo 50000 > $S/ivh_eval_cooldown_ns
python3 - "$OUT" <<'PY'
import sys, statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for x in rows:
    if x[3]=='NA': continue
    d.setdefault(x[0],{'r':[],'m':[]}); d[x[0]]['r'].append(float(x[3])); d[x[0]]['m'].append(int(x[4]))
A,B=d['pv']['r'],d['mig']['r']
ma,mb=st.mean(A),st.mean(B)
sa=st.stdev(A)/len(A)**.5 if len(A)>1 else 0; sb=st.stdev(B)/len(B)**.5 if len(B)>1 else 0
g=100*(mb-ma)/ma; se=100*((sa**2+sb**2)**.5)/ma
print(f"\n  ===== EBIZZY FINAL =====")
print(f"    pv    {ma:8.0f} recs/s  CV {100*st.stdev(A)/ma if len(A)>1 else 0:4.1f}%  n={len(A)}")
print(f"    mig   {mb:8.0f} recs/s  CV {100*st.stdev(B)/mb if len(B)>1 else 0:4.1f}%  n={len(B)}  migs={int(st.mean(d['mig']['m']))}/run")
print(f"    gain  {g:+7.2f}% +/-{se:4.2f}   {'SIG' if abs(g)>2*se else 'ns'}")
PY
echo "EBIZZY_FINAL_DONE $OUT"
