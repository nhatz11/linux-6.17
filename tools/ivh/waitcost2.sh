#!/bin/bash
# waitcost2.sh [BLOCKS] [PER] [ONLY]  -- the final table.
#
# vs waitcost.sh: BLOCK design with a DISCARDED WARMUP after every arm switch.
# That warmup is mandatory: without it ebizzy's first run in a fresh arm engages
# the mechanism 3x less (10,302 vs 30,514 migrations) and the arm reads +0.57%
# instead of +52%. Harmless on the others, so it is applied uniformly.
# Blocks alternate order so neither arm is systematically second.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
BLOCKS="${1:-2}"; PER="${2:-2}"; ONLY="${3:-}"
OUT=/root/ivh_logs/waitcost2_$(date +%m%d-%H%M%S).tsv
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
NH="env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=1 /root/linux-6.17/NHextend-csmin -l -n 16"
W=("hackbench|TIME|/root|timeout 300 hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
   "nhextend|THR|/root|timeout 90 $NH|grep -oP 'Ran for \K[0-9]+'"
   "ebizzy|THR|/root|timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
   "fsmark|THR|/root|timeout 300 fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
   "memtier|THR|/root|timeout 120 $MT|grep -oP 'Totals\s+\K[0-9.]+'"
   "dedup|TIME|$P|timeout 600 ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16|x")
[ -n "$ONLY" ] && W=($(printf '%s\n' "${W[@]}" | grep -E "^($ONLY)\|"))
gv(){ python3 $T/read_ivh_counters.py ivh_slowpath_wait_ns ivh_slowpath_wait_events 2>/dev/null | awk -F= -v k="$1" '$1~k{gsub(/ /,"",$2);print $2}'; }
mig(){ python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$'; }
arm(){ case $1 in
    pv_t1)  bash $T/pvbase.sh >/dev/null 2>&1 || return 1 ;;
    mig_t1) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
  esac
  echo 1 > $S/ivh_pv_tier1_enable
  local e=0
  [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || e=1
  [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || e=1
  [ "$(cat $S/ivh_pv_spin_threshold)" = 32768 ] || e=1
  case $1 in
    pv_t1)  [ "$(cat $S/ivh_universal_eligible)" = 0 ] || e=1 ;;
    mig_t1) [ "$(cat $S/ivh_universal_eligible)" = 1 ] || e=1
            [ "$(cat $S/ivh_time_left_threshold_ns)" = 2500000 ] || e=1
            [ "$(cat $S/ivh_cs_gate2_reference)" = 1 ] || e=1
            [ "$(cat $S/ivh_rcu_guard)" = 0 ] || e=1 ;;
  esac
  [ $e = 0 ] || { echo "  FATAL arm $1"; return 1; }
  sleep 1; }
prep(){ case $1 in
    fsmark)  rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
    memtier) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
             memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
    dedup)   sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null ;;
  esac; }
run1(){ local d="$1" c="$2" x="$3"
  if [ "$x" = x ]; then local s t; s=$(date +%s%N); ( cd "$d" && eval "$c" >/dev/null 2>&1 ); t=$(date +%s%N)
    python3 -c "print(f'{($t-$s)/1e9:.3f}')"
  else ( cd "$d" && eval "$c" 2>&1 | eval "$x" | head -1 ); fi; }
printf "workload\tmetric\tarm\tblock\trep\tval\twait_ns\twait_ev\tmigs\n" > "$OUT"
echo "### waitcost2: 2.5ms csmin tier1-BOTH tier2-OFF  blocks=$BLOCKS per=$PER  WARMUP per arm"
echo "### out=$OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n m d c x <<< "$e"
  echo "--- $n ---"
  for b in $(seq 1 "$BLOCKS"); do
    if [ $((b % 2)) -eq 1 ]; then ORDER="pv_t1 mig_t1"; else ORDER="mig_t1 pv_t1"; fi
    for a in $ORDER; do
      arm $a || continue
      prep "$n"; run1 "$d" "$c" "$x" >/dev/null 2>&1      # WARMUP, discarded
      for r in $(seq 1 "$PER"); do
        prep "$n"
        w0=$(gv ivh_slowpath_wait_ns); e0=$(gv ivh_slowpath_wait_events); m0=$(mig)
        v=$(run1 "$d" "$c" "$x")
        w1=$(gv ivh_slowpath_wait_ns); e1=$(gv ivh_slowpath_wait_events); m1=$(mig)
        printf "%s\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\n" "$n" "$m" "$a" "$b" "$r" "${v:-NA}" \
          "$((w1-w0))" "$((e1-e0))" "$((m1-m0))" >> "$OUT"
        printf "  b%d %-7s r%d %-11s wait=%.0fms migs=%d\n" "$b" "$a" "$r" "${v:-NA}" \
          "$(python3 -c "print(($w1-$w0)/1e6)")" "$((m1-m0))"
      done
    done
  done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference
echo "WAITCOST2_DONE $OUT"
