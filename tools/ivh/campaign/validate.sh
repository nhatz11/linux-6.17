#!/bin/bash
# Run every workload once, verify the extractor yields a number. No mode switching.
set -u
LIST=${LIST:-/root/ivh_tools/campaign/benchmarks.tsv}
OUT=/root/ivh_tools/campaign/validation.txt
mkdir -p /dev/shm/fiobench /dev/shm/fsmark /root/dbench_test
pgrep -x netserver >/dev/null || (netserver -p 12865 >/dev/null 2>&1 &)
pgrep -x iperf3   >/dev/null || (setsid nohup iperf3 -s >/dev/null 2>&1 &)
sleep 2
: > "$OUT"
while IFS=$'\t' read -r name dir to cmd ext hl; do
    [ -z "${name:-}" ] && continue
    case "$name" in \#*) continue;; esac
    if [ ! -d "$dir" ]; then echo "MISSINGDIR $name $dir" | tee -a "$OUT"; continue; fi
    t0=$(date +%s)
    v=$(cd "$dir" && timeout "$to" bash -c "$cmd" 2>/dev/null | eval "$ext" 2>/dev/null | head -1)
    t1=$(date +%s)
    if [ -z "$v" ]; then echo "FAIL       $name ($((t1-t0))s)" | tee -a "$OUT"
    else echo "OK         $name = $v ($((t1-t0))s)" | tee -a "$OUT"; fi
done < "$LIST"
echo "=== validation done ===" | tee -a "$OUT"
