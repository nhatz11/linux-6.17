#!/bin/bash
# dedup at one threshold/reference, via the VALIDATED parsec_ab.sh harness
# (per-run cache drop, discarded warmup, alternating arm order).
# Usage: dedup_phase.sh <A|B|C> [PAIRS]
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
case "${1:?phase}" in
  A) TH=4000000; G2=0 ;;  B) TH=2500000; G2=0 ;;  C) TH=2500000; G2=1 ;;
  *) echo "phase must be A|B|C"; exit 1 ;;
esac
bash $T/p7v2_arm.sh "$TH" >/dev/null 2>&1 || { echo "arm failed"; exit 1; }
echo "$G2" > $S/ivh_cs_gate2_reference
for k in ivh_time_left_threshold_ns:$TH ivh_cs_gate2_reference:$G2 ivh_preempt_event_source:2 ivh_rcu_guard:0; do
  n=${k%%:*}; w=${k##*:}; g=$(cat $S/$n)
  [ "$g" = "$w" ] || { echo "FATAL $n=$g want $w"; exit 1; }
done
echo "### dedup phase $1: THRESH=$TH gate2_ref=$G2"
PKGS=dedup PAIRS="${2:-3}" NTH=16 INPUT=native CFG=gcc bash $T/parsec_ab.sh
