#!/bin/bash
# acq_addendum.sh -- LONG variants of the two sub-second registry workloads.
#
# WHY. As invoked in campaign/benchmarks.tsv:
#   sysbench_mutex  --threads=16 --mutex-num=16 --mutex-locks=40000
#                   runs 0.31s and completes SIXTEEN events (one per thread).
#   fsmark_tmpfs    -D 16 -n 2000 -s 4096 -t 16 -L 1
#                   runs 0.14s -- tmpfs absorbs 32,000 files at 294k files/s.
# A lock RATE measured over a 0.15-0.3s window is dominated by thread creation,
# mmap and exit, not by the path the benchmark is named after. These variants
# do the SAME work, scaled to a ~15s window, so the rate reflects the workload.
# The registry is NOT modified -- these are reported as separate rows.
#
# sysbench scales by parameter (more locks, same benchmark). fsmark cannot:
# -n/-L scale the FILE COUNT, and 100 iterations of 32,000 x 4096B needs ~13GB
# of /dev/shm, so it is looped instead, with the directory wiped between
# iterations INSIDE the traced window (the wipe is part of what is counted --
# stated here rather than hidden).
set -u
exec env LIST="sysbench_mutex_long fsmark_long" \
     REPS="${REPS:-3}" MAXREPS="${MAXREPS:-7}" \
     OUT="${OUT:-/root/ivh_tools/acqadd_$(date +%m%d-%H%M%S).csv}" \
     ADDENDUM=1 bash /root/ivh_tools/acq_suite2.sh
