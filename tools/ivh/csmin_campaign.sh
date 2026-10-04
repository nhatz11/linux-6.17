#!/bin/bash
# 3-phase csmin / threshold campaign.  Usage: csmin_campaign.sh <A|B|C> [REPS]
#   A  THRESH=4000000  gate2_ref=0   <- the DOCUMENTED shipped state (eval_final:1505)
#   B  THRESH=2500000  gate2_ref=0   <- 1.5ms head spin budget + 1ms delta
#   C  THRESH=2500000  gate2_ref=1   <- same, but Gate 2 reads min_cs_ns
# Arms interleaved per rep so host drift cannot land on one arm.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
PH="${1:?phase A|B|C}"; REPS="${2:-3}"
case $PH in
  A) THRESH=4000000; G2=0 ;;
  B) THRESH=2500000; G2=0 ;;
  C) THRESH=2500000; G2=1 ;;
  *) echo "phase must be A, B or C"; exit 1 ;;
esac
OUT=/root/ivh_logs/csmin_phase${PH}_$(date +%m%d-%H%M%S).tsv
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
NH="env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=$G2 /root/linux-6.17/NHextend-csmin -l -n 16"

# name|metric|dir|cmd|extractor      metric: TIME=lower better, THR=higher better
W=("hackbench|TIME|/root|timeout 180 hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
   "nhextend|THR|/root|timeout 90 $NH|grep -oP 'Ran for \K[0-9]+'"
   "ebizzy|THR|/root|timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
   "fsmark|THR|/root|timeout 180 fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
   "memtier|THR|/root|timeout 120 $MT|grep -oP 'Totals\s+\K[0-9.]+'"
   )
# dedup is NOT here: it needs parsecmgmt, a per-run page-cache drop and a
# discarded warmup (parsec_ab.sh). Run it per phase with dedup_phase.sh.
[ -n "${ONLY:-}" ] && W=($(printf '%s\n' "${W[@]}" | grep -E "^($ONLY)\|"))

arm(){ case $1 in
    pv)  bash $T/pvbase.sh >/dev/null 2>&1 || return 1
         [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "FATAL pv: IVH on"; return 1; } ;;
    mig) bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1 || return 1
         [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "FATAL mig: IVH off"; return 1; }
         [ "$(cat $S/ivh_rcu_guard)" = 0 ] || { echo "FATAL mig: rcu_guard on"; return 1; }
         [ "$(cat $S/ivh_time_left_threshold_ns)" = "$THRESH" ] || { echo "FATAL mig: thresh"; return 1; } ;;
  esac
  echo "$G2" > $S/ivh_cs_gate2_reference
  [ "$(cat $S/ivh_cs_gate2_reference)" = "$G2" ] || { echo "FATAL: gate2_ref"; return 1; }
  sleep 1; }

prep(){ case $1 in
    fsmark)  rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
    memtier) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
             memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
  esac; }

printf "phase\tworkload\tmetric\tarm\trep\tval\tmigdone\n" > "$OUT"
echo "### phase $PH: THRESH=$THRESH gate2_ref=$G2, reps=$REPS, out=$OUT"
echo "### base_slice_ns=$(cat /sys/kernel/debug/sched/base_slice_ns)"
for e in "${W[@]}"; do
  IFS='|' read -r n m d c x <<< "$e"
  echo "--- $n ---"
  for rep in $(seq 1 "$REPS"); do
    for a in pv mig; do
      arm $a || { echo "  arm $a failed"; continue; }
      prep "$n"
      m0=$(python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$' || echo 0)
      if [ "$x" = x ]; then
        s=$(date +%s%N); ( cd "$d" && eval "$c" >/dev/null 2>&1 ); t=$(date +%s%N)
        v=$(python3 -c "print(f'{($t-$s)/1e9:.3f}')")
      else
        v=$( cd "$d" && eval "$c" 2>&1 | eval "$x" | head -1 )
      fi
      m1=$(python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$' || echo 0)
      printf "%s\t%s\t%s\t%s\t%d\t%s\t%d\n" "$PH" "$n" "$m" "$a" "$rep" "${v:-NA}" "$((m1-m0))" >> "$OUT"
      printf "  rep%d %-4s %-12s mig=%d\n" "$rep" "$a" "${v:-NA}" "$((m1-m0))"
    done
  done
  python3 $T/csmin_report.py "$OUT" "$n"
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference
echo "CSMIN_PHASE_DONE $OUT"
