#!/bin/bash
# spinsave.sh -- spin time saved, full stack vs stock PV, two definitions.
#
#  A) ITERATIONS x CONSTANT
#       (ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum
#        + ivh_head_spin_iters_sum + ivh_head_spin_iters_bail_sum) x NS_PER_ITER
#     NS_PER_ITER=23.5 from the zero-halt points of the spin_threshold sweep,
#     where wall IS pure spin by definition. Iteration counts are exact, so
#     arm-vs-arm RATIOS are exact; absolute seconds carry about +/-20%.
#
#  B) SLOWPATH TIME WHILE ON A vCPU
#       ivh_slowpath_wait_ns - (node_halt_cycles + head_halt_cycles)/2.2
#     i.e. total slowpath wall minus halted time, so preempted time is INCLUDED
#     and halted time is EXCLUDED, as asked. TSC = 2200 MHz.
#     KNOWN DEFECT (tools/bpf/docs/spin_time_measurement.md): this mixes two
#     gates and two clocks -- wait_ns is sched_clock and gated on
#     ivh_slowpath_wait_measure + !in_interrupt(); halt cycles are raw TSC with
#     no gate. It produced NEGATIVE spin on 3 of 243 runs. Treat B as
#     indicative, A as the defensible one.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
R="python3 $T/read_ivh_counters.py"
REPS="${REPS:-5}"
NS_PER_ITER="${NS_PER_ITER:-23.5}"
OUT="${OUT:-$T/spinsave_$(date +%m%d-%H%M%S).csv}"
tot(){ $R "$1" 2>/dev/null | grep -E "\[TOTAL" | grep -oE '[0-9]+$' || echo 0; }
plain(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' | tail -1 || echo 0; }
snap(){ echo "$(plain ivh_node_spin_iters_sum) $(plain ivh_node_spin_success_iters_sum) \
$(plain ivh_head_spin_iters_sum) $(plain ivh_head_spin_iters_bail_sum) \
$(plain ivh_slowpath_wait_ns) $(tot ivh_node_halt_cycles) $(tot ivh_head_halt_cycles)"; }
echo "workload,arm,rep,perf,node_i,node_si,head_i,head_bi,wait_ns,node_halt_c,head_halt_c" > "$OUT"
for spec in "ebizzy|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'" \
            "nhextend|/root|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16|grep -oP 'Ran for \K[0-9]+'"; do
  IFS='|' read -r name dir cmd ext <<< "$spec"
  echo "########## $name ##########"
  for r in $(seq 1 "$REPS"); do
    for i in 0 1; do
      a=$([ $(( (r+i) % 2 )) -eq 0 ] && echo pv || echo full)
      [ "$a" = pv ] && bash $T/pvbase.sh >/dev/null || { bash $T/p78_arm.sh tlt 10000000 >/dev/null; echo 8 > $S/ivh_max_concurrent; }
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      read n0 s0 h0 b0 w0 nh0 hh0 <<< "$(snap)"
      v=$( ( cd "$dir" && eval "$cmd" 2>&1 ) | eval "$ext" | tail -1 )
      read n1 s1 h1 b1 w1 nh1 hh1 <<< "$(snap)"
      echo "$name,$a,$r,${v:-0},$((n1-n0)),$((s1-s0)),$((h1-h0)),$((b1-b0)),$((w1-w0)),$((nh1-nh0)),$((hh1-hh0))" >> "$OUT"
      printf "  %-9s %-5s r%s perf=%s\n" "$name" "$a" "$r" "${v:-0}"
    done
  done
done
python3 - "$OUT" "$NS_PER_ITER" <<'PY'
import sys,csv,statistics as st
rows=list(csv.DictReader(open(sys.argv[1]))); K=float(sys.argv[2])
for w in ['ebizzy','nhextend']:
    rs=[r for r in rows if r['workload']==w]
    if not rs: continue
    print(f"\n=== {w} ===")
    res={}
    for a in ['pv','full']:
        g=lambda k:[float(x[k]) for x in rs if x['arm']==a]
        if not g('perf'): continue
        it=[(float(x['node_i'])+float(x['node_si'])+float(x['head_i'])+float(x['head_bi'])) for x in rs if x['arm']==a]
        A=[i*K/1e9 for i in it]
        B=[(float(x['wait_ns'])-(float(x['node_halt_c'])+float(x['head_halt_c']))/2.2)/1e9 for x in rs if x['arm']==a]
        res[a]=(st.median(g('perf')),st.median(A),st.median(B),st.median(it))
        print(f"  {a:5} perf={res[a][0]:>12,.0f}  A(iters x {K}ns)={res[a][1]:8.3f}s  B(slowpath on-vCPU)={res[a][2]:8.3f}s  iters={res[a][3]:>15,.0f}")
    if 'pv' in res and 'full' in res:
        p,f=res['pv'],res['full']
        print(f"  ---> perf     {100*(f[0]-p[0])/p[0]:+7.2f}%")
        print(f"  ---> spin A   {p[1]:.3f}s -> {f[1]:.3f}s   saved {p[1]-f[1]:+.3f}s  ({100*(p[1]-f[1])/p[1]:+.1f}%)")
        print(f"  ---> spin B   {p[2]:.3f}s -> {f[2]:.3f}s   saved {p[2]-f[2]:+.3f}s  ({100*(p[2]-f[2])/max(p[2],1e-9):+.1f}%)")
PY
bash $T/pvbase.sh >/dev/null 2>&1
echo SPINSAVE-DONE
