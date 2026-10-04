#!/bin/bash
# hostdose.sh -- host-side preemption as an INDEPENDENT dose axis.
#
# Why: the AS dose-response currently regresses the AS spin delta on the
# REFERENCE ARM's own spin, which is close to circular. Host runqueue wait_ns
# for our TD's 16 vCPU threads is measured outside the guest entirely, so it is
# independent of which arm is running.
#
# Our TD is pid 3143009 (tdvirsh-trust_domain); it changes every reboot and a
# stale pid silently reads ZEROS, so the pid is resolved by name each run and
# a zero delta is a hard failure, not a data point.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
H="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ControlMaster=auto -o ControlPath=/tmp/ivhhost-%r@%h -o ControlPersist=300 "$IVH_HOST""
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-8}"
OUT=/root/ivh_logs/hostdose_$(date +%m%d-%H%M%S).tsv
TDPID=$($H 'pgrep -f "guest=tdvirsh-trust_domain" | head -1' 2>/dev/null)
[ -n "$TDPID" ] || { echo "FATAL: could not resolve TD pid on host"; exit 1; }
echo "### TD pid on host = $TDPID   reps=$REPS   -> $OUT"
hostwait(){ $H "for t in \$(ls /proc/$TDPID/task); do case \"\$(cat /proc/$TDPID/task/\$t/comm 2>/dev/null)\" in 'CPU '*) awk '{print \$2}' /proc/$TDPID/task/\$t/schedstat 2>/dev/null;; esac; done | paste -sd+ | bc" 2>/dev/null; }
printf "arm\trep\ttime\tspin_ns\thostwait_ms\thostwait_ms_per_s\tt1\tt2f\tcap\n" > "$OUT"
cnt(){ python3 $T/read_ivh_counters.py ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_beat_tier1_fired ivh_beat_tier2_fired 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ bash $T/pvbase.sh >/dev/null 2>&1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
  case $1 in
    mig) for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
         [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1 ;;
    as)  IVH_MASK=255 bash $T/p11_arm.sh 400 >/dev/null 2>&1 || return 1
         [ "$(cat $S/ivh_pv_beat_publish_mask)" = 255 ] || return 1 ;;
  esac; sleep 1; }
for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="mig as";; 0) O="as mig";; esac
  for a in $O; do
    arm "$a" || { echo "  ARMFAIL $a"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    CM=$(capm); b=($(cnt)); W0=$(hostwait)
    t0=$(date +%s%N); timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1; t1=$(date +%s%N)
    W1=$(hostwait); f=($(cnt))
    [ -n "$W0" ] && [ -n "$W1" ] && [ "$W1" != "$W0" ] || { echo "  HOSTREAD FAIL (stale pid?) -- dropping run"; continue; }
    tm=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
    sp=$(( (${f[0]}-${b[0]}) - (${f[1]}-${b[1]}) ))
    HW=$(python3 -c "print(f'{($W1-$W0)/1e6:.1f}')")
    HWS=$(python3 -c "print(f'{($W1-$W0)/1e6/(($t1-$t0)/1e9):.1f}')")
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "$tm" "$sp" "$HW" "$HWS" \
      "$(( ${f[2]}-${b[2]} ))" "$(( ${f[3]}-${b[3]} ))" "$CM" >> "$OUT"
    echo "  rep$rep $a  time=${tm}s spin=$(python3 -c "print(f'{$sp/1e9:.1f}')")s  HOSTWAIT=${HW}ms (${HWS} ms/s)  cap=$CM"
  done
done
python3 $T/hostdose_report.py "$OUT" || true
echo "HOSTDOSE_DONE $OUT"
