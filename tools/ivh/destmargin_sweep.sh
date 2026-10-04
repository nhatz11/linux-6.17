#!/bin/bash
# destmargin_sweep.sh -- does a STRICTER destination bar stabilise the mig+t1
# baseline? PCT = required % by which a destination must beat the source.
# PCT=0 is the control (old behaviour: only the ~2.6% absolute noise rail).
# Hypothesis: at PCT high enough, marginal/noise targets are rejected, migration
# stops thrashing between equally-starved vCPUs, and the baseline CV falls.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-3}"; PCTS="${PCTS:-0 5 15 25}"
OUT=/root/ivh_logs/destmargin_$(date +%m%d-%H%M%S).tsv
printf "pct\trep\tpos\tdestfire\ttime\tspin_ns\tmigs\tnotbetter\tacc_t1\tacc_t2\tcaplow\tcap\n" > "$OUT"
rj(){ bpftool map lookup name reject_reasons key $1 0 0 0 2>/dev/null | grep -oP '"value": \K[0-9]+' | paste -sd+ | bc; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
cnt(){ python3 $T/read_ivh_counters.py ivh_slowpath_wait_ns ivh_slowpath_halt_ns 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
arm(){ bash $T/pvbase.sh >/dev/null 2>&1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
  for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
  bpftool map update name ivh_cfg key 1 0 0 0 value $1 0 0 0 >/dev/null || return 1
  V=$(bpftool map lookup name ivh_cfg key 1 0 0 0 | grep -oP '"value": \K[0-9]+')
  [ "$V" = "$1" ] || { echo "  CFGFAIL want=$1 got=$V"; return 1; }
  sleep 1; }
echo "### dest-margin sweep  pcts=[$PCTS] reps=$REPS  cap_mean=$(capm)  -> $OUT"
# ROTATE arm order each rep: without this, pct is aliased with position-in-rep,
# and within-rep drift (seen 1003-063105: time falls monotonically every rep,
# cap_mean falls with it) is indistinguishable from the knob's effect.
for rep in $(seq 1 $REPS); do
 SET=($PCTS); K=${#SET[@]}; ORD=""
 for i in $(seq 0 $((K-1))); do ORD="$ORD ${SET[$(( (i + rep - 1) % K ))]}"; done
 echo "  -- rep$rep order:$ORD"
 for p in $ORD; do
  POS=$(( $(echo "$ORD" | tr " " "\n" | grep -n "^$p$" | head -1 | cut -d: -f1) ))
  arm "$p" || continue
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
  CM=$(capm); d0=$(rj 12); M0=$(python3 $T/migcount.py); b=($(cnt)); n0=$(rj 5); a0=$(rj 9); a1=$(rj 10); c0=$(rj 4)
  t0=$(date +%s%N); timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1; t1=$(date +%s%N)
  f=($(cnt)); d1=$(rj 12); M1=$(python3 $T/migcount.py); n1=$(rj 5); A1=$(rj 9); A2=$(rj 10); c1=$(rj 4)
  tm=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
  sp=$(( (${f[0]}-${b[0]}) - (${f[1]}-${b[1]}) ))
  DF=$((d1-d0))
  [ "$p" = 0 ] && [ "$DF" -ne 0 ] && echo "  *** ASSERT FAIL: pct=0 but margin fired $DF"
  [ "$p" != 0 ] && [ "$DF" -eq 0 ] && echo "  *** ASSERT FAIL: pct=$p fired ZERO -- knob dead"
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$p" "$rep" "$POS" "$DF" "$tm" "$sp" "$((M1-M0))" \
     "$((n1-n0))" "$((A1-a0))" "$((A2-a1))" "$((c1-c0))" "$CM" >> "$OUT"
  echo "  rep$rep pct=$p destfire=$DF time=${tm}s spin=$(python3 -c "print(f'{$sp/1e9:.1f}')")s migs=$((M1-M0)) notbetter=$((n1-n0)) acc=$(( (A1-a0)+(A2-a1) )) cap=$CM"
 done
done
bpftool map update name ivh_cfg key 1 0 0 0 value 0 0 0 0 >/dev/null
python3 - "$OUT" <<'PY'
import sys,statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
print("\n=== does a stricter destination bar stabilise the baseline? ===")
print("  (POSITION control below: if position explains as much as pct, the knob is not the cause)")
print(f"  {'pct':>4s} {'n':>2s} {'time_mean':>10s} {'CV':>6s} {'spin_mean':>10s} {'CV':>6s} {'migs':>8s} {'accept%':>8s}")
for p in sorted({x[0] for x in r}, key=int):
    g=[x for x in r if x[0]==p]
    if len(g)<2: continue
    tv=[float(x[4]) for x in g]; sv=[int(x[5])/1e9 for x in g]; mv=[int(x[6]) for x in g]
    acc=[int(x[8])+int(x[9]) for x in g]; nb=[int(x[7]) for x in g]
    df=[int(x[3]) for x in g]
    ash=100*sum(acc)/max(sum(acc)+sum(nb),1)
    print(f"  {p:>4s} {len(g):>2d} {st.mean(tv):10.2f} {100*st.stdev(tv)/st.mean(tv):5.1f}% "
          f"{st.mean(sv):10.2f} {100*st.stdev(sv)/st.mean(sv):5.1f}% {st.mean(mv):8.0f} {ash:7.1f}%  destfire_mean {st.mean(df):>12,.0f}")
    print(f"       times {[round(x,2) for x in tv]}  spins {[round(x,1) for x in sv]}")
print("\n  --- POSITION-IN-REP control (same runs, grouped by slot not pct) ---")
for pos in sorted({x[2] for x in r}, key=int):
    g=[x for x in r if x[2]==pos]
    if len(g)<2: continue
    tv=[float(x[4]) for x in g]
    print(f"   slot {pos}  n={len(g)}  time_mean {st.mean(tv):6.2f}  CV {100*st.stdev(tv)/st.mean(tv):5.1f}%  "
          f"times {[round(x,2) for x in tv]}")
PY
echo "DESTMARGIN_DONE $OUT"
