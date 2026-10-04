#!/bin/bash
# Wait for the running point-7 sweep (pid 175390) to exit, then run point 8.
while kill -0 175390 2>/dev/null; do sleep 30; done
sleep 10
cd /root/ivh_tools && MODE=p8 REPS=3 bash p78.sh > /root/ivh_tools/p8_run.log 2>&1
