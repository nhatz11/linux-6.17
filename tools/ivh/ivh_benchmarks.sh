# ivh_benchmarks.sh -- THE confirmed-win workload set. Source this; do not
# invent workloads elsewhere.
#
# Source of truth for SET A is ivh_tools/campaign/benchmarks.tsv, the registry
# the campaign harness actually ran (campaign/run_main/results.csv holds its
# 1300 measurements). Set A below is generated verbatim from it.
#
# ---------------------------------------------------------------------------
# SET A -- the 19 confirmed wins, campaign section 6.
#
# GENERATED VERBATIM FROM ivh_tools/campaign/benchmarks.tsv, which is the
# registry the campaign harness actually ran. That file is the ONLY authority
# for these invocations. Earlier versions of this list were assembled from
# screen/mig_screen.sh and campaign/fullstack.sh instead, and 5 of 19 entries
# were wrong as a result -- dbench_16 was missing -F (no fsync: a materially
# different workload), sysbench_mutex had --threads=32 --mutex-locks=20000
# instead of 16/40000, schbench had -m 4 -t 4 and the wrong extractor,
# wis_mmap2 had -s 10 instead of -s 15, perf_sched_pipe had -l 400000
# instead of -l 300000.
#
# Format: name | dir | direction | command | extractor | recorded
IVH_WORKLOADS=(
"fsmark_tmpfs|/root|hi|fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'|+167.0"
"perf_sched_pipe|/root|hi|perf bench sched pipe -l 300000|grep -oP '^\s*\K[0-9]+(?= ops/sec)'|+146.9"
"ebizzy_mmap|/root|hi|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'|+104.3"
"stressng_dentry|/root|hi|stress-ng --dentry 16 -t 15s --metrics-brief|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+99.5"
"hackbench_pipe_thr|/root|lo|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'|+76.3"
"hackbench_sock_thr|/root|lo|hackbench -T -s 512 -g1 -f8 -l100000|grep -oP '^Time:\s*\K[0-9.]+'|+75.4"
"hackbench_pipe_proc|/root|lo|hackbench -p -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'|+61.8"
"perf_epoll_wait|/root|hi|perf bench epoll wait -t 16 -r 15|grep -oP 'Averaged\s+\K[0-9]+'|+53.9"
"stressng_flock|/root|hi|stress-ng --flock 16 -t 15s --metrics-brief|grep -oP 'flock\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+44.2"
"sysbench_mutex|/root|lo|sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=40000 run|grep -oP 'total time:\s*\K[0-9.]+'|+24.4"
"stressng_mmap|/root|hi|stress-ng --mmap 16 -t 15s --metrics-brief|grep -oP 'mmap\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+23.4"
"stressng_sock|/root|hi|stress-ng --sock 16 -t 15s --metrics-brief|grep -oP 'sock\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+19.7"
"dbench_16|/root|hi|dbench -F -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|+19.1"
"stressng_pipe|/root|hi|stress-ng --pipe 16 -t 15s --metrics-brief|grep -oP 'pipe\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+15.8"
"wis_mmap2|/root/bench/will-it-scale|hi|./mmap2_threads -t 16 -s 15|grep -oP 'average:\K[0-9]+'|+11.2"
"wis_mmap1|/root/bench/will-it-scale|hi|./mmap1_threads -t 16 -s 15|grep -oP 'average:\K[0-9]+'|+10.9"
"stressng_futex|/root|hi|stress-ng --futex 16 -t 15s --metrics-brief|grep -oP 'futex\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+10.5"
"perf_syscall_basic|/root|hi|perf bench syscall basic -l 30000000|grep -oP '\K[0-9]+(?= ops/sec)'|+8.7"
"schbench|/root|hi|bash -c '/root/bench/schbench/schbench -m 2 -t 8 -r 15 2>&1'|grep -oP 'average rps:\s*\K[0-9.]+'|+7.4"
)

# ---------------------------------------------------------------------------
# SET B -- "Migration alone", evaluation.md section 10.2
# (RE-MEASURED 2026-09-25, sampler off). 10 workloads: 8 PARSEC + psearchy +
# kernel build. Harness: ivh_tools/redo_all_nosampler.sh ->
# {tinyconfig_ab.sh, psearchy_ab.sh, parsec_redo.sh -> parsec_ab.sh}.
#
# DIFFERENT ARMS FROM SET A. Set A is the full IVH stack. Set B is migration
# ALONE on stock upstream PV spinning:
#     both arms  /root/spin_mode 1   and   ivh_pv_preempt_src=0
#     ivh_universal_eligible is the ONLY variable
# For probing Gate 2 / Gate 4 this is the cleaner isolation, since those are
# migration gates -- but it is NOT the shipping configuration. Do not pool
# Set A and Set B deltas into one table without saying which arms produced
# which row.
#
# MANDATORY: sync + drop_caches before EVERY run. Without it the second arm of
# each pair reads a page cache the first arm warmed; on dedup (large ISO read)
# that alone manufactured a bogus +88%.
#
# sig = significant at n=6 pairs (|t|>2.571) / n=8 (2.365) / n=10 (2.262).
# 7 of 10 significant; geometric mean of those 7 = +24.1%, median +14.4%.
IVH_MIGRATION_WORKLOADS=(
"parsec_dedup|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16|TIME|+86.86|sig"
"parsec_vips|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p vips -c gcc -i native -n 16|TIME|+57.39|sig"
"parsec_ferret|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p ferret -c gcc -i native -n 16|TIME|+15.21|sig"
"parsec_bodytrack|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p bodytrack -c gcc -i native -n 16|TIME|+14.39|sig"
"parsec_swaptions|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p swaptions -c gcc -i native -n 16|TIME|+10.43|sig"
"parsec_freqmine|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p freqmine -c gcc -i native -n 16|TIME|+4.96|sig"
"tinyconfig|/root|lo|rm -rf /tmp/asb; mkdir -p /tmp/asb; make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1; make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux|TIME|+0.99|sig"
# tinyconfig MUST wipe the build dir and generate the config first, then build
# the vmlinux target. Without the wipe, make finds nothing to do and returns in
# ~0.5s having built nothing -- which is exactly what the first point-15 pass
# recorded (0.47/0.50/0.68s, rows discarded). Source: tinyconfig_ab.sh run().
# The upstream harness times ONLY the third command; here the wipe+config are
# inside the timed window and dilute the rate by roughly the config's ~2s.
"parsec_blackscholes|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p blackscholes -c gcc -i native -n 16|TIME|+8.01|sig-2026-09-27"
"psearchy|/root/mosbench/psearchy|hi|./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x|THROUGHPUT|+0.82|NOT-sig"
"parsec_canneal|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p canneal -c gcc -i native -n 16|TIME|+0.14|NOT-sig"
)
# Provisional, sampler-on AND uncorrected harness -- evaluation.md says DO NOT
# QUOTE: streamcluster +35.84, facesim +23.86, fluidanimate +4.60.
# raytrace excluded (needs a display; headless CVM). x264 not built.
# NOTE: /root/kernels/linux-6.14-stock must stay pristine -- the tinyconfig
# build is out-of-tree (O=/tmp/asb) and does not dirty it.

# ---------------------------------------------------------------------------
# IVH_CORE -- best-of-family, ONE workload per tool. Set A and Set B combined
# and ranked by recorded delta; within each family only the strongest survives.
#
# set=A -> full IVH stack arms.  set=B -> migration-alone arms (spin_mode 1,
# ivh_pv_preempt_src=0, ivh_universal_eligible the only variable). The two are
# NOT the same configuration; a combined table must say which produced which.
#
# Format: name | set | family | recorded | kernel lock exercised
# NHextend-full = the AFL (adaptive futex lock) binary: IVH + adaptive
# spinning. NHextend3 is IVH ALONE and is a DIFFERENT ARM, not a substitute.
# CS length is set by env var, not a flag:
#     NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=<n> ./NHextend-full -n 16
# Recorded AFL benefit vs spin-only, ivh_afl_shared_futex_2026-09-14.md s3
# (3 rounds x 8s, same binary all arms):
#     loop_spin  ~CS     spin-only  private-AFL  benefit
#         5,000   11us     216,269      361,678    +64%
#        25,000   55us     104,136      131,789    +27%
#        50,000  110us      30,212       56,375    +88%
#       150,000  330us      10,305       20,693   +100%
#       300,000  660us       5,248       10,376    +99%
#       600,000  1.3ms       1,791        4,881   +175%
# "Adaptive spinning's benefit is preserved at every CS length studied, from
# 11us to 1.3ms." loop_spin=600000 (1.3ms) is the historical migration-win
# default used by nhextend3_stock_vs_migration.sh.
IVH_NHEXTEND="NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16"

IVH_CORE=(
"fsmark_tmpfs|A|fs_mark|+167.0|dcache/inode, VFS metadata"
"perf_sched_pipe|A|perf-bench|+146.9|pipe mutex + rq locks"
"ebizzy_mmap|A|ebizzy|+104.3|mmap_lock (rwsem wait_lock)"
"stressng_dentry|A|stress-ng|+99.5|dcache spinlocks"
"parsec_dedup|B|PARSEC-app|+86.86|pipeline, 4 lanes x 4 bounded queues"
"hackbench_pipe_thr|A|hackbench|+76.3|pipe + scheduler"
"parsec_vips|B|PARSEC-app|+57.39|image processing, thread pool"
"sysbench_mutex|A|sysbench|+24.4|userspace mutex -> futex"
"dbench_16|A|dbench|+19.1|VFS metadata + fsync"
"parsec_ferret|B|PARSEC-app|+15.21|content-based search pipeline"
"parsec_bodytrack|B|PARSEC-app|+14.39|computer vision, barriers + pool"
"wis_mmap2|A|will-it-scale|+11.2|mmap_lock"
"parsec_swaptions|B|PARSEC-app|+10.43|financial Monte Carlo"
"nhextend_full|AFL|NHextend|+64.0|userspace adaptive futex lock, 11us CS"
"schbench|A|schbench|+7.4|scheduler wakeup latency"
)
# Each PARSEC package is its own APPLICATION, not a variant of one tool, so
# they are listed individually; only tool-invocation variants are collapsed.
#
# Borderline / controls, deliberately excluded from IVH_CORE:
#   parsec_freqmine  +4.96  sig but under the +5% bar; collapsed to +2.03 (ns)
#                           in the sampler-on measurement
#   tinyconfig       +0.99  sig but tiny -- negative control
#   psearchy         +0.82  NOT sig -- negative control
#   parsec_canneal +0.14 -- NOT sig
#   parsec_blackscholes: recorded +1.07 NOT sig, but RE-TESTED 2026-09-27 at
#     +8.01% (6/6 pairs, t=8.88) and is now IN the suite. Change not
#     attributed (kernel moved to G-LOCK-48, migration only started firing
#     that day); quote as "+8.01% on G-LOCK-48, 2026-09-27".
#
# Dropped as within-family duplicates (still in the full lists above):
#   perf-bench    : epoll_wait +53.9, syscall_basic +8.7
#   stress-ng     : flock +44.2, mmap +23.4, sock +19.7, pipe +15.8, futex +10.5
#   hackbench     : sock_thr +75.4, pipe_proc +61.8
#   will-it-scale : mmap1 +10.9

# ---------------------------------------------------------------------------
# SCALED VARIANTS -- for parameter-sensitivity work needing a PV arm >= 5 s.
#
# Two confirmed wins run under a second at their campaign invocation, which is
# too short for a stable throughput delta (startup, drop_caches refill and
# scheduling jitter are a large fraction of the run):
#     fsmark_tmpfs   -n 2000           PV 0.48 s
#     sysbench_mutex --mutex-locks=40000  PV 0.59 s
#
# Scaled linearly and re-confirmed against stock PV, 5 pairs, order alternated,
# warmup discarded (2026-09-28):
#
#   fsmark_tmpfs -n 30000        PV 5.64 s   99,302 -> 306,644 files/s
#                                 +208.8%  5/5 pairs  t=24.96  SIGNIFICANT
#                                 (1875 MB in /dev/shm; recorded figure +167.0%)
#   sysbench_mutex 600000 locks  PV 5.91 s   5.91 s -> 4.74 s
#                                 +19.8% time saved  5/5  t=21.23  SIGNIFICANT
#                                 migrations 2940-3469 vs 0 in PV
#                                 (recorded figure +24.4%)
#
# The ratio is NOT exactly scale-invariant (fs_mark 167 -> 209, sysbench
# 24.4 -> 19.8), so quote these against their own config, not as a
# reproduction of the campaign number. They are, however, MUCH better
# measured: t=25 and t=21 here versus the sub-second runs, which spanned
# +173.6% to +224.9% on fs_mark within one day.
# The scaled fsmark entry CLEANS /dev/shm/fsmark first. At -n 30000 each run
# writes 1,875 MB; benchmarks.tsv leaves cleanup to its harness, so a bare
# command fills the 7.4 GB tmpfs after 4 runs and every later run exits in
# ~0.15 s having written nothing (2026-09-28: reps 1-3 at 32k-42k contention
# events, reps 4-9 at 7-15). It also starves whatever runs next -- 7.4 GB of a
# 14 GB guest pinned in tmpfs.
IVH_SCALED=(
"fsmark_tmpfs|/root|hi|rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark; fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'|+208.8"
"sysbench_mutex|/root|lo|sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=600000 run|grep -oP 'total time:\s*\K[0-9.]+'|+19.8"
)

# ---------------------------------------------------------------------------
# WATCH LIST -- below +5% today; re-check after points 7/8/11 tune parameters.
# All three were retired on the same sampler-corrected pass (evaluation.md
# 10.2) that also retired parsec_blackscholes -- which re-tested at +8.01%
# (6/6, t=8.88) on 2026-09-27 and is now IN the suite. Same doubt applies.
#   parsec_canneal  +2.01% NOT sig  4/4 pairs t=2.60 vs crit 2.776 at n=4 --
#                   UNDERPOWERED, stopped at 4 of 6 pairs. Strongest candidate:
#                   finishing its last 2 pairs is the cheapest open measurement.
#   psearchy        +0.82% NOT sig  6/8. sampler gap was +4.8pp (5.2 -> 0.8)
#   tinyconfig      +0.99% sig/tiny 9/10. sampler gap was +5.6pp (7.2 -> 1.0)
IVH_WATCHLIST=(
"parsec_canneal|/root/parsec-benchmark|lo|./bin/parsecmgmt -a run -p canneal -c gcc -i native -n 16|TIME|+2.01 ns"
"psearchy|/root/mosbench/psearchy|hi|./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x|grep -o 'throughput: [0-9.]*'|+0.82 ns"
"tinyconfig|/root|lo|rm -rf /tmp/asb; mkdir -p /tmp/asb; make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1; make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux|TIME|+0.99"
)
