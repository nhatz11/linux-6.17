#!/bin/bash
# spot25.sh -- the spotlight pair at ivh_time_left_threshold_ns = 2500000 (2.5ms),
# the value the professor fixed on 2026-10-02.
#
# Pass A is UNINSTRUMENTED and supplies the headline %. Pass B attaches
# migcost_light.bt in BOTH arms and supplies cost/delay/syscall. Never read a
# headline off pass B.
#
# ebizzy gets DROPC=1 (sync + drop_caches + sleep 1 before every run), matching
# noprobe.sh and fullstack.sh. NHextend does not need it -- it touches no files
# and its own wait counter is the metric.
set -u
export THRESH=2500000
echo "############ PASS A -- uninstrumented, THRESH=$THRESH ############"
NOBT=1              bash /root/ivh_tools/spotlight.sh nhextend_fin 3
NOBT=1 DROPC=1      bash /root/ivh_tools/spotlight.sh ebizzy_mmap  3
echo "############ PASS B -- light instrument, both arms ############"
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
                    bash /root/ivh_tools/spotlight.sh nhextend_fin 3
DROPC=1             bash /root/ivh_tools/spotlight.sh ebizzy_mmap  3
echo "############ SPOT25 DONE $(date -Is) ############"
