#!/bin/bash
# T1 + recovery curve: 8 loaded IVH+AS hackbench rounds with a 5 s background
# sampler, then keep sampling through IDLE_S seconds of idle.
set -u
ROUNDS=${ROUNDS:-8}; IDLE_S=${IDLE_S:-360}; S=/proc/sys/kernel
TAG=${TAG:-t1}; OUT=/root/ivh_tools/drift_${TAG}_$(date +%H%M%S)
W="hackbench -T -g1 -f8 -l400000"
MODE=${MODE:-ivhas}
if [ "$MODE" = pv ]; then echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null; WANT=0; else echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; WANT=2; fi
[ "$(cat $S/ivh_adaptive_mode)" = "$WANT" ] || { echo FATAL mode; exit 1; }
dmesg -n 1
( while :; do python3 /root/ivh_tools/drift_snap.py sample; sleep 5; done ) > $OUT.samples.jsonl 2>/dev/null &
SP=$!; trap 'kill $SP 2>/dev/null; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null' EXIT
python3 /root/ivh_tools/drift_snap.py start > $OUT.rounds.jsonl
for i in $(seq 1 $ROUNDS); do
    t0=$(date +%s.%N)
    v=$($W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "round $i time=${v}s start=$t0" | tee -a $OUT.log
    python3 /root/ivh_tools/drift_snap.py "round$i:$v" >> $OUT.rounds.jsonl
done
echo "idle sampling ${IDLE_S}s" | tee -a $OUT.log
sleep $IDLE_S
echo "done -> $OUT.*" | tee -a $OUT.log
