#!/bin/bash
# Campaign status at a glance.
R=${1:-/root/ivh_tools/campaign/run_main}
echo "=== $R ==="
[ -f "$R/ABORTED" ] && { echo "ABORTED: $(cat $R/ABORTED)"; }
[ -f "$R/DONE" ] && echo "STATUS: COMPLETE" || echo "STATUS: running ($(pgrep -fc run_campaign.sh) proc)"
tot=$(awk -F'\t' 'NF && $1 !~ /^#/{n++} END{print n}' /root/ivh_tools/campaign/benchmarks.tsv)
echo "workloads done (3 screen blocks): $(awk -F, 'NR>1 && $2=="screen"{c[$1"_"$3]++} END{b=0; for(k in c) if(c[k]>=4) b++; print int(b/3)}' $R/results.csv 2>/dev/null) / $tot"
echo "rows: $(($(wc -l < $R/results.csv 2>/dev/null)-1))   failures: $(grep -c ',FAIL,' $R/results.csv 2>/dev/null)   skipped: $(wc -l < $R/skipped 2>/dev/null || echo 0)"
echo "kernel warnings logged: $(wc -l < $R/kernel_warnings.txt 2>/dev/null || echo 0)"
echo "--- last 3 lines ---"; tail -3 "$R/log" 2>/dev/null
echo "--- current standings ---"
python3 /root/ivh_tools/campaign/analyze.py "$R/results.csv" /root/ivh_tools/campaign/benchmarks.tsv 2>/dev/null | head -18
