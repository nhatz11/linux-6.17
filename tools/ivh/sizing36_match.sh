# 36 vCPU, MATCHED (36 tasks = 1.0x vCPUs) -- the contrast arm for the
# oversubscription hypothesis. hackbench -T -g2 -f9 = 2 x 9 x 2 = 36 exactly.
IVH_SIZED36=(
"hackbench_pipe_thr|/root|lo|hackbench -T -g2 -f9 -l100000|grep -oP '^Time:\s*\K[0-9.]+'|NA"
"dbench_16|/root|hi|dbench -F -t 15 36 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|NA"
"stressng_dentry|/root|hi|stress-ng --dentry 36 -t 15s --metrics-brief|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'|NA"
)
IVH_SCALED=("${IVH_SIZED36[@]}" "${IVH_SCALED[@]}")
