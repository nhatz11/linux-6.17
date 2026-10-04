#!/bin/bash
# ebizzy_settle.sh -- is ebizzy NEUTRAL under AS, or genuinely negative?
#
# n=5 gave perf median -1.79% with stdev 1.90% and paired t = -1.54 (|t|>2.78
# needed at n=5), i.e. unresolved. n=12 gives t-crit 2.20 and ~2.2x tighter CI.
#
# Why neutral is the ceiling here, not a shortfall: ebizzy contends on only 1.21%
# of its 5.47M acq/s, and a contended entry acquires after 42.8 spin iterations =
# 0.13% of the 32768 budget. Both the publish mask (256) and PV_PREV_CHECK_MASK
# (256) are never reached, so AS's per-iteration cost never fires; its only cost
# is the per-entry enqueue stamp, which no threshold or mask can reduce. There is
# no spin to cut and no overhead to tune away -- only a question of whether the
# residual is inside +-1%.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
REPS="${REPS:-12}"
OUT=/root/ivh_logs/ebizzy_settle_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1; fi; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 1800 9 || { echo FATAL; exit 1; }
printf "arm\trep\tops\titers\tentries\tt2f\tcap\n" > "$OUT"
echo "### ebizzy_settle n=$REPS -> $OUT"
( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 )   # warm-up
for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
    arm "$a" || { echo "  ARMFAIL $a"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    CM=$(capm); b=($(snap))
    v=$( cd /root && timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 | grep -oP '^\K[0-9]+(?= records/s)' | head -1 )
    f=($(snap))
    IT=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) )); EN=$(( ${f[2]}-${b[2]} )); T2=$(( ${f[3]}-${b[3]} ))
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CM" >> "$OUT"
    echo "  rep$rep $a ops=${v:-NA} spin=$(python3 -c "print('%.2fs'%($IT*26e-9))") t2f=$T2 cap=$CM"
  done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 - "$OUT" <<'PY'
import sys, statistics as st
NS=26e-9
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
by={}
for x in r:
    if x[2] not in ('NA',''): by.setdefault(x[1],{})[x[0]]=x
pf=[];sp=[]
for rep,g in by.items():
    if 'pv' not in g or 'as' not in g: continue
    o0,o1=float(g['pv'][2]),float(g['as'][2]); i0,i1=int(g['pv'][3]),int(g['as'][3])
    pf.append(100*(o1-o0)/o0); sp.append(100*(i0-i1)/i0 if i0 else 0)
n=len(pf)
def ci(v):
    m=st.mean(v); sd=st.stdev(v); se=sd/n**0.5
    tc={6:2.57,7:2.45,8:2.36,9:2.31,10:2.26,11:2.23,12:2.20}.get(n,2.20)
    return m, m-tc*se, m+tc*se, m/se if se else 0
print(f"\n=== ebizzy, n={n} paired ===")
for lbl,v in (("PERF",pf),("SPIN",sp)):
    m,lo,hi,t=ci(v)
    verdict = "NEUTRAL (CI spans 0)" if lo<0<hi else ("POSITIVE" if lo>0 else "NEGATIVE")
    within = "  and entirely within +-1%" if lo>-1.0 and hi<1.0 else ("  and CI low end >= -1%" if lo>=-1.0 else "")
    print(f"  {lbl}: mean {m:+.2f}%  95% CI [{lo:+.2f}, {hi:+.2f}]  t={t:+.2f}  -> {verdict}{within}")
    print(f"        per-rep {[round(z,1) for z in v]}")
print(f"\n  GOAL TEST (spin>=0 and perf>=-1%): ", end="")
mp,lop,hip,_=ci(pf); ms,los,his,_=ci(sp)
print("PASS -- perf CI low end >= -1% and spin not negative" if lop>=-1.0 and his>0 else
      f"FAIL -- perf CI [{lop:+.2f},{hip:+.2f}], spin CI [{los:+.2f},{his:+.2f}]")
PY
echo "EBIZZY_SETTLE_DONE $OUT"
