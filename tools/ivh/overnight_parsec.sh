#!/bin/bash
# Waits for the psearchy A/B to finish, then:
#   Phase 1: time ONE run of each PARSEC candidate (migration off) to size them.
#   Phase 2: interleaved A/B on everything that lands in a measurable window.
# Robust to individual failures -- one bad package must not stop the rest.
set -u
T=/root/ivh_tools; export PARSECDIR=/root/parsec-benchmark
LOG=$T/overnight.log
exec > >(tee -a $LOG) 2>&1
echo "=== waiting for psearchy A/B ($(date -u +%H:%M:%S)) ==="
while ! grep -q "^DONE" $T/psab.log 2>/dev/null; do sleep 30; done
echo "psearchy finished."

cd $PARSECDIR
/root/spin_mode 1 > /dev/null; echo 0 > /proc/sys/kernel/ivh_universal_eligible

# raytrace excluded: needs a display, this is a headless CVM. x264 not built.
CAND="swaptions canneal blackscholes dedup vips streamcluster fluidanimate freqmine bodytrack ferret facesim"
echo; echo "=== PHASE 1: timing survey, native input, -n 16 ($(date -u +%H:%M:%S)) ==="
GOOD=""
for p in $CAND; do
  s=$(date +%s)
  timeout 1800 ./bin/parsecmgmt -a run -p $p -c gcc -i native -n 16 > /tmp/t_$p.log 2>&1
  rc=$?; d=$(( $(date +%s)-s ))
  if [ $rc -eq 0 ] && [ $d -ge 8 ]; then GOOD="$GOOD $p"; tag=OK
  elif [ $rc -ne 0 ]; then tag="FAILED rc=$rc"
  else tag="TOO SHORT"; fi
  printf "  %-15s %5ss  %s\n" "$p" "$d" "$tag"
  [ $rc -ne 0 ] && grep -iE "error|not found|cannot|No such" /tmp/t_$p.log | head -2 | sed 's/^/      /'
done
echo; echo "runnable:$GOOD"

echo; echo "=== PHASE 2: interleaved A/B, 6 pairs each ($(date -u +%H:%M:%S)) ==="
for p in $GOOD; do
  PKGS="$p" PAIRS=6 NTH=16 INPUT=native CFG=gcc timeout 14400 $T/parsec_ab.sh
done
echo 1 > /proc/sys/kernel/ivh_universal_eligible
echo; echo "=== ALL DONE $(date -u +%H:%M:%S) ==="
