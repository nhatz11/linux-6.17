#!/bin/bash
# Does enabling Gate 2 change anything?  ivh_preempt_event_source: 0 -> 2
#   0 = paravirt path, DEAD on this host (no KVM_FEATURE_STEAL_TIME) -> gate never rejects
#   2 = TSC path (ivh_vact_last_active_c), works without paravirt support
# Both arms: migration + AS. The ONLY variable is the gate-2 source.
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
CSV=gate2_check_$(date +%m%d_%H%M%S).csv
echo "workload,rep,gate2,value,tl_rejects,cap_rejects,migrations" > $CSV
rej(){ python3 - <<'PY'
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    tl=sum(r.read_u64(f,ph,sym["ivh_steal_imminent_time_left_reject"]+o) for o in offs)
    cp=sum(r.read_u64(f,ph,sym["ivh_steal_imminent_capacity_reject"]+o) for o in offs)
print(f"{tl} {cp}")
PY
}
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
run(){ case $1 in
  hackbench)       /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
  perf_sched_pipe) perf bench sched pipe -l 300000 2>&1 | grep -oP '^\s*\K[0-9]+(?= ops/sec)';;
  ebizzy_mmap)     /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null | grep -oP '^\K[0-9]+(?= records/s)';;
  dedup|blackscholes) s=$(date +%s.%N)
     (cd $PARSECDIR && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16) >/dev/null 2>&1
     e=$(date +%s.%N); echo "$e-$s"|bc;;
esac; }
for w in hackbench perf_sched_pipe ebizzy_mmap blackscholes dedup; do
  echo "########## $w ##########"
  for r in 1 2 3; do
    [ $((r%2)) -eq 1 ] && ORDER="0 2" || ORDER="2 0"
    for g in $ORDER; do
      echo "$g" > $S/ivh_preempt_event_source
      [ "$(cat $S/ivh_preempt_event_source)" = "$g" ] || { echo "write FAILED"; exit 1; }
      sync; echo 3 > /proc/sys/vm/drop_caches
      read t0 c0 <<< "$(rej)"; m0=$(mig)
      v=$(run $w)
      read t1 c1 <<< "$(rej)"; m1=$(mig)
      echo "$w,$r,$g,${v:-FAIL},$((t1-t0)),$((c1-c0)),$((m1-m0))" | tee -a $CSV
    done
  done
done
echo 0 > $S/ivh_preempt_event_source
echo "WROTE $CSV"; echo GATE2-CHECK-DONE
