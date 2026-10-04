# PARSEC run DIRECTLY, bypassing ./bin/parsecmgmt. Source AFTER ivh_benchmarks.sh.
#
# WHY: parsecmgmt is a shell harness that itself acquires ~600,000 spinlocks/s
# (`parsecmgmt -a status`, no application work at all, measured 599,374/s). Via
# the harness all six packages landed within 1.6% of each other (807k-826k/s)
# regardless of the application; blackscholes read 825,500/s through it and
# 103,644/s direct -- ~87% harness. eval_final 15.1 therefore runs each package's
# native.runconf run_exec/run_args directly from its run/ directory, and so does
# this file.
#
# Invocations taken verbatim from each package's parsec/native.runconf:
#   vips      run_exec=bin/vips,     run_args="im_benchmark orion_18000x18000.v output.v"
#             plus `export IM_CONCURRENCY=${NTHREADS}` -- vips takes its thread
#             count from the environment, NOT argv, so omitting it would run
#             single-threaded and measure nothing about lock contention.
#   dedup     run_exec=bin/dedup,    run_args="-c -p -v -t ${NTHREADS} -i FC-6-x86_64-disc1.iso -o output.dat.ddp"
#   bodytrack run_exec=bin/bodytrack, args "sequenceB_261 4 261 4000 5 0 ${NTHREADS}"
#
# NTHREADS=16, matching the registry's documented `-n 16` for these three.
#
# METRIC: wall clock (extractor TIME, direction `lo`), because the perf figure
# parsecmgmt printed is no longer available when the binary is run directly.
PD=/root/parsec-benchmark/pkgs
IVH_PARSEC_DIRECT=(
"parsec_vips|$PD/apps/vips/run|lo|IM_CONCURRENCY=16 $PD/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|TIME|NA"
"parsec_dedup|$PD/kernels/dedup/run|lo|rm -f output.dat.ddp; $PD/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp|TIME|NA"
"parsec_bodytrack|$PD/apps/bodytrack/run|lo|$PD/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 16|TIME|NA"
)
# Prepend so lookup() finds these before the parsecmgmt entries.
IVH_SCALED=("${IVH_PARSEC_DIRECT[@]}" "${IVH_SCALED[@]}")
