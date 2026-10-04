#!/bin/bash
# waitcost.sh [REPS] [ONLY]
#
# Performance vs WAIT COST, pv+t1 vs mig+t1, at THRESH=2.5ms with csmin.
#   wait cost = lock waiting time + total migration mechanism cost
#
# ARMS (tier1 is ON in BOTH; migration is the only difference):
#   pv_t1   pvbase.sh      -- tier1 on, tier2/evict/skip off, no migration,
#                             spin_threshold 32768. The user's stated baseline.
#   mig_t1  p7v2_arm 2.5ms -- same, PLUS migration, gate2_reference=1 (csmin),
#                             and tier2 FORCED OFF (p7v2 leaves it on, despite
#                             its own comment claiming tier1/tier2 are off).
#
# WAITING TIME is per-workload, because ivh_slowpath_wait_ns counts QSPINLOCKS
# ONLY. ebizzy -m serialises on mmap_lock, an rwsem, so its qspinlock wait is a
# <1% term and the wrong instrument; ebizzy's wait is measured separately with
# migcost_light.bt's rwsem probes. See that file's header.
#
# This pass is UNINSTRUMENTED: no bpftrace, because a probe on the migrate path
# changed migrations/run 1071->399 and the headline +17.5%->+7.0%. Cost per
# migration comes from a separate instrumented pass (waitcost_mech.sh).
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${1:-3}"; ONLY="${2:-}"
OUT=/root/ivh_logs/waitcost_$(date +%m%d-%H%M%S).tsv
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
NH="env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=1 /root/linux-6.17/NHextend-csmin -l -n 16"
CTRS="ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_slowpath_halt_ns"

# name|metric|dir|cmd|extractor   (TIME = lower better, THR = higher better)
W=("hackbench|TIME|/root|timeout 300 hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
   "nhextend|THR|/root|timeout 90 $NH|grep -oP 'Ran for \K[0-9]+'"
   "ebizzy|THR|/root|timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
   "fsmark|THR|/root|timeout 300 fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
   "memtier|THR|/root|timeout 120 $MT|grep -oP 'Totals\s+\K[0-9.]+'"
   "dedup|TIME|$P|timeout 600 ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16|x")
[ -n "$ONLY" ] && W=($(printf '%s\n' "${W[@]}" | grep -E "^($ONLY)\|"))

gv(){ python3 $T/read_ivh_counters.py $CTRS 2>/dev/null | awk -F= -v k="$1" '$1~k{gsub(/ /,"",$2);print $2}'; }
mig(){ python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$'; }

arm(){ case $1 in
    pv_t1)  bash $T/pvbase.sh >/dev/null 2>&1 || return 1 ;;
    mig_t1) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_pv_tier2_enable
            echo 1 > $S/ivh_cs_gate2_reference ;;
  esac
  echo 1 > $S/ivh_pv_tier1_enable
  # assert the contrast is exactly what it claims to be
  local e=0
  [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || { echo "  FATAL tier1 off"; e=1; }
  [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "  FATAL tier2 ON"; e=1; }
  [ "$(cat $S/ivh_pv_evict_enable)" = 0 ] || { echo "  FATAL evict ON"; e=1; }
  [ "$(cat $S/ivh_pv_spin_threshold)" = 32768 ] || { echo "  FATAL spin_threshold"; e=1; }
  case $1 in
    pv_t1)  [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "  FATAL pv: mig on"; e=1; } ;;
    mig_t1) [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  FATAL mig: off"; e=1; }
            [ "$(cat $S/ivh_time_left_threshold_ns)" = 2500000 ] || { echo "  FATAL thresh"; e=1; }
            [ "$(cat $S/ivh_cs_gate2_reference)" = 1 ] || { echo "  FATAL csmin not set"; e=1; }
            [ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "  FATAL preempt_src"; e=1; }
            [ "$(cat $S/ivh_rcu_guard)" = 0 ] || { echo "  FATAL rcu_guard"; e=1; } ;;
  esac
  [ $e = 0 ] || return 1
  sleep 1; }

prep(){ case $1 in
    fsmark)  rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
    memtier) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
             memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
    dedup)   sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null ;;  # I/O-heavy: 672MB ISO
  esac; }

printf "workload\tmetric\tarm\trep\tval\twait_ns\twait_ev\thalt_ns\tmigs\n" > "$OUT"
echo "### waitcost: THRESH=2500000 csmin=1 tier1=BOTH tier2=OFF reps=$REPS"
echo "### out=$OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n m d c x <<< "$e"
  echo "--- $n ---"
  [ "$n" = dedup ] && { arm pv_t1; prep dedup; ( cd "$d" && eval "$c" >/dev/null 2>&1 ); echo "  (dedup warmup discarded)"; }
  for rep in $(seq 1 "$REPS"); do
    # alternate order so neither arm is systematically second
    if [ $((rep % 2)) -eq 1 ]; then ORDER="pv_t1 mig_t1"; else ORDER="mig_t1 pv_t1"; fi
    for a in $ORDER; do
      arm $a || { echo "  arm $a FAILED -- skipping"; continue; }
      prep "$n"
      w0=$(gv ivh_slowpath_wait_ns); e0=$(gv ivh_slowpath_wait_events); h0=$(gv ivh_slowpath_halt_ns); m0=$(mig)
      if [ "$x" = x ]; then
        s=$(date +%s%N); ( cd "$d" && eval "$c" >/dev/null 2>&1 ); t=$(date +%s%N)
        v=$(python3 -c "print(f'{($t-$s)/1e9:.3f}')")
      else
        v=$( cd "$d" && eval "$c" 2>&1 | eval "$x" | head -1 )
      fi
      w1=$(gv ivh_slowpath_wait_ns); e1=$(gv ivh_slowpath_wait_events); h1=$(gv ivh_slowpath_halt_ns); m1=$(mig)
      printf "%s\t%s\t%s\t%d\t%s\t%d\t%d\t%d\t%d\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$((w1-w0))" "$((e1-e0))" "$((h1-h0))" "$((m1-m0))" >> "$OUT"
      printf "  rep%d %-7s %-12s wait=%.1fms ev=%d migs=%d\n" "$rep" "$a" "${v:-NA}" \
        "$(python3 -c "print(($w1-$w0)/1e6)")" "$((e1-e0))" "$((m1-m0))"
    done
  done
  python3 $T/waitcost_report.py "$OUT" "$n" 2>/dev/null
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference
echo "WAITCOST_DONE $OUT"
