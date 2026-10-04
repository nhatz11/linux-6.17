#!/bin/bash
# PARSEC lock-acquisition rates, run DIRECTLY -- bypassing ./bin/parsecmgmt.
#
# parsecmgmt is a shell harness that itself acquires ~600,000 spinlocks/s:
# `parsecmgmt -a status` with NO application work measured 599,374/s. Tracing
# `parsecmgmt -a run` therefore measures the wrapper, which is why all six
# packages landed within 1.6% of each other (807k-826k/s) regardless of what
# the application does. blackscholes via the harness read 825,500/s; run
# directly it is 107,980/s -- a 7.6x overstatement, ~87% harness.
#
# Invocations are the native.runconf run_exec/run_args for each package, with
# NTHREADS=16, executed from the package's run/ directory where the native
# input is already staged.
set -u
P=/root/parsec-benchmark
REPS="${REPS:-3}"
OUT="${OUT:-/root/ivh_tools/acq_parsec_$(date +%m%d-%H%M%S).csv}"
echo "workload,rep,seconds,total_acq,slowpath" > "$OUT"
one(){ # $1=pkgpath $2=label  $3..=argv after the binary
  local p="$1" lbl="$2"; shift 2
  local bin="$P/pkgs/$p/inst/amd64-linux.gcc/bin/$(basename $p)"
  [ -x "$bin" ] || { echo "  !! $lbl: no binary at $bin"; return; }
  [ -d "$P/pkgs/$p/run" ] || { echo "  !! $lbl: no run dir"; return; }
  for r in $(seq 1 "$REPS"); do
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    o=$( cd "$P/pkgs/$p/run" && bash /root/ivh_tools/lockrate.sh "$lbl" "$bin" "$@" 2>/dev/null )
    python3 - "$lbl" "$r" "$OUT" <<PY
import re,sys
o="""$o"""
lbl,r,out=sys.argv[1:4]
sec=re.search(r'wall ([0-9.]+)s',o); tot=re.search(r'TOTAL\s+(\d+)',o)
slow=sum(int(m) for m in re.findall(r'queued_spin_lock_slowpath\s+(\d+)',o))
if sec and tot: open(out,'a').write(f"{lbl},{r},{float(sec.group(1)):.3f},{tot.group(1)},{slow}\n")
PY
  done
  tail -$REPS "$OUT" | python3 -c "
import sys,statistics as st
a=[];s=[]
for l in sys.stdin:
    f=l.strip().split(',')
    if len(f)>=5 and float(f[2])>0: a.append(int(f[3])/float(f[2])); s.append(int(f[4])/float(f[2]))
if a: print(f'  {\"$lbl\":22} n={len(a)} acq={st.median(a):10,.0f}/s  slowpath={st.median(s):8,.0f}/s'
            f'  sec={st.median([1]):.0f}  spread={max(a)/max(min(a),1):.2f}x')"
}
one apps/blackscholes parsec_blackscholes 16 in_10M.txt prices.txt
one apps/swaptions    parsec_swaptions    -ns 128 -sm 1000000 -nt 16
one apps/vips         parsec_vips         im_benchmark orion_18000x18000.v output.v
one apps/ferret       parsec_ferret       corel lsh queries 50 20 16 output.txt
one apps/bodytrack    parsec_bodytrack    sequenceB_261 4 261 4000 5 0 16
echo "WROTE $OUT"; echo ACQ-PARSEC-DONE
