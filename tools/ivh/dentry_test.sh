#!/bin/bash
# dentry_test.sh -- can stressng_dentry replace ebizzy as the 5th/6th workload?
#
# Rationale: ebizzy CANNOT save spin -- spin volume 2.83e6 (contended 66,091/s x
# 42.8 iters/entry), 180x below hackbench, so there is no wait to recover and the
# n=12 result is spin mean -3.17% with a CI spanning zero. dentry by contrast
# measured 118.11s of PV spin in point 10 and saved +84.51s, the second-largest
# absolute saving of any workload tested.
#
# Point 10 ran dentry with migration ON (perf -10.99%). This runs AS-ALONE
# (migration off both arms), which is the configuration that is perf-neutral on
# the other four. Same shared 50us on beat/evict/noise, mask 255, inputs
# unchanged from benchmarks.tsv.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
REPS="${REPS:-6}"
OUT=/root/ivh_logs/dentry_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1; fi; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 1800 9 || { echo FATAL; exit 1; }
printf "arm\trep\tbogo\titers\tentries\tt2f\tcap\n" > "$OUT"
echo "### dentry AS-alone @50us mask255 n=$REPS cap=$(capm) -> $OUT"
for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
    arm "$a" || { echo "  ARMFAIL $a"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    CM=$(capm); b=($(snap))
    v=$( timeout 120 stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' | head -1 )
    f=($(snap))
    IT=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) )); EN=$(( ${f[2]}-${b[2]} )); T2=$(( ${f[3]}-${b[3]} ))
    [ "$a" = as ] && [ "$T2" -eq 0 ] && echo "  *** DEAD AS: zero tier2"
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CM" >> "$OUT"
    echo "  rep$rep $a bogo=${v:-NA} spin=$(python3 -c "print('%.2fs'%($IT*26e-9))") t2f=$T2 cap=$CM"
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
pf=[];sp=[];s0=[];s1=[]
for rep,g in by.items():
    if 'pv' not in g or 'as' not in g: continue
    o0,o1=float(g['pv'][2]),float(g['as'][2]); i0,i1=int(g['pv'][3]),int(g['as'][3])
    pf.append(100*(o1-o0)/o0); sp.append(100*(i0-i1)/i0 if i0 else 0)
    s0.append(i0*NS); s1.append(i1*NS)
n=len(pf)
tc={3:4.30,4:3.18,5:2.78,6:2.57,7:2.45,8:2.36}.get(n,2.57)
def ci(v):
    m=st.mean(v); se=st.stdev(v)/n**0.5 if n>1 else 0
    return m, m-tc*se, m+tc*se
print(f"\n=== stressng_dentry, AS-alone @50us, n={n} paired ===")
print(f"  PV spin {st.mean(s0):.2f}s -> AS spin {st.mean(s1):.2f}s   absolute saved {st.mean(s0)-st.mean(s1):+.2f}s")
for lbl,v in (("PERF",pf),("SPIN",sp)):
    m,lo,hi=ci(v)
    print(f"  {lbl}: mean {m:+.2f}%  95% CI [{lo:+.2f}, {hi:+.2f}]  ({sum(1 for z in v if z>0)}/{n} positive)")
    print(f"        per-rep {[round(z,1) for z in v]}")
mp,lop,hip=ci(pf); ms,los,his=ci(sp)
ok = ms>0 and lop>=-1.0
print(f"\n  GOAL TEST (spin saved and perf >= -1%): {'PASS' if ok else 'FAIL'}")
print(f"    spin mean {ms:+.2f}% (need >0), perf CI low {lop:+.2f}% (need >=-1)")
PY
echo "DENTRY_DONE $OUT"
