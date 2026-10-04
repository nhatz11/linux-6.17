#!/usr/bin/env python3
"""Cost/plan for the Gate 2 time-left sweep. Prints; runs nothing."""
# per-run seconds: MEASURED where a CSV or today's run exists, else estimated
W = [  # name, set, seconds, source, cs_signal
 ("fsmark_tmpfs",       "A",   8, "measured today",        "NONE (0 holds >=476us)"),
 ("perf_sched_pipe",    "A",   2, "est from ops/sec",      "unknown"),
 ("ebizzy_mmap",        "A",  15, "-S 15 fixed",           "unknown"),
 ("stressng_dentry",    "A",  15, "-t 15s fixed",          "unknown"),
 ("parsec_dedup",       "B",  14, "CSV n=36",              "unknown"),
 ("hackbench_pipe_thr", "A",   3, "measured today",        "GOOD (250 -> 3)"),
 ("parsec_vips",        "B",  14, "CSV n=36",              "unknown"),
 ("sysbench_mutex",     "A",   2, "est",                   "unknown"),
 ("dbench_16",          "A",  30, "-t 30 fixed",           "unknown"),
 ("parsec_ferret",      "B",  68, "CSV n=36",              "unknown"),
 ("parsec_bodytrack",   "B",  63, "CSV n=36",              "unknown"),
 ("wis_mmap2",          "A",  10, "-s 10 fixed",           "unknown"),
 ("parsec_swaptions",   "B",  37, "CSV n=36",              "unknown"),
 ("nhextend_full",     "AFL",  8, "DURATION=8 fixed",      "unknown"),
 ("schbench",           "A",  15, "-r 15 fixed",           "unknown"),
 ("parsec_blackscholes","B?",  24, "CSV n=48 + today",     "unknown"),
 ("parsec_canneal",     "B?",  82, "CSV n=44 + today",     "unknown"),
 ("psearchy",           "B?",  31, "CSV n=16",             "unknown"),
 ("tinyconfig",         "B?",  25, "CSV n=20",             "unknown"),
]
OVERHEAD = 6      # drop_caches+sync, arm switch, sleep 1, 4x counter reads
ARMS = ["PV", "250us", "500us", "1ms", "2ms", "4ms", "8ms", "16ms"]

print("=" * 78)
print("GATE 2 TIME-LEFT SWEEP -- PLAN ONLY, NOTHING RUN")
print("=" * 78)
print(f"\nARMS ({len(ARMS)}): {', '.join(ARMS)}")
print("  PV    = stock PV, spin_mode 1, adaptive_mode==0 asserted, migration off")
print("  each threshold = full IVH stack, ivh_time_left_threshold_ns set + readback-asserted")
print("  Range from burst_probe.py: bursts median 1.04ms, p90 2.05ms; shipped")
print("  4ms ~= p96, so the interesting region is BELOW it.")
print(f"\nWORKLOADS ({len(W)}): 15 IVH_CORE + 4 re-test candidates\n")
print(f"  {'workload':22}{'set':5}{'sec':>5}  {'x8 arms':>8}  {'source':22} CS signal")
tot = 0
for n, s, sec, src, cs in W:
    per = sec + OVERHEAD
    tot += per
    print(f"  {n:22}{s:5}{sec:5}  {per*len(ARMS)/60:7.1f}m  {src:22} {cs}")
print(f"\n  one pass of all {len(W)} (1 arm)      = {tot/60:6.1f} min")
print(f"  one full rep ({len(ARMS)} arms)          = {tot*len(ARMS)/3600:6.2f} h   "
      f"({len(W)*len(ARMS)} runs)")
for r in (1, 2, 3):
    print(f"  {r} rep(s)                       = {tot*len(ARMS)*r/3600:6.2f} h   "
          f"({len(W)*len(ARMS)*r} runs)")
print("\n  cost concentration:")
exp = sorted(W, key=lambda x: -x[2])[:4]
es = sum(x[2] + OVERHEAD for x in exp)
print(f"    4 slowest ({', '.join(x[0] for x in exp)})")
print(f"    = {es/tot*100:.0f}% of total time")
print(f"    dropping them: {(tot-es)*len(ARMS)*2/3600:.2f} h for 2 reps instead of "
      f"{tot*len(ARMS)*2/3600:.2f} h")
