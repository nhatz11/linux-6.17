# System-wide kernel lock-acquisition rate, per workload (2026-09-29)

Measured 2026-09-29 on the 16-vCPU TDX guest, kernel `6.17.0-G-LOCK-48-skipcheck+`,
arm **stock PV** (`spin_mode 1`), host corunner active (guest loadavg ~7.7 from
`vcap_probe`, which is by design).

Harness `ivh_tools/acq_suite.sh`; data `acq_systemwide_data.csv` (91 runs),
`acq_systemwide_long_variants.csv`, `acq_systemwide_memtier_{tuned,distro}.csv`;
per-function breakdown of every run in `acq_systemwide_perfunction.txt`.

## 1. Why this supersedes the point-15 rates

Point 15 ranked workloads with two counters, both of which see less than the
whole system:

| counter | blind spot |
|---|---|
| `lock:contention_begin` | fires only in `queued_spin_lock_slowpath` -- only CONTENDED acquisitions. 1.2% of schbench's total. |
| `ivh_prelock_calls` | counts only acquisitions ELIGIBLE for IVH, after five bails in `ivh_pre_lock()` -- notably `!rcu_preempt_depth()`, which excludes dcache, lockref, net and slab callers. Reads **zero** in the PV arm by construction. |

The ftrace function profiler has **no pid filter and no eligibility filter**: it
counts an entry to any of the 17 `_raw_spin_lock*` / `_raw_read_lock*` /
`_raw_write_lock*` symbols on any CPU by any task. Workload threads, threads the
workload forks, kernel threads woken on its behalf, and separate server processes
all land in the same per-CPU stat files.

The gap between the two is a real per-workload quantity. `parsec_dedup` reads
272,228/s on `ivh_prelock_calls` and **1,203,748/s** here -- a 4.4x ratio that is
the `ivh_pre_lock()` bail rate made visible.

**Limits.** The profiler taxes every traced call and `_raw_spin_lock` fires
millions of times a second, so a traced run's throughput is NOT comparable to an
untraced one. Counts are exact; this measures lock rate, not performance. The
slowpath column is counted separately and subtracted from the total, because
`queued_spin_lock_slowpath` is a callee of `_raw_spin_lock`, not a peer.

## 2. Results

Idle floor **55,729 acq/s, 108 slowpath/s** (n=7 x 15s). The floor is bimodal:
5 of 7 reps fall in 53,520-57,061, two consecutive reps ran 3x that with 50x the
slowpath and then it settled. That is a background burst, not the floor; the
median is the right statistic.

```
workload                n  wall_s        acq/s    net acq/s  xfloor     slow/s  slow%  spread   CV%
memtier_tuned           3    11.2   11,477,623   11,421,894  206.0x    192,342  1.68%   1.09x   4.0
perf_epoll_wait         3    16.1   10,663,409   10,607,680  191.3x    104,944  0.98%   1.04x   1.7
stressng_dentry         3    17.3    9,230,165    9,174,436  165.6x    849,754  9.21%   1.01x   0.6
memtier_distro          3    11.2    8,400,733    8,345,004  150.7x     56,655  0.67%   1.01x   0.4
hackbench_pipe_thr      3     8.4    5,537,412    5,481,683   99.4x    936,996 16.92%   1.04x   1.8
ebizzy_mmap             3    15.0    5,472,776    5,417,047   98.2x     66,091  1.21%   1.05x   2.2
fsmark_tmpfs            7     0.1    4,231,795    4,176,066   75.9x     63,046  1.49%   1.27x   8.3  WINDOW<1s
fsmark_long             3    32.8    3,708,331    3,652,602   66.5x     27,979  0.75%   1.01x   0.3
psearchy                3    21.8    1,370,991    1,315,262   24.6x     15,140  1.10%   1.03x   1.2
nhextend_full           3     8.0    1,325,043    1,269,314   23.8x     23,310  1.76%   1.02x   0.7
parsec_dedup            7     5.2    1,203,748    1,148,019   21.6x     11,861  0.99%   1.77x  25.5
tinyconfig              3    21.3    1,153,154    1,097,425   20.7x      5,527  0.48%   1.02x   0.6
dbench_16               3    18.1      958,826      903,097   17.2x      9,929  1.04%   1.01x   0.5
wis_mmap2               3    21.1      772,725      716,996   13.9x     13,057  1.69%   1.07x   3.0
parsec_vips             3     4.6      441,852      386,123    7.9x      1,507  0.34%   1.10x   4.0
sysbench_mutex          7     0.2      341,767      286,038    6.1x     69,118 20.22%   1.91x  22.6  WINDOW<1s
sysbench_mutex_long     7    10.0      266,840      211,111    4.8x     77,362 28.99%   1.66x  22.1
parsec_bodytrack        3    42.8      135,898       80,169    2.4x      3,277  2.41%   1.03x   1.5  near floor
schbench                3    15.0      130,087       74,358    2.3x        380  0.29%   1.08x   3.5  near floor
parsec_blackscholes     7    16.9      117,635       61,906    2.1x      1,879  1.60%   1.68x  21.3  near floor
parsec_canneal          7    58.1       71,952       16,223    1.3x        250  0.35%   1.48x  17.8  AT FLOOR
parsec_ferret           7    51.5       68,244       12,515    1.2x        116  0.17%   1.43x  14.0  AT FLOOR
parsec_swaptions        3    21.8       59,176        3,447    1.1x        115  0.19%   1.13x   5.6  AT FLOOR
```

3 reps minimum, extended automatically to 7 while max/min of acq/s exceeded 1.15.

## 3. The coverage gap is real, and memcached is the demonstration

`memtier_benchmark` is the process you invoke. The lock traffic is not in it:

```
memtier (tuned)   _raw_spin_lock_bh       2,693,571/s    softirq: loopback TCP
                  _raw_spin_lock_irqsave  2,137,846/s
                  _raw_spin_lock          1,201,890/s
                  _raw_spin_lock_irq      1,004,699/s
                  _raw_write_lock_irq       804,638/s
```

`_raw_spin_lock_bh` is softirq-context network work living in the **memcached
server process** and ksoftirqd, a different process from the client entirely.
Any instrument scoped to the invoked command misses the majority of it.

## 4. Three PARSEC packages are below the instrument's resolution

Attributable rate (median minus idle floor):

| package | net acq/s | slowpath/s | floor slowpath/s |
|---|---|---|---|
| swaptions | 3,447 | 115 | 108 |
| ferret | 12,515 | 116 | 108 |
| canneal | 16,223 | 250 | 108 |

Their slowpath rates are indistinguishable from the floor's own. These three take
essentially no kernel spinlocks; an IVH result on them is measuring something
other than lock behaviour. This puts a number on point 15's "PARSEC is the wrong
instrument" for each package rather than as a blanket claim -- and `vips` (7.9x
floor) and `dedup` (21.6x) are genuine exceptions that should not be lumped in.

## 5. Two registry invocations are too short to measure -- already solved

`campaign/benchmarks.tsv` as written:

```
sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=40000 run
    total time: 0.3106s     total number of events: 16      <- one per thread
fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1
    Count 32000  Files/sec 294,049      real 0m0.136s       <- tmpfs absorbs it
```

Both needed all 7 reps here and neither converged (spread 1.91x and 1.27x).

**CORRECTION (2026-09-29).** An earlier revision of this file presented these as
an open problem and proposed new "long variants". That was wrong: the scaled
configurations already exist and are recorded in `eval_final.md`, validated
2026-09-28 at 5/5 pairs:

| workload | campaign cfg | PV | **suite cfg** | PV | IVH vs PV | t |
|---|---|---|---|---|---|---|
| `fsmark_tmpfs` | `-n 2000` | 0.48 s | **`-n 30000`** | 5.64 s | +208.8% thr | 24.96 |
| `sysbench_mutex` | `--mutex-locks=40000` | 0.59 s | **`--mutex-locks=600000`** | 5.91 s | +19.8% time | 21.23 |

These scale the WORK (a single run does more), which is the right shape. The
`fsmark_long` measured here -- 100 iterations of the short invocation -- is the
WRONG shape: it charges 100 process startups into the measurement, the very
defect that makes the short form untrustworthy. It is retained below only as
evidence of the size of the error, and should not be used.

```
fsmark_tmpfs (-n 2000, 0.14s)   4,231,795 acq/s   1.49% slowpath   CV 8.3
fsmark_long  (100x loop, 32.8s) 3,708,331 acq/s   0.75% slowpath   CV 0.3
sysbench_mutex (0.31s)            341,767 acq/s  20.22% slowpath   CV 22.6
sysbench_mutex_long (10.0s)       266,840 acq/s  28.99% slowpath   CV 22.1
```

The short forms overstate rate by +14% and +28%; fsmark's contended share is 2x
too high and sysbench's is understated -- its real share is the HIGHEST in the
suite, above hackbench's 16.92%. fsmark's CV collapses 8.3 -> 0.3 once the window
is long enough, so that variance was pure startup noise; sysbench_mutex stays at
CV 22% over 10 s, so that variance is the workload.

**Config authority.** `campaign/benchmarks.tsv` holds the CAMPAIGN config.
`eval_final.md`'s "suite cfg" SUPERSEDES it for the scaled workloads. Check both
before quoting an invocation. Lock rates for the scaled configs have not been
re-measured here; the rows above are the campaign configs.

## 6. Rate does not predict the IVH win -- now on a system-wide counter

| workload | acq/s rank | slowpath% | IVH outcome |
|---|---|---|---|
| memtier (tuned) | 1 of 21 | 1.68% | **+26.21%** migration, largest in project |
| psearchy | 7 | 1.10% | negative control, +0.82% NOT sig |
| tinyconfig | 11 | 0.48% | negative control, +0.99% sig but tiny |
| dbench_16 | 12 | 1.04% | consistent winner |

Both designated negative controls have healthy absolute rates -- above dbench,
which wins -- and psearchy's contended share (1.10%) exceeds that of the distro
memcached run (0.67%). Neither total rate nor contended share separates winners
from controls.

This reproduces the blocking-structure finding on a counter with no eligibility
filter. Previously it rested on `ivh_prelock_calls`, which a reviewer could
dismiss as measuring IVH's own bail conditions; that objection does not apply
here.

## 7. CORRECTION: the memtier server was never the tuned one

The memtier work earlier in this project started its server with

```
pkill -x memcached >/dev/null 2>&1; sleep 1
memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=10
```

Two independent failures stack:

1. **`hashpower=10` is rejected outright** -- "Initial hashtable multiplier of 10
   is too low". The tuned server never starts. memcached sizes the item-lock
   table from the worker-thread count, so `-t 16` forces item-lock power 14 and
   hashpower must be **>= 15**.
2. **`pkill` never clears port 11211.** The distro service is `Restart=always`
   with `RestartUSec=100ms`, so it is back before the 1-second sleep ends and
   holds the port.

Every memtier measurement in this project was therefore served by
`/usr/bin/memcached -m 64 -p 11211 -u memcache` -- **64MB, 4 worker threads,
default hashpower** -- not 1GB/16 threads. Confirmed by direct control: the
original suite row (8,619,902/s, 0.67%) matches an explicit distro-server run
(8,400,733/s, 0.67%) within 2.5%, while the correctly-started tuned server reads
11,477,623/s, 1.68%.

**What survives.** The client-side tuning is real -- `-t 16 -c 50` are
`memtier_benchmark` flags and took effect; that is what moved migrations from
~18,000 to ~70,000 per run. The **+26.21%** figure is a valid measurement of a
real memcached workload.

**What does not.** Every statement about the server configuration. The workload
that produced +26.21% was a 4-thread 64MB memcached, and it should be described
that way, or the experiment re-run against the tuned server -- which is a
materially different workload: **+36.6% lock rate and 2.5x the contended share**.

A correct invocation, with the restart race closed and a valid hashpower:

```
systemctl stop memcached; sleep 1
memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15; sleep 2
# then VERIFY -- do not assume:
pid=$(ss -lntp | grep -oP '11211.*pid=\K[0-9]+' | head -1)
tr '\0' ' ' < /proc/$pid/cmdline
```

**Generalisation.** A benchmark that talks to a server over a port must assert
which process owns that port before it runs. The same class of error invalidated
the `memtier_distro` control in this very session: its PREP used
`systemctl restart memcached` while the tuned server still held 11211, so the
restart failed to bind and the control silently measured the tuned server again
(both arms returned 1.68%). It was caught only because the numbers were
identical to three significant figures.
