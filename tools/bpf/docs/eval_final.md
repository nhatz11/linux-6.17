# IVH final evaluation

Results only, and how each was obtained. This file is deliberately NOT a
record of everything tried -- that is `evaluation.md`, which is a culmination
including retractions, dead ends and superseded numbers. Anything written
here is meant to be quotable as-is, with its own caveats attached.

Rules for this file:

- Every number carries the config it was taken under and the control it was
  measured against.
- A claim that has been withdrawn stays visible, struck, with the reason.
  Silent deletion is how a retracted number gets re-quoted six weeks later.
- If a result is not yet measured, the point says so rather than being left
  ambiguous.

Status legend: **DONE** / **PARTIAL** / **NOT STARTED**

| # | point | status |
|---|---|---|
| 1 | Kill PLE | NOT STARTED |
| 2 | Kill PV | NOT STARTED |
| 3 | How many cloud instances use PV spinlock | NOT STARTED |
| 4 | How many threads are migratable | NOT STARTED |
| 5 | **TSC accuracy** | **DONE (4 of 4 sub-claims)** |
| 6 | Migration impact | NOT STARTED |
| 7 | Time-left sensitivity | PARTIAL (curve taken, see evaluation.md §11) |
| 8 | Budget sensitivity | NOT STARTED |
| 9 | Cost vs gain | NOT STARTED |
| 10 | Adaptive spinning is good | PARTIAL (see 5.3/5.4 for detection; throughput unmeasured) |
| 11 | Iterations before deciding preempted | PARTIAL (publish cadence measured, see 5.4) |
| 12 | Full test on 1 VM | NOT STARTED |
| 13 | Scalability | NOT STARTED |
| 14 | Our weaknesses | NOT STARTED |
| 15 | **Workload reliability** | **DONE (with 3 stated limits)** |

---

# Appendix A. The benchmark suite -- 16 workloads, exact configurations

**One workload per family.** Set A (full IVH stack) and Set B (migration
alone) are merged here into a single suite: these are the workloads that show
a benefit, regardless of which arm configuration demonstrated it. Where a
family had several members only the strongest is kept. PARSEC packages are
separate APPLICATIONS, not variants of one tool, so they count individually;
only tool-invocation variants were collapsed.

| workload | family | recorded | command |
|---|---|---|---|
| `stressng_dentry` | stress-ng | +99.5% | `stress-ng --dentry 16 -t 15s --metrics-brief` |
| `hackbench_pipe_thr` | hackbench | +76.3% | `hackbench -T -g1 -f8 -l150000` |
| `sysbench_mutex` | sysbench | **+19.8%** ‡ | `sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=600000 run` |
| `ebizzy_mmap` | ebizzy | +104.3% | `/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304` |
| `nhextend_full` | NHextend | +64.0% | `NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16` |
| `dbench_16` | dbench | +19.1% | `dbench -F -t 15 16 -D /root/dbench_test` |
| `fsmark_tmpfs` | fs_mark | **+208.8%** ‡ | `fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1` |
| `wis_mmap2` | will-it-scale | +11.2% | `./mmap2_threads -t 16 -s 15`<br>*(cwd `/root/bench/will-it-scale`)* |
| `parsec_vips` | PARSEC | +57.39% | `./bin/parsecmgmt -a run -p vips -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |
| `parsec_bodytrack` | PARSEC | +14.39% | `./bin/parsecmgmt -a run -p bodytrack -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |
| `parsec_dedup` | PARSEC | +86.86% | `./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |
| `parsec_blackscholes` | PARSEC | **+8.01%** † | `./bin/parsecmgmt -a run -p blackscholes -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |
| `perf_epoll_wait` | perf-bench | +53.9% | `perf bench epoll wait -t 16 -r 15` |
| `schbench` | schbench | +7.4% | `bash -c '/root/bench/schbench/schbench -m 2 -t 8 -r 15 2>&1'` |
| `parsec_swaptions` | PARSEC | +10.43% | `./bin/parsecmgmt -a run -p swaptions -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |
| `parsec_ferret` | PARSEC | +15.21% | `./bin/parsecmgmt -a run -p ferret -c gcc -i native -n 16`<br>*(cwd `/root/parsec-benchmark`)* |

**Collapsed within family** (kept in `ivh_tools/ivh_benchmarks.sh`, not in the
suite): perf-bench sched_pipe +146.9 / syscall_basic +8.7; stress-ng flock
+44.2 / mmap +23.4 / sock +19.7 / pipe +15.8 / futex +10.5; hackbench sock_thr
+75.4 / pipe_proc +61.8; will-it-scale mmap1 +10.9; PARSEC freqmine +4.96
(below the +5% bar).

† `parsec_blackscholes` is the one row whose figure is **not** a campaign
number. It was recorded at +1.07% (4/6, t=0.76, NOT significant) and re-tested
on 2026-09-27 at **+8.01% (6/6 pairs, t=8.88, crit 2.571, SIGNIFICANT)**,
migration-alone arms, 330-1,348 migrations per IVH run against exactly 0 per PV
run. **The change is not attributed**: the kernel moved to G-LOCK-48, migration
only began firing after the `ivh_preempt_event_source` fix of the same day, and
host contention may differ. Quote it as "+8.01% on G-LOCK-48, 2026-09-27", not
as a correction of the recorded value.

**Two arm configurations produced these numbers, and they are not the same.**
Rows measured under the full IVH stack: stressng_dentry, hackbench_pipe_thr,
sysbench_mutex, ebizzy_mmap, dbench_16, fsmark_tmpfs, wis_mmap2,
perf_sched_pipe, schbench. Rows measured with **migration alone** (both arms
`spin_mode 1`, `ivh_pv_preempt_src=0`, `ivh_universal_eligible` the only
variable): all five PARSEC packages. `nhextend_full` is a third contrast
entirely -- AFL vs spin-only, not IVH vs PV. A combined results table must say
which arm produced which row.

**Sources of truth.** `ivh_tools/campaign/benchmarks.tsv` for the non-PARSEC
rows (the registry the campaign harness ran; 1300 measurements in
`campaign/run_main/results.csv`). `ivh_tools/parsec_ab.sh` driven by
`parsec_redo.sh` for PARSEC, with `PAIRS=6 NTH=16 INPUT=native CFG=gcc` --
**`native` is the largest PARSEC input**. `drop_caches` before every PARSEC
run is mandatory; without it the second arm of each pair reads a page cache
the first warmed, which on dedup alone manufactured a bogus +88%.

**Why this appendix exists.** A workload list assembled on 2026-09-27 from
`screen/mig_screen.sh` and `campaign/fullstack.sh` had **5 of 19 entries
wrong**: `dbench_16` missing `-F` (no fsync -- a materially different workload
from the one that scored +19.1%), `sysbench_mutex` at 32 threads / 20,000
locks instead of 16 / 40,000, `schbench` at `-m 4 -t 4` with an extractor
matching a different output line, `wis_mmap2` at `-s 10` instead of `-s 15`,
`perf_sched_pipe` at `-l 400000` instead of `-l 300000`. `mig_screen.sh` is an
earlier screening harness (2026-09-14), not the campaign.

## A.1 ‡ Two workloads are run at a scaled size

`fsmark_tmpfs` and `sysbench_mutex` complete in under a second at their
campaign invocation -- too short for a stable throughput delta, where startup
and `drop_caches` refill are a large fraction of the run. **The suite table
above lists the SCALED configuration for both**; the campaign invocation is
kept here for provenance.

| workload | campaign cfg | PV | **suite cfg** | PV | IVH vs PV | pairs | t |
|---|---|---|---|---|---|---|---|
| `fsmark_tmpfs` | `-n 2000` | 0.48 s | **`-n 30000`** | 5.64 s | **+208.8%** thr | 5/5 | 24.96 |
| `sysbench_mutex` | `--mutex-locks=40000` | 0.59 s | **`--mutex-locks=600000`** | 5.91 s | **+19.8%** time | 5/5 | 21.23 |

Re-confirmed against stock PV, 5 pairs, order alternated, warmup discarded,
2026-09-28. sysbench's migration counts (2,940-3,469 per IVH run, 0 per PV run)
confirm the mechanism engaged. fs_mark at `-n 30000` needs 1,875 MB in
`/dev/shm`.

**The ratio is not exactly scale-invariant** -- fs_mark's recorded +167.0%
becomes +208.8%, sysbench's +24.4% becomes +19.8% -- which is why the suite
table quotes the SCALED figures, not the campaign ones. They are also far
better measured: t=24.96 and t=21.23, against sub-second runs that spanned
+173.6% to +224.9% on fs_mark within a single day.

Lock rates in section 15 are measured at the scaled configs and barely move:
fs_mark 5,598 -> 4,672 /s, sysbench 13,390 -> 15,754 /s. No stratum changes.

## A.2 Thread counts

**All 16 workloads run 16 threads, workers or clients**: `--dentry 16`,
`-T -g1 -f8` (1 group x 8 fds = 16 tasks), `--threads=16`, `-t 16`, `-n 16`,
`dbench ... 16`, `-m 2 -t 8`.

**`perf_epoll_wait` replaced `perf_sched_pipe` to achieve this.**
`perf bench sched pipe` CANNOT be made 16-threaded: perf 6.14.11 describes it
as *"Benchmark for pipe() between two processes"*, and its entire option set is
`-G/--cgroups`, `-l/--loop`, `-n/--nonblocking`, `-T/--threaded` -- there is no
pair or instance count, so 8 pairs is unreachable from within the tool.
Running 8 concurrent instances would give 16 threads but is not the recorded
benchmark and would not be comparable to its +146.9%.

The swap trades a +146.9% headline for +53.9%, and also removes the one
workload that was bimodal on lock rate (`perf_sched_pipe`: 356, 361, **5,111**
/s). `perf_epoll_wait` measures 365,562 / 395,263 / 414,118 /s -- a 13%
spread -- and 16.2 s in the PV arm. `perf_sched_pipe` remains in the registry
and its +146.9% stands; it is simply not in the suite.

## A.3 Registry

`/root/ivh_tools/ivh_benchmarks.sh` carries the suite, the collapsed
within-family entries, and the scaled variants. Use it rather than
re-deriving invocations.

---

# 5. TSC accuracy

**The claim.** On a confidential VM with no steal-time and
`vcpu_is_preempted()` hardwired false, the guest can still (a) account its own
steal and active time, (b) count its own deschedules, (c) detect that a lock
*holder* has been preempted, and (d) mark a preempted *waiter* for skipping --
all from TSC alone.

All four are measured below. (a) and (b) are validated against the hypervisor;
(c) and (d) are validated against the host at the population level, and by
construction/self-observation per event.

Platform for every number in this section unless stated: Intel TDX guest,
16 vCPUs, `CONFIG_HZ=1000`, `nohz=off`, `tsc_khz=2200000`, kernel
`6.17.0-G-LOCK-4x`. Host contention from a co-running sysbench VM; host-side
ground truth from `/proc/<vcpu-tid>/schedstat` and `perf sched`.

---

## 5.0 Method

How each sub-claim was obtained. Configuration is in 5.5, scripts in 5.6.

| sub-claim | procedure | control / ground truth |
|---|---|---|
| 5.1 steal + active | read the guest's own TSC-derived accounting over a fixed window; compare against the same window measured on the host | host `/proc/<vcpu-tid>/schedstat`; the TD pid is re-resolved every boot |
| 5.2 deschedule count | count TSC jumps exceeding the deschedule threshold; compare the count to the host's own context-switch count for that vCPU | host `schedstat` `nr_switches` |
| 5.3 holder detection | arm the CS stamp, run a workload, bucket every hold by duration at release (`ivh_cs_prev_hold_hist`); a "detection" is a hold past `ivh_cs_noise_cycles` | **dose-response**: idle vs loaded host, two machines and two clocks with no shared instrument. Plus a stock-PV arm (`spin_mode 1`, `ivh_adaptive_mode==0` asserted) in every comparison |
| 5.4 waiter marking | at the eviction decision point, record the predecessor's stamp age and whether the node was already `VCPU_SKIPPED`; precision = marked ∩ genuinely stale / marked | self-observation at the decision point; funnel accounting for refusals |

**Two rules that every number here depends on.**

1. **Stock PV is `spin_mode 1` AND `ivh_adaptive_mode==0`, asserted.** Setting
   `ivh_universal_eligible=0` alone leaves adaptive spinning, head bypass and
   eviction running -- that is a third configuration, not a baseline. Using it
   as the denominator inflated one fs_mark measurement from +173.6% to +239%.
2. **`/root/spin_mode` CLEARS `ivh_cs_owner_enable` and `ivh_cs_owner_clear`.**
   CS stamping must be re-armed AFTER every `spin_mode` call, with a readback
   assert. Arming once at startup silently disarms it on the first arm switch
   and the hold histogram then reads a flat zero for the whole run.

**Reporting convention.** Improvements in this file are stated as **% wall
time saved** for time-reported benchmarks and **% throughput gained** for
rate-reported ones; the two are not interchangeable (hackbench PV 59.57s ->
IVH 15.55s is +73.9% time saved and +283% throughput). Each table says which.

---

## 5.1 Steal and active time vs the hypervisor

**Result: on contended vCPUs, steal reads 0.907 and active 0.912 of host
truth -- both ~9% under, in the safe direction.**

Shipped configuration:

    ivh_tks_sampler_ns  = 200000    (200 us, continuous)
    ivh_tks_duty_pct    = 100       (no extrapolation)
    ivh_tks_phase_pct   = 0         (no tuned constant)
    ivh_tks_deadband_ns = 1000

Contended vCPUs, per 60 s window:

| quantity | host | kernel | error | ratio |
|---|---|---|---|---|
| steal  | 43.1% | 39.1% | -4.0 pp | **0.907** |
| active | 34.4% | 31.4% | -3.0 pp | **0.912** |

Undershooting is the safe direction for a capacity gate: it reports a
contended vCPU as slightly healthier than it is, so it under-triggers rather
than thrashing.

Run-to-run spread collapsed from 1.20-1.76 (tick-driven) to 0.90-1.07, so the
aliasing the old estimator suffered is gone live, not just in replay.

**Cannot claim:**

- Accuracy on lightly-loaded vCPUs with short-quantum preemption: **0.101** at
  the shipped setting.
- That active time is independently correct -- it is bounded by guest idle
  accounting. See `evaluation.md` §6.
- Cross-configuration comparison: the four configs were measured under
  different self-induced loads (host steal 43-63%), so only the ratios are
  comparable.

Method and full error budget: `evaluation.md` §4-§7.

---

## 5.2 Counting deschedules from TSC jumps

**Result: 97-106% recall against host `perf sched` for deschedules above
50 us, with 0.028 false jumps/s on a busy-but-unpreempted vCPU.**

Method: three independent witnesses over one window -- host `perf sched` on
the vCPU threads, the in-guest `ivh_vact` tick/raw-TSC detector, and
`vcpu_trace` as a cross-check. Reproduction in `evaluation.md` §12.3.

Measured 2026-09-26, cpu0, 45 s, `sampler_ns=0`, `ivh_vact_jump_ns` at its
1.5 ms default:

| host population | rate | detector recall |
|---|---|---|
| deschedules > 1.5 ms | 73.2/s | **105.7%** |
| deschedules > 1 ms   | 73.4/s | 105.4% |
| deschedules > 500 us | 76.8/s | **100.8%** |
| deschedules > 50 us  | 79.9/s | **96.9%** |
| ALL deschedules      | 104.4/s | **74.1%** |

**Specificity (the real negative control):** on vCPUs 8-15, which were
**70.9% active** but had only **0.131%** host steal, the detector produced
**0.028 jumps/s**. Busy and not preempted yields essentially no detections.

What it misses: 24.5 deschedules/s below 50 us -- 23% of events but, at ~11 us
each, under 0.1% of wall time. That is why steal-time accuracy is 0.907 (5.1)
while *event* recall is 74%.

**Claim "above 50 us" freely. Say 74% if claiming ALL deschedules.**

> ~~"The detector only achieves 8% recall"~~ -- WITHDRAWN, denominator error:
> it scored 96.5 jumps/s against 1272 host deschedules/s in a *dbench* window
> whose mean off-CPU was 290 us, i.e. against a population dominated by events
> an order of magnitude below the detector's threshold.

---

## 5.3 Detecting a preempted lock HOLDER

**Result: detection is real, host-validated, and beats stock PV in 10 of 10
paired runs.**

### The signal

Not the heartbeat. `is_cs_preempted()` criterion 1 compares

    held > o->last_cs + ivh_cs_noise_cycles

where `o->tsc` is an **event-driven** stamp the owner writes at acquire. It is
an exact `rdtsc` delta with no sampling floor, computed from a cacheline the
reader already owns. Operating point `ivh_cs_noise_cycles = 550000` (250 us),
chosen by the floor sweep below.

### Long holds are host preemption (dose-response)

The inferential link, and the reason none of this depends on an in-guest
oracle. Predictor is a guest-side `rdtsc` delta; ground truth is host-side
`schedstat wait_ns`. Two machines, two clocks, no shared instrument.
Harness: `/root/ivh_tools/dose.sh`.

| condition | host stolen | detector fires | long holds (ppm of all holds) |
|---|---|---|---|
| loaded A | 37.1% | 123.3/s | 79.3 |
| **IDLE** | 8.6% | **0.0/s** | **0.3** |
| loaded B | 32.5% | 75.4/s | 72.3 |

    corr(host stolen, fires/s)       = 0.970
    corr(host stolen, long-hold ppm) = 0.997
    loaded 75.8 ppm vs idle 0.3 ppm  = 253x separation

The pre-registered bar was <5 fires/s idle and ~100/s loaded; both met. The
**long-hold ppm column is the cleanest number** -- it comes straight from
`ivh_cs_prev_hold_hist` with no detector and no oracle involved, and the two
loaded runs agree within 9%. Long holds are not long critical sections; they
vanish 253x when the host stops stealing.

Caveat: 3 points, one idle. The r values are not strong statistics at n=3 --
the **effect size** carries this (0.0 vs 99.3 fires/s), not the correlation.

### Hold durations are bimodal

`ivh_cs_prev_hold_hist`, n=74,563,337, log2 TSC-cycle buckets:

| range | holds | |
|---|---|---|
| 7.4 us - 238 us | 48,368 | real critical-section work |
| **238 us - 477 us** | **121** | **valley, two orders deep** |
| 477 us - 15 ms | 3,344 | preemptions |

A raw histogram: no detector, no oracle. A raw spinlock cannot legitimately be
held 500 us -- preemption is off and it cannot sleep.

### The operating point

Floor sweep, hold-centric, scored as flagged/long-holds with false fires on
*short* holds tracked separately:

| floor | long holds | flagged | recall | false fires <477us |
|---|---|---|---|---|
| 500 us | 193 | 32 | 16.6% | 0 |
| **250 us** | **205** | **53** | **25.9%** | **0** |
| 100 us | 217 | 46 | 21.2% | 13 |
| 50 us | 193 | 50 | 25.9% | 15 |
| 10 us | 200 | 46 | 23.0% | 49 |

250 us is the setting: best recall in the sweep with **zero** fires on short
holds. Below it, false fires climb and recall plateaus (~24% pooled over the
four low-floor arms, 195/815).

### vs stock PV

Metric: of long (>=477 us) holds released with a queue present, did the head
stop spinning? Counter `ivh_cs_react[had_tail][state][bucket]`, read at the
holder's own unlock from the lock word, **strictly before the releasing
store**. Harness: `/root/ivh_tools/react3.sh`, `spinsweep.sh`.

Sweeping `ivh_pv_spin_threshold`, which sets when exhaustion can fire; the CS
floor stays at 250 us throughout:

| spin_thr | ~exhaust at | stock PV | IVH+CS | delta |
|---|---|---|---|---|
| 8192 | 112 us | 43.9% | 48.9% | +5.0 |
| 16384 | 223 us | 47.7% | 61.4% | +13.8 |
| 32768 | 447 us | 50.7% | 59.4% | +8.7 |
| 65536 | 894 us | 40.4% | 52.2% | +11.9 |
| **131072** | **1787 us** | **1.7%** | **24.3%** | **+22.6** |

**IVH wins all 10 pairs.** The bottom row is the isolation experiment: at
131072 iterations exhaustion takes ~1.8 ms, longer than almost any hold, so
stock PV halts the head **1.7%** of the time -- effectively switched off. The
CS predicate still gets **24.3%**, a **14x** gap with nothing else to credit.

This also explains why the same comparison at the default 32768 looked like
+1.2 and did not resolve: exhaustion already catches most long holds there, so
the detector competes for a sliver.

**Specificity:** `short-hold halted% = 0.00%` on every one of 38 runs.

Caveats: 2 reps per cell with run-to-run sd of 8-14 points, so no individual
delta is significant on its own -- **the 10/10 consistency and the 131072
isolation carry this**, not any single number. One workload (hackbench),
16 vCPUs, one contention level.

### Withdrawn from this sub-section

> ~~29% / 43% / 49% / 26-of-26 / 54-of-54 / "100% precision at every floor"~~
> WITHDRAWN. All were scored with `ivh_vact`, whose resolution is
> `jump_ns + driver period` = 500 us -- **coarser than the 10 us-and-up
> population the detector targets**. Only extreme-tail holds were ever
> judgeable (1-7 scoreable events per arm against 8-340 discarded), and a
> 7-arm floor sweep re-scored the *same* handful of multi-millisecond holds at
> every floor and reported it as seven confirmations. Independent arithmetic
> kills it too: at a 2 us floor at most 4.1% of fires can be preemptions
> (68,227 holds >2 us, only 2,807 in the preempted mode), so "100% at every
> floor" was structurally impossible.

> ~~Recall of 16-26%~~ WITHDRAWN as a statement about the mechanism: it
> measured the **bookkeeping**. The cross-CPU deposit ledger credited only 33
> of 104 distinct detections, and `ivh_cs_v_nested=0` /
> `ivh_cs_dep_clobbered=11` accounted for none of the other 71.

---

## 5.4 Marking a preempted WAITER (`VCPU_SKIPPED`)

**Result: 99.8% of waiters marked `VCPU_SKIPPED` had genuinely been gone for
>=477 us.**

### The self-check

The question in its cleanest form: *a waiter comes back, sees its own stamp
has not been refreshed for a while, checks its state -- is it `SKIPPED`?*

Counter `ivh_skipcheck[was_SKIPPED][gap_bucket]` (G-LOCK-48). The gap is
measured from the node's **own** `head_ctl` stamp, written by that CPU --
self-observation, no remote read, no cross-CPU deposit to lose. Harness:
`/root/ivh_tools/skipcheck.sh`.

Two record sites are required and it is not an optimisation:
`pv_wait_node()`'s loop tests `VCPU_SKIPPED` **before** it reaches
`ivh_node_publish_in_spin()`, so a waiter that returns and finds itself
skipped breaks out and never publishes. Instrumenting only the publish site
makes the numerator identically zero and reads as "0% recall" -- an artifact
indistinguishable from a real negative.

### Precision

`ivh_pv_evict_threshold=500us`, `hop_cap=2`, `lookahead=1`, sysbench on all
vCPUs:

| gap the marked waiter measured | marked SKIPPED |
|---|---|
| 0.23 us - 0.47 us | 3 |
| 476.6 us - 953 us | 34 |
| 953 us - 1.9 ms | 1,504 |
| 1.9 ms - 3.8 ms | 145 |
| 3.8 ms - 7.6 ms | 1 |
| **TOTAL** | **1,687** |
| **away >= 477 us** | **1,684 = 99.8%** |

Only 3 of 1,687 were not genuinely away, and those measured sub-microsecond
gaps -- consistent with a waiter marked and re-stamped in quick succession
rather than a misfire.

Instrument check: 1,687 of the evictor's 1,689 reported marks were captured
(**99.9%**), so unlike the G-LOCK-45 ledger this is not losing events.

### Recall, and why it is 8.2%

Of 20,606 waiters that returned from a >=477 us absence, 1,684 were marked:
**8.2%**. This is **not** a detection failure. The funnel:

    ivh_evict_walks              1,819,641
    ivh_evict_walks_acted            1,489   (0.08%)
    ivh_evict_marked                 1,490
      lookahead_refused          1,798,005   <-- 98.8% of every walk
      stop_halted                   20,284
      cap_refused                        0
      halt_race                          0

The walk **finds** the stale waiters 1.8 million times. Look-ahead then
declines to mark 98.8% of them, because within `hop_cap` hops there is no
non-preempted node to promote instead -- skipping a stale waiter to reach
another stale waiter gains nothing and pays the requeue cost. Under sysbench
on all 16 vCPUs the queue is uniformly preempted, so that condition is
almost always true. `cap_refused=0`: `hop_cap` never refused a mark directly.

So 8.2% is the **policy** declining, not the predicate missing. Recoverable
lever: `hop_cap` widens look-ahead's search for a live node (prior measurement
in `evaluation.md`: 100% no-live-waiter at hop_cap=1, 42.9% at hop_cap=4).
Not yet swept with the corrected threshold.

### Boundary on 5.3 and 5.4

"Away >= 500 us" is the waiter's **self-observed stamp gap**, and "held
> 250 us" is the holder's own `rdtsc` delta. That either means **host
preemption** is established at the *population* level by the dose-response in
5.3 (253x), not per event. The defensible sentence is "99.8% of marked waiters
had genuinely stopped executing"; the link from that to host descheduling
rests on 5.3.

Not excluded per-event: a holder can run long because it blocked on an *inner*
lock whose holder was preempted -- this vCPU was never descheduled. The
dose-response cannot separate that, because the convoy is also created by host
load. Settling it needs per-event correlation of guest hold windows against
host `perf sched` timestamps (possible -- `perf`'s `TIME_CONV` record converts
host samples to TSC, and the TD's TSC offset is fixed).

---

## 5.5 Configuration required to reproduce section 5

Three predicates read three different clocks and need three different
thresholds. They are no longer allowed to share a knob (G-LOCK-47).

| predicate | knob | value | signal | noise floor |
|---|---|---|---|---|
| `is_cs_preempted` (holder) | `ivh_cs_noise_cycles` | 550000 (250 us) | acquire stamp, event-driven | none (exact) |
| eviction (`VCPU_SKIPPED`) | `ivh_pv_evict_threshold` | 1100000 (500 us) | per-node stamp, every 4096 spin iters | ~47-110 us |
| tier 2 `is_wait_preempted` | `ivh_pv_beat_threshold` | 11000000 (5 ms) | per-CPU heartbeat, tick-bound | **~3 ms** |

`ivh_pv_evict_threshold` self-calibrates from `tsc_khz` at `late_initcall`.
`ivh_cs_noise_cycles` does **not** -- it compiles to 22000 (10 us) and is
pinned in `goto_mode.sh`; without that line the 250 us operating point
silently reverts 25x on every reboot.

**Tier 2 is effectively off at any sound setting** and should be treated as
such: it fires 0-21 times per run at 5 ms, against ~99 host preemptions/s.
Its per-CPU heartbeat floor (~3 ms, p99 of max staleness across 16 vCPUs,
400 samples, idle box) sits *above* the 250-450 us preemption scale it is
meant to detect, so no threshold separates signal from noise. That floor is
set by **host-delayed ticks** -- i.e. by host preemption itself -- so the
threshold has no fixed point, which is why it drifted 100 us -> 1.5 ms ->
3.3 ms -> 5 ms.

> ~~"5 ms is the accurate setting for tier 2"~~ WITHDRAWN. It was chosen by
> "mean duration of the halt each fire caused", which rises with the threshold
> **mechanically** -- a fire at threshold T requires the beat to be >=T stale.
> The honest reading of n=21 is that tier 2 is **off**, not precise.

Before tier 2 is deleted, the recorded `tier1+tier2` hackbench win (-3.76%,
8/10 SIG) must be re-attributed: at 0-21 fires tier 2 cannot have produced it,
so it was tier 1 alone, or tier 2 acting at a low threshold as an
unconditional early-bail (a spin-threshold cut, not a detector).

---

## 5.6 Harnesses

| script | produces |
|---|---|
| `/root/ivh_tools/dose.sh <label>` | 5.3 dose-response; needs sysbench toggled |
| `/root/ivh_tools/react3.sh` | 5.3 vs stock PV, 3 arms x N reps, round-robin |
| `/root/ivh_tools/spinsweep.sh` | 5.3 spin-threshold sweep incl. the isolation row |
| `/root/ivh_tools/skipcheck.sh` | 5.4 waiter precision |
| `/root/ivh_tools/evict_funnel.sh` | 5.4 funnel and refusal attribution |
| `/root/ivh_tools/cs_floor.sh` | 5.3 floor sweep |

Host ground truth: re-resolve the TD's pid every boot (`virsh domid`); a stale
pid reads zeros silently and looks exactly like an idle host. Console is
`hvc0`, not `ttyS0`.

---

# 1. Kill PLE

NOT STARTED. Plan: benchmark with PLE on, legacy vs CVM, with and without a
sysbench co-runner, Intel and AMD. Track PLE exit count and wait length; show
both rise under contention on legacy but stay 0 on Intel CVM. AMD needs a
rented server; only the Intel setup is done here.

# 2. Kill PV

NOT STARTED. Show migration + adaptive spinning beats pvqspinlock and a TAS
lock on one kernel and one user benchmark.

# 3. How many cloud instances use PV spinlock

NOT STARTED. CLI sweep across sizes, regions, clouds (GCP/AWS/Azure) and
instance types. Host-side work.

# 4. How many threads are migratable

NOT STARTED. migratable/total per workload across the suite.

# 6. Migration impact

NOT STARTED. % improvement and preempted-vCPU reduction vs pvqspinlock, from
NHextend plus the 2 best non-microbenchmark kernel workloads, split across 4 /
8 / 16 contended vCPUs.

# 7. Time-left sensitivity

PARTIAL. Curve taken; see `evaluation.md` §11. Needs the cache-locality and
migration-cost arms to converge on 4 ms.

# 8. Budget sensitivity

NOT STARTED. Same method as 7, for the concurrent-migration budget
(`ivh_max_concurrent`, currently 8).

# 9. Cost vs gain

NOT STARTED. Migration cost, syscall cost (0 in kernel, positive for
NHextend), and % preempted-vCPU reduction.

# 10. Adaptive spinning is good

PARTIAL. Detection is established in 5.3/5.4. **Throughput is not measured.**
`ivh_cs_head_bail` has been detect-only throughout, so the predicate has never
been allowed to change behaviour in a throughput run. Lock skipping is
recorded as null on real workloads in `evaluation.md`; head bypass has two
contradictory entries and needs re-taking now that tier 2 is going away.

# 11. Iterations before deciding preempted

PARTIAL. Publish cadence is `ivh_pv_beat_publish_mask+1` = 4096 spin
iterations (~47 us bare `cpu_relax`, ~60-110 us with the real loop body); the
node-stamp floor that follows from it is in 5.5. The 0.5 ms staleness cut is
in use for eviction. Not yet swept for wait-time/throughput as the point asks.

# 12. Full test on 1 VM

NOT STARTED. Full suite, migration+AS vs pvqspinlock, at 16 / 32 / 64 vCPUs.

# 13. Scalability

NOT STARTED. As 12 with 2 and 4 co-running sysbench VMs; likely restricted to
the top 8 workloads.

# 14. Our weaknesses

NOT STARTED. Slowdown vs pvqspinlock on non-spinlock-intensive workloads, and
on pinned spinlock-intensive workloads where migration cannot run.

# 15. Workload reliability

DONE, with three limits stated at the end. Measured 2026-09-27 on
`6.17.0-G-LOCK-48-skipcheck+`.

**Purpose.** Rank the candidate workloads by how hard they exercise kernel
locks, so the parameter-sensitivity points (7, 8, 11) can be run on 5-6
workloads spanning that range instead of ~20 through every arm. At 8 arms and
2 reps the full set costs 2.58 h per sweep; a stratified subset costs a
fraction of that and represents the range better than the top of it.

## 15.0 Method

**Config.** Single arm, stock PV (`/root/spin_mode 1`, `ivh_adaptive_mode==0`
asserted), `ivh_universal_eligible=0`, `ivh_tks_sampler_ns=0`, CS stamping
armed AFTER `spin_mode` (it clears `ivh_cs_owner_enable`), `drop_caches` before
every run. There is no second arm because this characterises the WORKLOAD, not
IVH -- there is nothing to compare against.

**Instruments.** Two, independent, both safe:

| instrument | what it counts |
|---|---|
| `perf stat -a -e lock:contention_begin` | kernel spinlock contention, exact |
| `ivh_cs_prev_hold_hist` (sum) | acquisitions that formed a stamped MCS queue |

They agree on the ranking, which is what makes the stratification trustworthy.
`fentry` on `_raw_spin_lock*` was rejected: bpftrace flags it a "dangerous
function" that risks kernel deadlock *and* drops events under its own
mitigation, and a lossy counter cannot stratify.

**Procedure.** Each workload runs 3x; any workload flagged UNSTABLE (>3x
spread across reps), NEAR-IDLE (<2x background) or SHORT (<1 s) then gets +5
reps. Medians over all reps are reported. `drop_caches` before every run.
Anomaly re-runs additionally sampled a 3 s idle background before each run --
see 15.5 limit 2 for why that correction was discarded.

---

## 15.1 The suite, ranked by lock-acquisition rate

All 16 workloads at the Appendix A configurations. `contended/s` is the median
PV-arm rate; `PV s` is the PV-arm duration at the recorded config.

**Kernel-lock workloads -- the axis is valid here**

| workload | contended/s | PV s | recorded | note |
|---|---|---|---|---|
| `stressng_dentry` | 570,785 | 16.1 | +99.5% | settled |
| `hackbench_pipe_thr` | 181,050 | 19.0 | +76.3% | settled |
| `sysbench_mutex` | 15,754 | 6.3 | +19.8% ‡ | scaled cfg (`--mutex-locks=600000`) |
| `ebizzy_mmap` | 13,387 | 15.3 | +104.3% | settled |
| `dbench_16` | 7,388 | 18.4 | +19.1% | settled |
| `fsmark_tmpfs` | 4,672 | 8.1 | +208.8% ‡ | scaled cfg (`-n 30000`) |
| `wis_mmap2` | 3,412 | 21.4 | +11.2% | settled |
| `perf_epoll_wait` | 395,263 | 16.2 | +53.9% | settled (13% spread) |
| `schbench` | 291 | 15.4 | +7.4% | settled |

Range **291 -> 570,785 /s, a 1,961x span.**

**Userspace-sync workloads -- the axis is INVALID here**

| workload | contended/s | PV s | recorded | note |
|---|---|---|---|---|
| `nhextend_full` | 7,637 | 8.2 | +64.0% | AFL vs spin-only, not IVH vs PV |
| `parsec_vips` | 2,077 | 17.8 | +57.39% | userspace sync |
| `parsec_bodytrack` | 1,107 | 67.8 | +14.39% | userspace sync |
| `parsec_dedup` | 1,035 | 46.5 | +86.86% | userspace sync |
| `parsec_blackscholes` | 128 | 26.1 | +8.01% † | BIMODAL 5 reps ~127 / 3 reps ~1,447 |
| `parsec_swaptions` | 67 | 42.0 | +10.43% | BLIND: at/below ~64/s idle floor |
| `parsec_ferret` | 58 | 77.9 | +15.21% | BLIND: at/below ~64/s idle floor |

Idle background on this box is ~64/s. `parsec_ferret` medians **58/s** and
`parsec_swaptions` **67/s** -- at or BELOW that floor against clean baselines
-- while carrying +15.21% and +10.43% recorded wins. They synchronise via
`pthread_mutex` -> futex, which `lock:contention_begin` never observes. **A low
reading here means the instrument is blind, NOT that the workload has little
contention.** These rows must not be ranked against the kernel-lock table.
`nhextend_full` is the exception: its adaptive futex lock makes real futex
syscalls that take the kernel's `hb->lock`, so it registers at 7,637/s.

**Every row above was measured at the Appendix A config.** Five were first
measured at wrong invocations and re-measured on 2026-09-28
(`ivh_tools/point15_recheck.sh`): `dbench_16` 9,381 -> 7,388 (added `-F`),
`wis_mmap2` 3,623 -> 3,412 (`-s 10` -> `-s 15`), `schbench` 299 -> 291
(`-m 4 -t 4` -> `-m 2 -t 8`), `sysbench_mutex` 14,887 -> 13,390 (32 threads /
20k locks -> 16 / 40k), `hackbench_pipe_thr` 248,507 -> 181,050
(`-g 8 -f 20 -l 2000` -> `-g1 -f8 -l150000`). **No stratum assignment changed.**

## 15.2 Stratified subset for points 7, 8 and 11

| stratum | workloads | contended/s |
|---|---|---|
| INTENSE | stressng_dentry, hackbench_pipe_thr | 570,785 / 181,050 |
| MID | ebizzy_mmap, dbench_16 | 13,387 / 7,388 |
| LOW | wis_mmap2, schbench | 3,412 / 291 |

All six clear 5 s in both arms. PARSEC is excluded because it cannot be
placed on this axis at all. `perf_sched_pipe` is no longer in the suite (see
A.2): it is a 2-task benchmark and was also the only one bimodal on lock rate
(356, 361, **5,111** /s), which would have read as a threshold effect that is
not there.

## 15.3 Lock rate does not predict the benefit

Demonstrated at both ends, which is stronger than the previous evidence for
this claim:

| workload | contended/s | recorded IVH win | in the suite? |
|---|---|---|---|
| `parsec_dedup` | 1,035 | **+86.86%** | yes |
| `psearchy` | 5,536 | **+0.82%, not significant** | no -- negative control |

5.3x the lock rate, and the benefit inverts. `psearchy` is not part of the
15-workload suite (it shows no benefit, which is the point of citing it);
its rate was measured in the same pass, at the `parsec_ab`-style invocation
given in the point-15 harness. This supports the existing
"blocking structure predicts the win, lock rate does not" finding with a direct
measurement of the rate rather than an inference from it.

## 15.4 Limits of this measurement

1. **PARSEC cannot be placed on this axis at all** (15.2). Stratifying PARSEC
   requires a futex-rate instrument that does not exist yet.
2. **Background subtraction was tried and FAILED.** Sampling `lock:contention_
   begin` for 3 s immediately before each run, then subtracting, gave nonsense
   negative rates: **14 of 35 samples (40%)** were contaminated by the
   *previous* run's teardown, and the contamination clusters on the long PARSEC
   runs. The `net` column was discarded. Raw medians over 5+ reps are what
   worked.
3. **A discarded warmup run is missing and is needed.** `psearchy_ab.sh` has
   one; the point-15 harness did not. Its absence produced the two largest
   artifacts seen -- `fsmark_tmpfs` 86,939/s and `parsec_swaptions` 685/s, both
   first reps following a different workload. This must be added before points
   7/8/11, where arm order rotates and a contaminated first rep per arm biases
   whichever arm happens to run first.

Also noted: `fsmark_tmpfs` (0.7 s) and `sysbench_mutex` (0.6 s) complete in
under a second at their recorded invocations. Their *rates* are stable anyway
(sysbench 8 reps within 10%), but their *throughput* figures at that duration
are dominated by startup, which matters for points 7/8/11 and not for this one.


## 15.5 Watch list -- re-check after parameter tuning

Three workloads sit below the +5% bar today but are kept under observation:
tuned Gate 2 / Gate 4 / lock-skip parameters (points 7, 8, 11) could move them
over it, and one of their cohort already moved.

| workload | latest | evidence | config |
|---|---|---|---|
| `parsec_canneal` | **+2.01%, NOT sig** | 4/4 pairs, t=2.60 vs crit 2.776 at n=4 — **underpowered, stopped at 4 of 6 pairs**. Recorded +0.14% ns. 249 cont/s, PV 85.0 s | `./bin/parsecmgmt -a run -p canneal -c gcc -i native -n 16` |
| `psearchy` | +0.82%, NOT sig | 6/8 pairs. 5,536 cont/s (real kernel contention), PV 35.3 s | `cd /root/mosbench/psearchy && ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x` |
| `tinyconfig` (kernel build) | +0.99%, sig but tiny | 9/10 pairs. 2,409 cont/s, PV 39.6 s | `rm -rf $B; make -C /root/kernels/linux-6.14-stock O=$B tinyconfig` then time `make -C ... O=$B -j16 vmlinux` |

**Why they are worth re-checking.** All three were retired on the same
sampler-corrected pass (evaluation.md 10.2) that also retired
`parsec_blackscholes` — which, re-tested on 2026-09-27, came back at **+8.01%,
6/6, t=8.88** and is now in the suite. The same doubt applies to these three.
Their sampler gaps were psearchy +4.8pp (5.2 -> 0.8) and tinyconfig +5.6pp
(7.2 -> 1.0), i.e. the same mechanism.

`parsec_canneal` is the strongest candidate of the three: it was directionally
positive on every pair measured and missed significance only because the run
was stopped early. **Finishing its last 2 pairs is the cheapest outstanding
measurement in this file.**

`tinyconfig` must wipe the build directory and build the `vmlinux` target;
without the wipe `make` returns in ~0.5 s having built nothing.

## 15.6 Harnesses

| file | role |
|---|---|
| `ivh_tools/point15_lockrate.sh` | main pass, 20 workloads x 3 reps |
| `ivh_tools/point15_rerun_anomalies.sh` | +5 reps for UNSTABLE / NEAR-IDLE / SHORT |
| `ivh_tools/point15_report.py` | report, splits the two groups |
| `ivh_tools/ivh_benchmarks.sh` | workload registry with recorded deltas |
| `ivh_tools/point15_0927-220345.csv` | raw data (`_rerun.csv` for the re-runs) |
