#!/bin/bash
# pass 3 -- the AUTHORITATIVE mechanism pass. Low-perturbation instrument
# (migcost_light.bt: every probe fires at the migration rate, none inside
# my_spinlock) and BOTH arms traced, so the rwsem and syscall terms can be
# differenced the same way the qspinlock counter already is.
set -u
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
echo "============ nhextend_fin (pass3, light instrument, both arms) ============"
bash /root/ivh_tools/spotlight.sh nhextend_fin 3
echo "============ ebizzy_mmap (pass3, light instrument, both arms, warm-up + 5) ============"
WARMUP=1 bash /root/ivh_tools/spotlight.sh ebizzy_mmap 5
echo "============ PASS3 DONE $(date -Is) ============"
