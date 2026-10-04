#!/bin/bash
# nh_thresh_derived.sh -- NHextend-fin across candidate time-left thresholds.
#
# BUG FIXED 2026-10-02: v1 took the flock itself, so the spotlight.sh it calls
# hit `flock -n` on the SAME file, printed FATAL, and never ran -- every row
# re-read the previous TSV and all four thresholds reported identical numbers.
# No lock here; spotlight.sh takes its own. The OUT path is parsed from
# spotlight.sh's own banner instead of `ls -t`, so a stale file cannot be read.
#
# Candidates, under the professor's framing
#   time-to-preemption = w + CSmin + delta,  delta = 1 ms (upper bound on
#   prediction error AND migration cost)
#   kernel  : w 1.5ms + CSmin~0 + delta 1ms   = 2.5 ms
#   NHextend: threshold = CSmin + delta       = 908.6us + 1000us = 1909 us
# 800 us is the empirical best found so far; 1500 us is the value to check.
set -u
T=/root/ivh_tools
REPS="${1:-3}"
echo "CSmin measured for NHextend-fin @ loop_spin=600000: 908600 ns"
for TH in 1000000 1909000 2500000; do
  echo "######### THRESH = $TH ns #########"
  log=$(THRESH=$TH NOBT=1 bash $T/spotlight.sh nhextend_fin "$REPS" 2>&1)
  echo "$log" | grep -E "^  rep|FATAL|ARMFAIL"
  f=$(echo "$log" | grep -oP '\-> \K/root/ivh_logs/\S+\.tsv' | head -1)
  [ -n "$f" ] && [ -s "$f" ] || { echo "  !! no TSV produced -- spotlight.sh failed"; continue; }
  python3 - "$f" "$TH" <<'PY'
import sys,statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
pv=[float(x[3]) for x in r if x[1]=='pv']; mg=[float(x[3]) for x in r if x[1]=='mig']
mi=[int(x[9]) for x in r if x[1]=='mig']
upv=[float(x[10]) for x in r if x[1]=='pv']; umg=[float(x[10]) for x in r if x[1]=='mig']
n=min(len(pv),len(mg)); d=[mg[i]-pv[i] for i in range(n)]
t=st.mean(d)/(st.stdev(d)/n**0.5) if n>1 and st.stdev(d)>0 else float('nan')
norm=st.mean(mg)/st.mean(pv); saved=st.mean(upv)*norm-st.mean(umg)
print(f"  ==> {int(sys.argv[2])/1000:7.0f} us   PV {st.mean(pv):7.1f}  G {st.mean(mg):7.1f}"
      f"  benefit {100*(norm-1):+6.2f}%  t={t:+6.2f}  migs {st.mean(mi):8.0f}  saved {saved:6.2f}s"
      f"  [{sys.argv[1].split('/')[-1]}]")
PY
done
echo "DONE"
