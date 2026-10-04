#!/bin/bash
# Corrected re-run: cache dropped per run, arm order alternated per pair.
# Fast packages first so the implausible ones (dedup, vips) are answered soonest.
set -u; T=/root/ivh_tools
for p in dedup vips blackscholes swaptions freqmine ferret canneal bodytrack; do
  PKGS="$p" PAIRS=6 NTH=16 INPUT=native CFG=gcc timeout 7200 $T/parsec_ab.sh
done
echo 1 > /proc/sys/kernel/ivh_universal_eligible
echo "REDO-ALL-DONE"
