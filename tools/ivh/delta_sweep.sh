#!/bin/bash
# delta_sweep.sh -- the csmin sleep margin, one variable, ABSOLUTE ops.
#
# Comparing benefit% across configs is unfair: a better lock raises the PV
# baseline and shrinks migration's headroom, so benefit% falls even when the
# configuration is faster in absolute terms. This sweeps only the delta, with
# the kernel arm fixed at stock PV and no migration, so the number compared is
# raw throughput.
#
#   fin        heartbeat staleness (50us), in-CS republish -- the old design
#   d10000     csmin, sleep at cmin + 10us   (below the ~1.3ms mean CS)
#   d100000    csmin, sleep at cmin + 100us
#   d1000000   csmin, sleep at cmin + 1ms    (above the mean CS; THE project delta)
#   ctl        csmin, delta 100ms -- predicate never fires
set -u
R="${1:-5}"
OUT=/root/ivh_logs/deltasweep_$(date +%m%d-%H%M%S).tsv
exec 9>/var/lock/ivh_clean_check.lock
flock -w 1800 9 || { echo "FATAL: bench lock"; exit 1; }
bash /root/ivh_tools/pvbase.sh >/dev/null 2>&1
echo "### stock PV, no migration. base_slice=$(cat /sys/kernel/debug/sched/base_slice_ns)"
printf "arm\trep\tops\twait_s\n" > "$OUT"
ARMS="fin d10000 d100000 d1000000 ctl"
for rep in $(seq 1 "$R"); do
  ORD=$(python3 -c "a='''$ARMS'''.split(); k=($rep-1)%len(a); print(' '.join(a[k:]+a[:k]))")
  for a in $ORD; do
    case $a in
      fin) B=/root/linux-6.17/NHextend-fin ;;
      ctl) B=/root/linux-6.17/NHextend-csmin-ctl ;;
      *)   B=/root/linux-6.17/NHextend-csmin-${a/d/d} ;;
    esac
    o=$(timeout 60 env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 "$B" -l -n 16 2>&1)
    ops=$(printf '%s' "$o" | grep -oP 'Ran for \K[0-9]+')
    w=$(printf '%s' "$o" | grep -oP 'Total wait time: \K[0-9.]+')
    printf "%s\t%s\t%s\t%s\n" "$a" "$rep" "${ops:-NA}" "${w:-NA}" >> "$OUT"
    echo "  rep$rep $a ops=${ops:-NA}"
  done
done
echo "DELTASWEEP_DONE $OUT"
