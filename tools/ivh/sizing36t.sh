# 36-TASK sizing for a 36-vCPU box. Source AFTER ivh_benchmarks.sh (and after
# parsec_direct.sh, since this supersedes its thread counts).
#
# WHY THIS EXISTS: the registry is 16-vCPU sized. Running it on 36 vCPUs leaves
# ~20 vCPUs near-idle, and the BPF migration selector acts on GUEST-COMPUTED
# CAPACITY -- idle vCPUs are abundant migration targets regardless of what the
# host is doing. That is why making host contention uniform did NOT suppress
# migration here (hackbench 33,128/run uniform vs 33,184-44,780 split) while the
# same change at 16 vCPUs collapsed it 21,058 -> 323: at 16v the 16 tasks filled
# the box, so there was no spare capacity to migrate toward.
#
# Every thread/client count below is set to 36 to match the vCPU count, so no
# vCPU carries spare capacity for migration to exploit.
#
# hackbench: -T -g2 -f9 = 2 groups x 9 fds x 2 = 36 tasks exactly.
# schbench:  -m 4 -t 9  = 4 message threads x 9 workers = 36.
# CAVEAT, stated because it limits the conclusion: these workloads BLOCK (pipes,
# fsync, futexes), so 36 runnable-at-peak tasks do not pin 36 vCPUs at 100%.
# Some spare capacity will remain and migration will not fall to zero. If it is
# still in the tens of thousands after this, the next step is oversubscription
# (hackbench -g4 -f8 = 64), not more of the same.
IVH_SIZED36T=(
"ebizzy_mmap|/root|hi|/home/nick/Desktop/ebizzy -S 15 -t 36 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'|NA"
"hackbench_pipe_thr|/root|lo|hackbench -T -g2 -f9 -l150000|grep -oP '^Time:\s*\K[0-9.]+'|NA"
"dbench_16|/root|hi|dbench -F -t 15 36 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|NA"
"wis_mmap2|/root/will-it-scale|hi|./mmap2_threads -t 36 -s 15|grep -oP 'average:\K[0-9]+'|NA"
"fsmark_tmpfs|/root|hi|rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark; fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 36 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'|NA"
"schbench|/root|hi|bash -c '/root/bench/schbench/schbench -m 4 -t 9 -r 15 2>&1'|grep -oP 'average rps:\s*\K[0-9.]+'|NA"
"parsec_vips|/root/parsec-benchmark/pkgs/apps/vips/run|lo|IM_CONCURRENCY=36 /root/parsec-benchmark/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|TIME|NA"
"parsec_dedup|/root/parsec-benchmark/pkgs/kernels/dedup/run|lo|rm -f output.dat.ddp; /root/parsec-benchmark/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 36 -i FC-6-x86_64-disc1.iso -o output.dat.ddp|TIME|NA"
"parsec_bodytrack|/root/parsec-benchmark/pkgs/apps/bodytrack/run|lo|/root/parsec-benchmark/pkgs/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 36|TIME|NA"
)
IVH_SCALED=("${IVH_SIZED36T[@]}" "${IVH_SCALED[@]}")
