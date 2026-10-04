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
IVH_WORKLOADS_A=(
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
"wis_mmap2|/root/bench/will-it-scale|hi|./mmap2_threads -t 16 -s 15|grep -oP "average:\\K[0-9]+"|+11.2"
"wis_mmap1|/root/bench/will-it-scale|hi|./mmap1_threads -t 16 -s 15|grep -oP "average:\\K[0-9]+"|+10.9"
"stressng_futex|/root|hi|stress-ng --futex 16 -t 15s --metrics-brief|grep -oP 'futex\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|+10.5"
"perf_syscall_basic|/root|hi|perf bench syscall basic -l 30000000|grep -oP '\K[0-9]+(?= ops/sec)'|+8.7"
"schbench|/root|hi|bash -c '/root/bench/schbench/schbench -m 2 -t 8 -r 15 2>&1'|grep -oP 'average rps:\s*\K[0-9.]+'|+7.4"
)
