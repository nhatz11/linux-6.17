#!/bin/bash
# pass 1 -- UNINSTRUMENTED. The headline % must never be read off a traced run.
set -u
for w in nhextend_fin ebizzy_mmap; do
  echo "============ $w (pass1, no bpftrace) ============"
  NOBT=1 bash /root/ivh_tools/spotlight.sh "$w" 3
done
echo "============ PASS1 DONE $(date -Is) ============"
