#!/bin/bash
# 2026-09-25 re-run of the three migration-alone workloads with the G-LOCK-39
# sampler OFF. The 2026-09-23 numbers (tinyconfig +7.16%, psearchy +5.24%,
# PARSEC dedup +77.55% etc.) were all taken with ivh_tks_sampler_ns=200000,
# which costs the arm that cannot mitigate lock-holder preemption up to 48%.
# Small wins are the most at risk; this re-measures all of them.
set -u
T=/root/ivh_tools
LOG=/root/ivh_tools/redo_all_$(date +%m%d_%H%M%S).log
exec > >(tee -a "$LOG") 2>&1
echo "=== redo start $(date) ==="
source $T/bench_guard.sh

echo; echo "########## 1/3 tinyconfig kernel build ##########"
timeout 5400 $T/tinyconfig_ab.sh || echo "tinyconfig exited $?"

echo; echo "########## 2/3 psearchy (MOSBench pedsort) ##########"
timeout 5400 $T/psearchy_ab.sh || echo "psearchy exited $?"

echo; echo "########## 3/3 PARSEC (8 packages, 6 pairs each) ##########"
timeout 43200 $T/parsec_redo.sh || echo "parsec exited $?"

echo; echo "=== redo complete $(date) ==="
echo "REDO-ALL-NOSAMPLER-DONE"
