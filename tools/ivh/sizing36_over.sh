# 36 vCPU, OVERSUBSCRIBED (64 tasks = 1.78x vCPUs).
# hackbench -T -g4 -f8 = 4 groups x 8 fds x 2 = 64 tasks -- the EXACT config the
# recorded 36-vCPU skip experiments used. dbench/dentry scaled to 64 to match
# the same oversubscription ratio.
# HYPOTHESIS: eviction needs a LIVE waiter behind the preempted one to promote.
# At hop_cap=1, 100% of evictions find none (ivh_eviction_nogain_rate). Deeper
# queues => more likely a live successor exists => eviction can pay off.
# ivh_evict_lookahead_refused / ivh_evict_marked is the direct test.
IVH_SIZED36=(
"hackbench_pipe_thr|/root|lo|hackbench -T -g4 -f8 -l100000|grep -oP '^Time:\s*\K[0-9.]+'|NA"
"dbench_16|/root|hi|dbench -F -t 15 64 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|NA"
"stressng_dentry|/root|hi|stress-ng --dentry 64 -t 15s --metrics-brief|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|NA"
)
IVH_SCALED=("${IVH_SIZED36[@]}" "${IVH_SCALED[@]}")
