#!/bin/bash
# ebizzy_bothmetrics.sh -- settle ebizzy's spin verdict WITHOUT a kernel build, by
# measuring it with BOTH metrics at once:
#
#   A = (ivh_slowpath_wait_ns - ivh_slowpath_halt_ns) / entries
#       COMPLETE SCOPE: brackets the whole slowpath frame, so it includes HEAD
#       spin. Contaminated by host preemption charged to spin, hence per-entry.
#   B = node_spin_iters / entries
#       EXACT but NODE ONLY; blind to 100% of ebizzy's 1.39M head tenures.
#
# ebizzy is head-dominated (3.16 head tenures per node attempt), so B sees the
# minority of its spinning. If A shows a saving where B does not, the saving is on
# the head path. If neither moves, ebizzy genuinely does not save spin.
#
# hackbench is the positive control: both metrics agreed there before.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
REPS="${REPS:-8}"
OUT=/root/ivh_logs/restmetrics_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_enter ivh_cs_head_bailed ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1; fi; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 2400 9 || { echo FATAL; exit 1; }
printf "wl\tarm\trep\tval\twait_ns\thalt_ns\tentries\tnode_iters\thead_ten\tcsb\tt2f\n" > "$OUT"
echo "### both-metrics, n=$REPS -> $OUT"
for wl in memtier dbench vips; do
 echo "########## $wl ##########"
 [ "$wl" = vips ] && ( cd /root/parsec-benchmark/pkgs/apps/vips/run && IM_CONCURRENCY=16 /root/parsec-benchmark/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
 for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
   arm "$a" || { echo "  ARMFAIL"; continue; }
   sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
   b=($(snap))
   case $wl in
    memtier) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
      memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2
      v=$( timeout 120 /root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 \
           -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram 2>&1 \
           | grep -oP 'Totals\s+\K[0-9.]+' | head -1 ) ;;
    dbench) rm -rf /root/dbench_test; mkdir -p /root/dbench_test
      v=$( timeout 180 dbench -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+' | head -1 ) ;;
    vips) v=$( cd /root/parsec-benchmark/pkgs/apps/vips/run && t0=$(date +%s%N); \
           IM_CONCURRENCY=16 /root/parsec-benchmark/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips \
           im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1; t1=$(date +%s%N); \
           python3 -c "print(f'{($t1-$t0)/1e9:.3f}')" ) ;;
   esac
   f=($(snap))
   printf "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n" "$wl" "$a" "$rep" "${v:-NA}" \
     "$(( ${f[0]}-${b[0]} ))" "$(( ${f[1]}-${b[1]} ))" "$(( ${f[2]}-${b[2]} ))" \
     "$(( (${f[3]}-${b[3]}) + (${f[4]}-${b[4]}) ))" "$(( ${f[5]}-${b[5]} ))" \
     "$(( ${f[6]}-${b[6]} ))" "$(( ${f[7]}-${b[7]} ))" >> "$OUT"
   echo "  rep$rep $a $wl val=${v:-NA}"
  done
 done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 - "$OUT" <<'PY'
import sys, statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
for wl in ('memtier','dbench','vips'):
    wr=[x for x in r if x[0]==wl and x[3] not in ('NA','')]
    by={}
    for x in wr: by.setdefault(x[2],{})[x[1]]=x
    A=[];B=[];HT=[]
    for rep,g in by.items():
        if 'pv' not in g or 'as' not in g: continue
        p,a=g['pv'],g['as']
        a0=(int(p[4])-int(p[5]))/max(int(p[6]),1); a1=(int(a[4])-int(a[5]))/max(int(a[6]),1)
        b0=int(p[7])/max(int(p[6]),1);             b1=int(a[7])/max(int(a[6]),1)
        A.append(100*(a0-a1)/a0 if a0 else 0); B.append(100*(b0-b1)/b0 if b0 else 0)
        HT.append(100*(int(p[8])-int(a[8]))/max(int(p[8]),1))
    n=len(A)
    if n<2: continue
    tc={3:4.30,4:3.18,5:2.78,6:2.57,7:2.45,8:2.36}.get(n,2.36)
    def ci(v):
        m=st.mean(v); se=st.stdev(v)/n**0.5; return m, m-tc*se, m+tc*se
    print(f"\n=== {wl}, n={n} paired  (+ = AS spins LESS) ===")
    for lbl,v in (("A wall/entry (NODE+HEAD)",A),("B iters/entry (NODE only)",B),("head tenures",HT)):
        m,lo,hi=ci(v)
        sig = "SIGNIFICANT" if (lo>0 or hi<0) else "ns (CI spans 0)"
        print(f"  {lbl:<26s} mean {m:+7.2f}%  CI [{lo:+7.2f},{hi:+7.2f}]  {sig}")
        print(f"  {'':26s} per-rep {[round(z,1) for z in v]}")
PY
echo "RESTMETRICS_DONE $OUT"
