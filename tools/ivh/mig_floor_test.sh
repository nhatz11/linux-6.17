#!/bin/bash
# mig_floor_test.sh -- is the mig+t1 BASELINE unstable because migration is
# thrashing between equally-starved vCPUs?
#
# Under uniform starvation every vCPU reads ~630-780, all <= ivh_capacity_threshold
# (1010), so Gate 1 calls all 16 unhealthy and migration fires freely -- to
# destinations no better than the source. Lowering the threshold BELOW the live
# capacity makes Gate 1 reject, so migration goes quiet. That is arguably the
# gate working correctly, not crippling it: with no healthy target there is
# nothing to win by moving.
#
# arms:  mig1010  = current baseline (threshold 1010, migration fires freely)
#        mig500   = threshold 500 (below live cap -> Gate 1 rejects -> quiet)
# Both keep tier 1 on. Compare MEAN and CV of time and spin, plus migrations.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-4}"
OUT=/root/ivh_logs/migfloor_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_beat_tier1_fired"
printf "arm\trep\ttime\tspin_ns\tmigs\tt1\tcap\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ bash $T/pvbase.sh >/dev/null 2>&1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
  for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
  echo "$1" > $S/ivh_capacity_threshold
  [ "$(cat $S/ivh_capacity_threshold)" = "$1" ] || return 1
  sleep 1; }
echo "### live cap_mean = $(capm);  threshold 1010 => Gate 1 passes all; 500 => rejects all"
for rep in $(seq 1 $REPS); do
  for th in 1010 500; do
    arm $th || { echo "ARMFAIL"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    CM=$(capm); m0=$(mig)
    b=$(python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}')
    t0=$(date +%s%N); timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1; t1=$(date +%s%N)
    f=$(python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}')
    m1=$(mig)
    set -- $b; bw=$1; bh=$2; bt=$3; set -- $f; fw=$1; fh=$2; ft=$3
    tm=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
    printf "mig%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$th" "$rep" "$tm" "$(( (fw-bw)-(fh-bh) ))" "$((m1-m0))" "$((ft-bt))" "$CM" >> "$OUT"
    echo "  rep$rep mig$th  time=${tm}s  spin=$(python3 -c "print(f'{(($fw-$bw)-($fh-$bh))/1e9:.1f}')")s  migrations=$((m1-m0))  cap=$CM"
  done
done
echo 1010 > $S/ivh_capacity_threshold
python3 - "$OUT" <<'PY'
import sys,statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
print("\n=== BASELINE STABILITY ===")
for a in ['mig1010','mig500']:
    v=[float(x[2]) for x in r if x[0]==a]; s=[int(x[3])/1e9 for x in r if x[0]==a]
    m=[int(x[4]) for x in r if x[0]==a]
    if len(v)<2: continue
    print(f"  {a:>8s} n={len(v)}  time {st.mean(v):6.2f}s CV {100*st.stdev(v)/st.mean(v):5.1f}%  "
          f"spin {st.mean(s):7.2f}s CV {100*st.stdev(s)/st.mean(s):5.1f}%  migrations {st.mean(m):9.0f}")
    print(f"             times {[round(x,2) for x in v]}   spins {[round(x,1) for x in s]}")
PY
echo "MIGFLOOR_DONE $OUT"
