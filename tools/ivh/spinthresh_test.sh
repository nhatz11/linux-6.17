#!/bin/bash
# spinthresh_test.sh -- sweep ivh_pv_spin_threshold, the one mechanism threshold
# never swept. Applied identically to BOTH arms and all workloads.
#
# TWO reasons it should matter:
# 1) MEASURABILITY. vips's head term is quantised: head iterations are always a
#    multiple of the spin budget because only EXHAUSTED heads are counted. At
#    32768 that is 8.2 events/run (Poisson rel. sd 35%) each worth 32,768 iters,
#    injected into a ~20 ms total -> CV 131%. Lowering the budget raises the
#    event count and shrinks the quantum; variance falls as 1/sqrt(events).
# 2) THE RE-ARM PENALTY. `threshold` is re-read inside the outer for(;;)
#    (qspinlock_paravirt.h:1919/:3595), so each false-positive halt costs a FULL
#    fresh budget. At 4096 that cost is 8x smaller, which is the dominant term
#    on low-spin workloads (ebizzy, vips).
#
# vips first: it is the blocker. If variance drops and the verdict moves, extend.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${REPS:-8}"; THRS="${THRS:-32768 4096}"
OUT=/root/ivh_logs/spinthresh_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ # $1 = pv|as   $2 = spin_threshold
  if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
  else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
       echo 0 > $S/ivh_universal_eligible; fi
  echo "$2" > $S/ivh_pv_spin_threshold
  [ "$(cat $S/ivh_pv_spin_threshold)" = "$2" ] || { echo "  THRESHFAIL"; return 1; }
  sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 3600 9 || { echo FATAL; exit 1; }
printf "wl\tthr\tarm\trep\tval\tnode\thead\tentries\tt2f\tcap\n" > "$OUT"
echo "### spinthresh thrs=[$THRS] reps=$REPS cap=$(capm) -> $OUT"
( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
for TH in $THRS; do
 echo "########## spin_threshold=$TH ##########"
 for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
   arm "$a" "$TH" || { echo "  ARMFAIL"; continue; }
   sync; sleep 1; CM=$(capm); b=($(snap)); t0=$(date +%s%N)
   ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
   t1=$(date +%s%N); f=($(snap))
   v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
   printf "vips\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\n" "$TH" "$a" "$rep" "$v" \
     "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" \
     "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" "$CM" >> "$OUT"
   echo "  thr$TH rep$rep $a ${v}s t2f=$(( ${f[5]}-${b[5]} )) cap=$CM"
  done
 done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 32768 > $S/ivh_pv_spin_threshold; echo 22000 > $S/ivh_cs_noise_cycles
python3 - "$OUT" <<'PY'
import sys, statistics as st, math
NS=26e-9
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
print("\n=== vips: does a smaller spin budget make it measurable? ===")
for th in sorted({x[1] for x in r}, key=int, reverse=True):
    wr=[x for x in r if x[1]==th and x[4] not in ('NA','')]
    by={}
    for x in wr: by.setdefault(x[3],{})[x[2]]=x
    pvs=[];sp=[];pf=[];t2=[]
    for rep,g in by.items():
        if 'pv' not in g or 'as' not in g: continue
        p,a=g['pv'],g['as']
        t0=int(p[5])+int(p[6]); t1=int(a[5])+int(a[6])
        if t0<=0: continue
        pvs.append(t0*NS*1000); sp.append(100*(t0-t1)/t0)
        pf.append(100*(float(p[4])-float(a[4]))/float(p[4])); t2.append(int(a[8]))
    n=len(sp)
    if n<3: continue
    tc=1.96+2.4/n
    def ci(v):
        m=st.mean(v); se=st.stdev(v)/math.sqrt(n); return m,m-tc*se,m+tc*se
    cv=100*st.stdev(pvs)/st.mean(pvs)
    m,lo,hi=ci(sp); mp,lop,hip=ci(pf)
    print(f"\n  threshold {th:>6s}  n={n}  PV spin {st.mean(pvs):6.1f}ms  CV {cv:5.0f}%   t2 fires/run {st.mean(t2):6.0f}")
    print(f"    SPIN {m:+8.2f}%  CI [{lo:+8.2f},{hi:+8.2f}]  {'POSITIVE' if lo>0 else ('NEGATIVE' if hi<0 else 'neutral')}")
    print(f"    PERF {mp:+8.2f}%  CI [{lop:+8.2f},{hip:+8.2f}]  {'POSITIVE' if lop>0 else ('NEGATIVE' if hip<0 else 'neutral')}")
PY
echo "SPINTHRESH_DONE $OUT"
