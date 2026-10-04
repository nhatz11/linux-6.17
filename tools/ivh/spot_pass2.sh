#!/bin/bash
# pass 2 -- INSTRUMENTED (migcost.bt attached in the mig arm).
set -u
echo "============ nhextend_fin (pass2, instrumented) ============"
bash /root/ivh_tools/spotlight.sh nhextend_fin 3
echo "============ ebizzy_mmap (pass2, instrumented, warm-up + 5 reps) ============"
WARMUP=1 bash /root/ivh_tools/spotlight.sh ebizzy_mmap 5
echo "============ PASS2 DONE $(date -Is) ============"
