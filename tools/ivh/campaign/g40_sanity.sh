#!/bin/bash
# G-LOCK-40 sanity: 1 pv + 1 ivh(migration+AS) on the KNOWN-GOOD workloads.
# Purpose is only "did the RCU fix break or change a known win". Not statistics.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 2 > $S/ivh_pv_preempt_src
arm(){ case $1 in
  pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "pv arm failed"; exit 1; } ;;
  ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "ivh arm failed"; exit 1; } ;;
esac; }
run(){ case $1 in
  perf_sched_pipe) perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)';;
  ebizzy_mmap)     /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)';;
  stressng_dentry) stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+';;
  fsmark_tmpfs)    rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
                   fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1 2>/dev/null | grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+' | tail -1;;
  hackbench)       /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
esac; }
# reference = 2026-09-15 campaign (mig+AS, G-LOCK-30, sampler did not exist)
declare -A REF=( [perf_sched_pipe]=146.9 [ebizzy_mmap]=104.3 [stressng_dentry]=99.5 [fsmark_tmpfs]=167.0 [hackbench]=76.3 )
declare -A HI=(  [perf_sched_pipe]=1 [ebizzy_mmap]=1 [stressng_dentry]=1 [fsmark_tmpfs]=1 [hackbench]=0 )
printf "%-18s %12s %12s %10s %12s\n" workload pv ivh delta "ref(09-15)"
for w in perf_sched_pipe ebizzy_mmap stressng_dentry fsmark_tmpfs hackbench; do
  arm pv;  sync; echo 3 > /proc/sys/vm/drop_caches; p=$(run $w)
  arm ivh; sync; echo 3 > /proc/sys/vm/drop_caches; i=$(run $w)
  if [ -n "$p" ] && [ -n "$i" ]; then
    if [ "${HI[$w]}" = 1 ]; then d=$(python3 -c "print(f'{100*($i-$p)/$p:+.1f}%')")
    else                        d=$(python3 -c "print(f'{100*($p-$i)/$p:+.1f}%')"); fi
  else d="FAIL"; fi
  printf "%-18s %12s %12s %10s %11s%%\n" "$w" "$p" "$i" "$d" "+${REF[$w]}"
done
echo G40-SANITY-DONE
