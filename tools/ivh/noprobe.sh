#!/bin/bash
# noprobe.sh -- does the reported benefit come back without the probes?
#
# Points 7 and 8 attach bpftrace (kprobe on bpf_sched_pre_lock_migrate +
# kretprobe + 2 tracepoints) and a 1 ms /proc/kcore sampler to EVERY arm. That
# was done so the baseline is not advantaged -- but it does NOT equalise cost:
# bpf_sched_pre_lock_migrate is never CALLED in the pv arm (universal_eligible=0),
# so the kprobe fires 8k-100k/s in the IVH arms and 0/s in pv. The probe tax is
# therefore one-sided by construction, and this run measures how large it is.
#
#   A  pv          stock PV, no probes          (pvbase.sh)
#   B  best        best params, NO probes       <- the publishable number
#   C  best_probed best params, probes attached <- B - C is the probe tax
#
# Everything else is identical: same arms, same order rotation, same prep,
# same drop_caches, same suite.
set -u
T=/root/ivh_tools
source $T/suite14.sh
R="python3 $T/read_ivh_counters.py"
TLT="${TLT:-8000000}"
MC="${MC:-8}"
REPS="${REPS:-3}"
BTF_ID=66718
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
OUT="${OUT:-$T/noprobe_$(date +%m%d-%H%M%S).csv}"
CTRS="ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_prelock_calls"

[ -n "${DROP:-}" ] && { k=(); for e in "${SUITE14[@]}"; do n="${e%%|*}"; case " $DROP " in *" $n "*) ;; *) k+=("$e");; esac; done; SUITE14=("${k[@]}"); }

setarm(){
  case "$1" in
    pv) bash $T/p78_arm.sh pv >/dev/null ;;
    *)  bash $T/p78_arm.sh tlt "$TLT" >/dev/null || return 1
        echo "$MC" > /proc/sys/kernel/ivh_max_concurrent
        [ "$(cat /proc/sys/kernel/ivh_max_concurrent)" = "$MC" ] || { echo "FATAL: mc"; return 1; } ;;
  esac
}
ARMS=(pv best best_probed)
echo "workload,arm,rep,perf,dur_s,migs,wait_ns,node_iters,head_iters,prelock" > "$OUT"
echo "== noprobe: tlt=$TLT mc=$MC, ${#ARMS[@]} arms x ${#SUITE14[@]} workloads x $REPS reps -> $OUT"

for e in "${SUITE14[@]}"; do
  IFS='|' read -r wl dir met cmd ext <<< "$e"
  [ "$cmd" = "MEMTIER_CMD" ] && cmd="$MT"
  echo "########## $wl ##########"
  for r in $(seq 1 "$REPS"); do
    off=$(( (r-1) % 3 ))
    for i in 0 1 2; do
      a=${ARMS[$(( (i+off) % 3 ))]}
      setarm "$a" || continue
      bpftool link list 2>/dev/null | grep -q "target_btf_id $BTF_ID" || { echo "FATAL: selector gone"; exit 1; }
      prep14 "$wl" >/dev/null 2>&1 || continue
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      $R $CTRS > /tmp/n0.$$ 2>/dev/null; m0=$(python3 $T/migcount.py 2>/dev/null||echo 0)
      BT=""; SC=""
      if [ "$a" = best_probed ]; then
        timeout 400 bpftrace $T/migtime.bt > /tmp/nb.$$ 2>&1 & BT=$!
        for _ in $(seq 1 100); do grep -q Attaching /tmp/nb.$$ 2>/dev/null && break; sleep 0.2; done
        timeout 300 python3 $T/sample_atomic.py ivh_in_schedule 300 0.001 >/dev/null 2>&1 & SC=$!
      fi
      t0=$(date +%s.%N)
      if [ "$met" = TIME ]; then ( cd "$dir" && eval "$cmd" ) >/dev/null 2>&1; v=""
      else v=$( ( cd "$dir" && eval "$cmd" 2>&1 ) | eval "$ext" | tail -1 ); fi
      t1=$(date +%s.%N)
      [ -n "$SC" ] && { kill -INT $SC 2>/dev/null; wait $SC 2>/dev/null; }
      [ -n "$BT" ] && { kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null; }
      m1=$(python3 $T/migcount.py 2>/dev/null||echo 0); $R $CTRS > /tmp/n1.$$ 2>/dev/null
      python3 - "$wl" "$a" "$r" "${v:-}" "$met" "$t0" "$t1" "$((m1-m0))" /tmp/n0.$$ /tmp/n1.$$ "$OUT" <<'PY'
import sys,re
wl,a,r,v,met,t0,t1,mig,f0,f1,out=sys.argv[1:12]
dur=float(t1)-float(t0)
c=lambda p:{m.group(1):int(m.group(2)) for m in (re.match(r'\s*(\S+)\s*=\s*(\d+)',l) for l in open(p)) if m}
c0,c1=c(f0),c(f1); D=lambda k:c1.get(k,0)-c0.get(k,0)
perf=v if v else f"{dur:.4f}"
open(out,'a').write(f"{wl},{a},{r},{perf},{dur:.3f},{mig},{D('ivh_slowpath_wait_ns')},"
  f"{D('ivh_node_spin_iters_sum')+D('ivh_node_spin_success_iters_sum')},"
  f"{D('ivh_head_spin_iters_sum')+D('ivh_head_spin_iters_bail_sum')},{D('ivh_prelock_calls')}\n")
print(f"  {wl:20} {a:>12} r{r} perf={perf:>12} migs={mig:>7} wait={D('ivh_slowpath_wait_ns')/1e9:7.3f}s")
PY
      rm -f /tmp/n0.$$ /tmp/n1.$$ /tmp/nb.$$
    done
  done
done
bash $T/pvbase.sh >/dev/null 2>&1
echo "WROTE $OUT"; echo NOPROBE-DONE
