# 36-vCPU workload sizing override. Source AFTER ivh_benchmarks.sh.
#
# The registry is sized for 16 vCPUs and under-subscribes a 36-vCPU box, which
# would not generate the contention the recorded skip result depends on. A null
# under 16-vCPU sizing on 36 vCPUs is uninformative -- see REPLICATE_36VCPU.md.
#
# hackbench: `-T -g4 -f8 -l100000` is the EXACT config the recorded 36-vCPU
#   experiments used (per the stepcombowl notes). -g4 -f8 -T = 4 groups x 8 fds
#   x 2 = 64 tasks on 36 vCPUs, i.e. oversubscribed, which is the point.
#   The 16-vCPU `-g1 -f8` is only 16 tasks -- under-subscribed here.
# dbench / dentry: the 36-vCPU client/worker counts were never recorded, so
#   they are scaled 16 -> 36 to preserve the 1-per-vCPU ratio the 16-vCPU runs
#   had. That is an assumption, stated here rather than hidden.
IVH_SIZED36=(
"hackbench_pipe_thr|/root|lo|hackbench -T -g4 -f8 -l100000|grep -oP '^Time:\s*\K[0-9.]+'|NA"
"dbench_16|/root|hi|dbench -F -t 15 36 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|NA"
"stressng_dentry|/root|hi|stress-ng --dentry 36 -t 15s --metrics-brief|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|NA"
)
# Prepend so lookup() finds these first.
IVH_SCALED=("${IVH_SIZED36[@]}" "${IVH_SCALED[@]}")
