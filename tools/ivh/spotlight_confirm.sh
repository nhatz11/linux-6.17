#!/bin/bash
# spotlight_confirm.sh -- 3x(pv,mig) on the two spotlight workloads, sequential.
# Each clean_one.sh invocation takes/releases the flock itself.
set -u
for w in ebizzy_mmap nhextend_fin; do
  echo "============ $w ============"
  bash /root/ivh_tools/clean_one.sh "$w" 3
done
echo "============ DONE $(date -Is) ============"
