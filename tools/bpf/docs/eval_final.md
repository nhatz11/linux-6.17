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
| 7 | **Time-left sensitivity** | **DONE** -- no threshold is distinguishable; ship 4 ms as the cheapest |
| 8 | **Budget sensitivity** | **DONE** -- the value is bounded by the healthy-vCPU count, not tuned |
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

**Head bypass is dead for the SAME reason, and has a second gate on top**
(measured 2026-09-28). `ivh_head_observe()` applies the identical staleness
test against `ivh_pv_beat_threshold`, so at the shipped 5 ms it too never
fires. One hackbench run per threshold:

| `beat_threshold` | obs_samples | obs_stale | held | free_open | actionable | bypass fired | tier2 fired |
|---|---|---|---|---|---|---|---|
| 11,000,000 (5 ms) | 1,726,222 | **0** | 0 | 0 | 0 | **0** | 456 |
| 2,200,000 (1 ms) | 2,189,903 | 23,160 | 1,311 | 21,435 | 414 | 412 | 102,814 |
| 220,000 (100 us) | 2,180,193 | 97,543 | 13,492 | 83,032 | 1,019 | 1,019 | 356,775 |

The observer itself runs -- 1.7M samples per run -- and dies at the staleness
test. Two further facts follow from the table:

1. **`ivh_head_bypass_enable=1` is not sufficient.** The bypass code lives
   inside `ivh_head_observe()`, whose only call site (`qspinlock_paravirt.h`
   ~2010) is gated on `ivh_head_bypass_probe`, which also defaults to 0. Both
   sysctls are required before a single bypass can fire.
2. **The addressable case is ~2% of the opportunity even when tuned to fire.**
   At 1 ms, 21,435 of 23,160 stale heads are `free_open` -- free AND already
   stealable, where "a bypass adds nothing" -- against 414 actionable (1.8%).
   This is the same shape as lock skipping's failure in section 11: the
   unfair steal valve already covers most of the target case.

Consequence for earlier results: **any arm labelled `t12+bypass` at the shipped
`beat_threshold` was tier 1 only.** That includes every IVH arm of the point 11
sweep. The recorded `t12+bypass = +5.73% vs stock PV` must state which
threshold it ran at before it can be relied on.

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

DONE. 2026-09-28. `ivh_time_left_threshold_ns` swept 250 us - 16 ms, 6 workloads
x 8 arms x 5 reps = **240 runs** (`ivh_tools/point7_full.sh`, raw data
`p7full_data.csv`, report `ivh_tools/point7_report.py`).

**The claim.** Throughput is statistically indistinguishable across the whole
useful range of this constant, while migration cost rises monotonically with it.
4 ms is shipped as the cheapest threshold at which no performance is given up --
not as an optimum.

## 7.1 Method

Workloads are the stratified six from 15.3. Four measurements per run, kept
SEPARATE (no ratio is formed in the harness):

| metric | source |
|---|---|
| perf | workload metric, direction-corrected so higher is better |
| ipi/kwork | RES+CAL+TLB per 1000 units of work, work = perf x duration. **Not** per unit throughput -- that double-counts the speedup on fixed-work benchmarks |
| wait_ns | `ivh_slowpath_wait_ns`, aggregate spinlock wait (in-kernel) |
| mig_cost | stopper dispatch + the actual move, **excluding** target-runqueue wait |

**Migration cost is measured, not inferred**, by bpftrace with no kernel change:
`cpu_stop_queue_work` -> `migration_cpu_stop` entry gives dispatch (~2 us);
`migration_cpu_stop` entry -> return gives the move (~3 us). Cross-checked
against an independent method (total `set_cpus_allowed_ptr` time minus the
`sched_info.run_delay` delta, which is valid at `sched_schedstats=0` and never
accrues while blocked): 4.8 us vs 6.1 us, agreeing to ~25%. Probe overhead is a
1-2 us floor against a ~3 us signal, so these are order-of-magnitude figures --
adequate for showing migration is not free, not for a precision claim.

**Target-runqueue wait is logged but NOT counted as migration cost.** It is
~1.5 ms modal, which is one EEVDF `base_slice_ns` (2.8 ms on this guest): the
migrated thread lands behind an incumbent and waits a scheduling quantum. That
is a property of how loaded the target is, not of the migration mechanism. Per
migration: move ~3 us, dispatch ~2 us, **runqueue wait ~1,500 us**.

**Capacity settling is filtered, not averaged over.** The PV arm perturbs
`ivh_uc_capacity` for ~2-3 runs afterwards; during that window Gate 1 passes
everything and the arm is not comparable. Rows with `g1_reject < 50,000` are
excluded from the averages and the count of exclusions is printed.

## 7.2 Result, pooled across the six workloads

Normalised per workload first, so hackbench's 69 s of spin does not dominate
sysbench's 0.17 s.

| arm | perf vs PV | ipi vs PV | spin saved | **migrations** | mig cost | us/mig |
|---|---|---|---|---|---|---|
| 250 us | +32.8% | -8.5% | 0.35 s | 5,258 | 76.6 ms | 11.3 |
| 500 us | +30.6% | -8.1% | 0.33 s | 4,440 | 59.2 ms | 8.6 |
| 1 ms | +36.3% | -8.9% | 0.44 s | 4,708 | 65.7 ms | 10.0 |
| 2 ms | +35.9% | -5.9% | 0.29 s | 5,570 | 76.9 ms | 10.0 |
| **4 ms** | **+40.3%** | **-10.3%** | **0.43 s** | **5,318** | **78.8 ms** | **9.7** |
| 8 ms | +42.6% | -4.6% | 0.53 s | 6,258 | 90.4 ms | 9.6 |
| 16 ms | +45.0% | -9.8% | 0.50 s | 6,934 | 98.8 ms | 10.8 |

## 7.3 The differences are not statistically resolvable

| comparison | n | mean | t | crit | verdict |
|---|---|---|---|---|---|
| 250 us vs 16 ms (whole range) | 6 | +44.8 pp | 1.33 | 2.571 | **not significant** |
| 4 ms vs 8 ms | 6 | +11.3 pp | 1.06 | 2.571 | **not significant** |

Best arm per workload is scattered: 250 us x1, 2 ms x1, 4 ms x2, 8 ms x2. The
apparent monotone rise in 7.2 is driven almost entirely by `parsec_dedup`, which
swings **+63 pp between adjacent arms** and whose PV baseline varies 58-175 s.
**Three of six workloads prefer 4 ms to 8 ms.**

> ~~"performance rises monotonically with the threshold and it is not noise-flat"~~
> **WITHDRAWN** (stated in-session before the paired test was run). The pooled
> median rises but per-workload variance swamps it; t=1.33 over the full range.

## 7.4 What IS directionally consistent: the cost

Migration cost rises with the threshold -- 78.8 -> 90.4 -> 98.8 ms across
4/8/16 ms -- and the mechanism is mechanical: a looser gate admits more
candidates, so more migrations happen. Migrations at 4/8/16 ms on `ebizzy_mmap`:
5,446 -> 7,202 -> 7,735. `dbench_16`: 50,252 -> 51,384 -> 54,490.

Pooled migration counts track the cost directly, which is the mechanism:
the gate admits more candidates, more migrations happen, more cost is paid.

Since throughput cannot be distinguished between arms and cost can, the cheapest
arm that loses nothing is the rational choice. That is the argument for 4 ms.

> **The congestion hypothesis is NOT supported.** Per-migration cost (`us/mig`)
> sits at 8.6-11.3 us with no trend across a 64x threshold range. Two per-arm
> observations had suggested it rose with the gate (4.8 -> 8.7 us on sysbench,
> 9.2 -> 11.9 us on dedup); those do not survive averaging over 6 workloads.
> Total cost grows because there are MORE migrations, not because each is dearer.

## 7.5 The negative control worked

`dbench_16` was included deliberately as a workload known flat on this knob
(15.3). It behaved as designed: **migrations 36,628 -> 54,490 across the sweep
(+49%) while throughput stayed at +7.9% to +9.6%.** Migration volume changed
substantially and throughput did not, which rules out a systematic artifact
affecting all arms equally.

## 7.6 Limits

1. **The 250 us and 500 us arms lost reps** to capacity-unsettling -- hackbench
   dropped 4 of 5 at 250 us and 3 of 5 at 500 us. The low end rests on n=1 and
   n=2 and is the weakest part of the curve.
2. **`ipi/kwork` is unreliable on PARSEC.** dedup's swings +203% to -10.8% across
   arms; its PV baseline is too variable. The column is solid on hackbench
   (-76 to -80%) and sysbench (-61 to -66%), where it is also flat across arms.
3. **`parsec_dedup` dominates every pooled mean.** Any pooled figure should be
   sanity-checked with it removed.
4. Migration cost carries 1-2 us of probe overhead (7.1). Constant across arms,
   so arm-to-arm ranking is unaffected; absolute values are not precise.

# 8. Budget sensitivity

DONE. 2026-09-28. `ivh_max_concurrent` swept 2 / 4 / 8 / 16 (1/8, 1/4, 1/2, 1x
nproc on this 16-vCPU guest), 6 workloads x 5 arms x 5 reps = **150 runs**
(`ivh_tools/point8_full.sh`, raw data `p8full_data.csv`). `ivh_time_left_threshold_ns`
pinned at 4 ms throughout so only the budget varies.

**The claim.** The budget is bounded above by the number of healthy vCPUs,
because each in-flight migration reserves its target. Above that value the gate
never fires at all; below it the gate throttles migrations but throughput does
not change. The shipped value of 8 equals the healthy-vCPU count on this guest,
so it is **derived from the machine rather than tuned**.

## 8.1 Gate 4 has no reject counter, so occupancy was sampled directly

Gate 4 is `fair.c:13942`:

```c
if ((unsigned long)atomic_read(&ivh_in_schedule) >= ivh_max_concurrent)
        return;
```

Nothing increments a counter there, so unlike Gates 1 and 2 its firing rate
cannot be read off. Instead `ivh_in_schedule` is sampled at 0.5 ms during every
run and reported as `sc_max` (peak occupancy) and `at_cap%` (share of samples at
or above the budget, i.e. when the gate was rejecting).

`ivh_in_schedule` is held across the **whole** `set_cpus_allowed_ptr()` call,
including the ~1.5 ms target-runqueue wait (see 7.1), so occupancy is dominated
by that wait rather than by migration work.

> **Two earlier readings of this counter were WRONG and are withdrawn.**
> ~~"ivh_in_schedule is 0 in 9,551 samples"~~ and ~~"0 in 60,724 samples",~~ from
> which ~~"Gate 4 never binds; the budget is ~500x oversized"~~ was concluded. A
> held-open `/proc/kcore` fd serves reads from the page cache and reports a
> FROZEN value with no error. Verified by sampling `ivh_migrations_done`: 119
> identical samples while a reopen-per-call reader showed it advancing by 7,537.
> The sampler now reopens per sample.

## 8.2 Result, pooled across the six workloads

| cap | perf vs PV | ipi vs PV | spin saved | migrations | cost | us/mig | at_cap% |
|---|---|---|---|---|---|---|---|
| 2 | +45.6% | -10.1% | 0.59 s | 6,327 | 85.4 ms | 10.2 | **7.2%** |
| 4 | +46.6% | -10.1% | 0.58 s | 6,992 | 86.6 ms | 9.8 | 1.1% |
| **8** | +42.8% | -5.9% | 0.56 s | 7,968 | 91.7 ms | 8.9 | **0.0%** |
| 16 | +44.7% | -2.3% | 0.67 s | 7,519 | 97.2 ms | 8.5 | **0.0%** |

| comparison | n | mean | t | verdict |
|---|---|---|---|---|
| cap2 vs cap4 | 6 | +8.1 pp | 1.37 | not significant |
| cap4 vs cap8 | 6 | +6.9 pp | 1.22 | not significant |
| cap8 vs cap16 | 6 | -4.2 pp | **-0.52** | not significant |
| cap2 vs cap16 | 6 | +10.7 pp | 1.05 | not significant |

## 8.3 The ceiling, which is the actual finding

`sc_max` never exceeds 8-10 at any budget, and `at_cap%` is **0.0% at both cap8
and cap16**. The mechanism is target reservation: `fair.c` does
`atomic_fetch_or(PRMPT_HELD_MASK, prmpt_flags(target_cpu))` on selection and the
BPF selector rejects any already-claimed CPU (`REJ_CLAIMED`, 4.0M rejections
measured). With **8 healthy vCPUs**, at most ~8 migrations can hold distinct
targets concurrently.

So a budget above the healthy-vCPU count is unreachable by construction. cap8
and cap16 are the same configuration in practice, and their comparison is
correspondingly the most null in the table (t=-0.52).

**Below the ceiling the gate does throttle**, and visibly: `at_cap%` runs
7.2% pooled at cap2, reaching **20.9% on hackbench** and **18.4% on
sysbench_mutex**. Migrations fall accordingly, 7,968 at cap8 to 6,327 at cap2.

`sc_max` occasionally exceeds the budget (10 at dedup/cap16, 3 at cap2) because
Gate 4 is a racy `atomic_read` with no serialisation -- a few candidates slip
past concurrently. The budget is soft, not a hard limit.

## 8.4 Throughput is indifferent; cost is not

Migrations rise +26% (6,327 -> 7,968) and cost +14% (85.4 -> 97.2 ms) from cap2
to cap8, while throughput stays in a 42.8-46.6% band with no significant
difference anywhere. Same structure as point 7: the knob demonstrably changes
migration volume and throughput does not respond.

The pooled median mildly favours cap2/cap4, but the per-workload picture is
mixed and should not be read as a result: four workloads prefer the low caps by
0.1-4.9 pp while `hackbench` and `parsec_dedup` prefer cap8 by **46.6 pp and
53.3 pp**. hackbench's cap2 row is n=1 (4 of 5 reps lost to capacity-unsettling)
and its cap4 row is n=3.

**Choice.** cap8 is recommended because it is *derived* -- it equals the
healthy-vCPU count, which is where target reservation caps concurrency anyway.
cap4 is equally defensible on cost grounds (86.6 ms vs 91.7 ms, saturating only
1.1% of the time so it keeps headroom). No measured optimum supports either:
every pairwise test is insignificant.

## 8.5 Limits

1. **hackbench lost reps at the low caps** -- n=1 at cap2, n=3 at cap4. The low
   end of its curve is the weakest data in the sweep.
2. **The ceiling is a property of this host's contention pattern.** 8 healthy
   vCPUs is what this host happened to leave; on a host starving a different
   fraction the meaningful ceiling moves with it. The *rule* (budget <= healthy
   vCPU count) generalises; the *value* 8 does not.
3. cap2's 7.2% saturation is fine at the contention level tested but is the arm
   most likely to degrade under heavier host contention, which was not tested.
4. `ipi vs PV` weakens monotonically with the cap (-10.1% to -2.3%) but carries
   the same PARSEC instability noted in 7.6.

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

DONE, as a NULL. Measured 2026-09-28 on `6.17.0-G-LOCK-48-skipcheck+`.
Raw data: `p11full_data.csv` (180 rows).

**The knob does not matter, because the predicate barely fires.** Sweeping the
eviction staleness cut over a 20x range produces no distinguishable change in
either throughput or wait time. This is a firing-rate result, not a tuning
result, and it corroborates the `evaluation.md` entry recording lock skipping as
null on real workloads.

Publish cadence is `ivh_pv_beat_publish_mask+1` = 4096 spin iterations (~47 us
bare `cpu_relax`, ~60-110 us with the real loop body); the node-stamp floor that
follows from it is in 5.5. The shipped cut is 1,100,000 cycles = 500 us.

## 11.1 Method

`ivh_pv_evict_threshold` at 220,000 / 550,000 / 1,100,000 (shipped) / 2,200,000
/ 4,400,000 cycles = 100 us / 250 us / 500 us / 1 ms / 2 ms at 2200 MHz, plus a
stock-PV arm. 6 arms x 5 reps x 6 workloads = 180 runs, arm order rotated each
rep. Harness `point11_full.sh`, report `point11_report.py`.

Two metrics, both in-kernel counters so the measurement costs nothing:
throughput, and `ivh_slowpath_wait_ns` aggregate wait. No p99 (out of scope for
this point). Eviction counters (`ivh_evict_marked`, `..._requeued`,
`..._lookahead_refused`) are logged per run so a flat result cannot be confused
with a knob that never engaged.

**Two prerequisites, both off by default.** Without BOTH of
`ivh_pv_evict_enable=1` and `ivh_pv_evict_node_stamp=1`, eviction fires zero
times at every threshold from 500 us down to 10 us. `node_stamp` is the real
blocker: at 0 the staleness test falls through to the per-CPU heartbeat arm
gated on `ivh_pv_beat_threshold` (5 ms), which sits below its own ~3 ms noise
floor and is documented as dead in 5.5. Both are asserted per arm.

**CORRECTION (2026-09-28, after the sweep).** This sweep's IVH arms were
configured with tier 1, tier 2 and head bypass all enabled, and are described
below as "the spin-side stack". They were in fact **TIER 1 ONLY**. Both tier 2
and head bypass read `ivh_pv_beat_threshold`, shipped at 11,000,000 cycles
(5 ms), and at that value neither fires. Measured directly, one hackbench run
per threshold:

| `beat_threshold` | obs_samples | obs_stale | actionable | bypass fired | tier2 fired |
|---|---|---|---|---|---|
| 11,000,000 (5 ms, shipped) | 1,726,222 | **0** | 0 | **0** | 456 / 6.3M checks |
| 2,200,000 (1 ms) | 2,189,903 | 23,160 | 414 | 412 | 102,814 |
| 220,000 (100 us) | 2,180,193 | 97,543 | 1,019 | 1,019 | 356,775 |

The observer runs (1.7M samples/run); it dies at the staleness test -- at 5 ms
NO head is ever judged preempted. Head bypass additionally requires
`ivh_head_bypass_probe=1`, because the bypass code lives inside
`ivh_head_observe()` whose only call site is gated on that sysctl; it too
defaults to 0.

Nothing in the point 11 conclusion changes -- the eviction threshold is still
inert, and that finding rests on eviction counters, not on tier 2 or bypass.
But wherever this section says the ~+1% throughput common to every arm is
"tier1+tier2+head bypass", read **tier 1 alone**. Whether tier 2 and bypass
help at a threshold where they fire is a separate question, measured in the
t12+bypass sweep.

**Migration is OFF in every arm** (`ivh_universal_eligible=0`, asserted). Lock
skipping is a queue-ORDER decision; vCPU placement is a separate mechanism whose
variance here is far larger than the effect being swept. A first smoke run with
migration on showed why: the arm that happened to follow the PV arm did 10,447
migrations while the others did 21-293, because the PV arm perturbs
`ivh_uc_capacity` for 2-3 runs afterwards and Gate 1 then passes everything.
With migration off the wait spread across arms fell from 8 s to 0.7 s.
Consequently these numbers are NOT comparable to points 7 and 8, which required
the 8-starved/8-healthy split; host contention here is uniform (all 16 vCPUs
~995 capacity).

## 11.2 The decisive contrast: 40x less firing, no change

The 2 ms arm fires essentially nothing on five of six workloads. If the
threshold mattered, it would have to differ from the 100 us arm.

| workload | evictions/run 100 us -> 2 ms | perf | wait/acq |
|---|---|---|---|
| hackbench_pipe_thr | 1,090 -> 24 | +0.47% | +0.45% |
| ebizzy_mmap | 63 -> 1 | +0.04% | -5.45% |
| dbench_16 | 195 -> 5 | +0.99% | +0.89% |
| sysbench_mutex | 2 -> 0 | -0.36% | +20.65% |
| parsec_vips | 5 -> 0 | -4.85% | +8.29% |
| **pooled (n=5)** | **271 -> 6** | **-0.74% (t=-0.70)** | **+4.97% (t=1.11)** |

A ~40x reduction in firing rate is indistinguishable from the most aggressive
setting on both metrics.

## 11.3 Full sweep, pooled

`parsec_dedup` is EXCLUDED: its PV arm alone ranges 15.2-194.1 s, CV 82%, and
individual runs span 15.2-231.5 s. At that variance it carries no information
and its apparent -44% to -61% on every IVH arm is noise, not an effect. The
other five workloads have PV-arm CV between 0.1% and 4.7%.

| arm | perf vs PV | t | wait/acq vs PV | t | evictions/run |
|---|---|---|---|---|---|
| 100 us | +1.41% | 1.08 | +5.21% | 0.76 | 271 |
| 250 us | +0.99% | 1.73 | +3.01% | 0.79 | 193 |
| 500 us (shipped) | +1.11% | 1.23 | +10.38% | 1.16 | 173 |
| 1 ms | +1.25% | 1.21 | +12.44% | 1.55 | 133 |
| 2 ms | +0.61% | 2.08 | +10.87% | 1.10 | 6 |

No arm clears |t| >= 2.57 on either metric. There is no monotonic trend in
either column, and wait time does not fall at any threshold -- the pooled
wait/acq change is positive (worse) at all five, though never significantly so.

**Wait time is NOT won.** The sign convention traps readers, so explicitly:
positive = IVH waits MORE than PV = worse. Per workload, wait/acq vs PV:

| workload | 100 us | 250 us | 500 us | 1 ms | 2 ms | arms reducing wait |
|---|---|---|---|---|---|---|
| hackbench_pipe_thr | +3.35% | +2.79% | +3.93% | +4.00% | +3.82% | 0/5 |
| ebizzy_mmap | -4.71% | -4.88% | -8.01% | -2.19% | -9.90% | 5/5 |
| dbench_16 | -6.93% | -5.37% | +0.11% | -3.42% | -6.11% | 4/5 |
| sysbench_mutex | +2.96% | +7.91% | +11.93% | +31.85% | +24.23% | 0/5 |
| parsec_vips | +31.39% | +14.60% | +43.94% | +31.94% | +42.28% | 0/5 |

Only ebizzy and dbench improve; three of five workloads are worse at every
threshold. Ranked on pooled wait, 1 ms is the WORST arm (+12.44%), not the best.

**And the improvement that does exist is not skipping.** Correlating eviction
count against wait change across all 25 (workload, arm) cells gives r = -0.185
(t = -0.90, n = 25), not significant. If skipping bought the reduction, more
evictions would mean more wait saved. On the two workloads that do improve, the
LARGEST reductions come from the 2 ms arm firing 1 and 5 evictions per run --
effectively the no-skipping arm. The intuition that fewer evictions means less
disruption does not terminate at 1 ms; it terminates at zero.

**The ~+1% throughput common to every arm is not eviction.** It appears
undiminished at the 2 ms arm, which fires 6 evictions per run. It is
attributable to **tier 1 alone** -- tier 2 and head bypass were enabled but
fired ~0 and 0 respectively at the shipped 5 ms `beat_threshold`, as the
correction in 11.1 sets out. This does NOT corroborate the recorded
t12+bypass result; that configuration was not running here.

## 11.4 Why: the firing rate

Evictions per million queued acquisitions, at the most aggressive threshold:

| workload | evictions | queued acquisitions | per million |
|---|---|---|---|
| dbench_16 | 195 | 92,891 | 2,097 |
| ebizzy_mmap | 63 | 155,159 | 407 |
| hackbench_pipe_thr | 1,090 | 3,228,271 | 338 |
| parsec_vips | 5 | 40,079 | 115 |
| parsec_dedup | 3 | 46,409 | 69 |
| sysbench_mutex | 2 | 113,217 | 19 |

Even on the most favourable workload, at the most aggressive setting, 0.21% of
queued acquisitions see an eviction. On four of six it is under 0.04%. A
mechanism touching that fraction of acquisitions cannot move an aggregate, and
no choice of threshold changes that -- lowering the cut raises firing only from
~0.02% to ~0.2%, and it raises `lookahead_refused` faster than it raises useful
evictions (hackbench 2,162 refusals against 1,090 marks at 100 us, versus 6
against 24 at 2 ms), i.e. the extra candidates found at an aggressive cut are
disproportionately waiters with nothing live behind them.

## 11.5 What can be claimed

The shipped 500 us is defensible only as *no worse than any alternative*. It is
not an optimum, and the sweep cannot be used to argue that it is. The honest
claim for the paper is the firing-rate one: the staleness threshold is
unfalsifiable on real workloads because the predicate fires 19-2,097 times per
million queued acquisitions, so no setting of it is distinguishable from any
other.

## 11.6 Limits

1. **Uniform host contention only.** The preempted-waiter population is set by
   the host's preemption pattern; a host starving a subset of vCPUs harder would
   raise the firing rate and could in principle make the threshold bite. Not
   tested here, and points 7/8's split contention is the obvious next condition.
2. **`parsec_dedup` excluded** on variance grounds (11.3), so the pooled figures
   are n=5, not n=6.
3. **Migration off**, so this is the isolated spin-side result. The combined
   configuration is point 12.
4. **`ivh_evict_halt_averted` read 0 throughout** and is not used above; whether
   that counter is wired is unverified.
5. Wait is reported per acquisition as well as in total because total wait is
   only directly comparable on fixed-work benchmarks; on the time-boxed ones
   (ebizzy, dbench) a faster arm accrues more total wait at lower per-acquisition
   wait. Where the two disagree, per-acquisition is the correct reading.

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

**Purpose.** Rank the suite by how hard it exercises kernel spinlocks, so the
parameter-sensitivity points (7, 8, 11) run on 6 workloads spanning that range
instead of 16 through every arm.

**Metric: lock ACQUISITIONS per second** -- see 15.2 for why, and for the two
instruments that were tried first and are wrong for this point.

**Instrument.** ftrace function profiler (`ivh_tools/lockrate.sh`), counting
entries to `_raw_spin_lock{,_irqsave,_irq,_bh,_nested,...}`,
`_raw_spin_trylock{,_bh}`, the rwlock variants, and both queued-spinlock
slowpaths. No PMU (none is virtualised on this TDX guest), no BPF. fentry on
`_raw_spin_lock*` was rejected: bpftrace flags it a "dangerous function" that
risks kernel deadlock and drops events under its own mitigation, and a lossy
counter cannot rank anything.

**Procedure.** 3 reps per workload, `drop_caches` before each, medians reported
with the max/min spread so instability is visible rather than hidden. The idle
background (54,071/s, same instrument) is measured and subtracted. PARSEC
packages are run directly from their `run/` directory, never through
`parsecmgmt` (15.2).

**Arm.** Any -- the profiler is independent of IVH state. `ivh_prelock_calls`
would NOT be: it sits behind the `ivh_universal_eligible` bail and reads zero in
the PV arm.

**Two failures worth not repeating.** (1) `/root/spin_mode` clears
`ivh_cs_owner_enable`/`ivh_cs_owner_clear`, so any CS-histogram reading must be
re-armed after every arm switch. (2) At the scaled size `fsmark_tmpfs` writes
1,875 MB per run; without a `rm -rf` in the command it filled a 7.4 GB tmpfs
after 4 runs and every later run exited in ~0.15 s having written nothing --
visible only as a 289x spread, not as an error.

## 15.1 Lock ACQUISITIONS per second

Median of 3 reps, ftrace function profiler, background subtracted. `slow%` is
the share of acquisitions that reached a queued-spinlock slowpath, i.e. that
were contended.

| workload | acq/s | net acq/s | slow% | spread | tier |
|---|---|---|---|---|---|
| `stressng_dentry` | 8,657,248 | 8,603,177 | 5.10% | 1.02x | HIGH |
| `hackbench_pipe_thr` | 7,988,479 | 7,934,408 | 6.79% | 1.01x | HIGH |
| `perf_epoll_wait` | 3,473,030 | 3,418,960 | 0.63% | 1.02x | HIGH |
| `fsmark_tmpfs` | 3,418,085 | 3,364,015 | 1.65% | 1.12x | HIGH |
| `ebizzy_mmap` | 2,963,979 | 2,909,909 | 0.62% | 1.16x | HIGH |
| `nhextend_full` | 1,338,259 | 1,284,188 | 1.47% | 1.01x | MID |
| `parsec_dedup` | 1,048,012 | 993,941 | 1.21% | 1.26x | MID |
| `dbench_16` | 1,010,623 | 956,552 | 0.76% | 1.00x | MID |
| `wis_mmap2` | 339,770 | 285,699 | 1.30% | 1.21x | LOW |
| `parsec_vips` | 219,124 | 165,053 | 0.57% | 1.03x | LOW |
| `sysbench_mutex` | 169,349 | 115,279 | **10.72%** | 1.09x | LOW |
| `parsec_bodytrack` | 128,341 | 74,270 | 1.17% | 1.18x | LOW † |
| `schbench` | 121,828 | 67,758 | 0.22% | 1.05x | LOW † |
| `parsec_blackscholes` | 103,644 | 49,574 | 0.77% | 1.63x | LOW † |
| `parsec_ferret` | 65,957 | 11,886 | 0.08% | 1.03x | LOW † |
| `parsec_swaptions` | 55,293 | 1,223 | 0.14% | 1.01x | LOW † |

Idle background **54,071/s**, measured with the same instrument and subtracted.
**†** = net rate within 2x of background, so the placement is weak;
`parsec_swaptions` is indistinguishable from an idle box.

**`slow%` is the more discriminating column than the rate.** `sysbench_mutex`
contends on **10.72%** of its acquisitions while `parsec_ferret` contends on
**0.08%** -- a 134x difference in the *character* of the locking at similar
absolute rates. Only `stressng_dentry` and `hackbench_pipe_thr` combine a high
rate with high contention.

## 15.2 Why this is acquisitions and not contentions

IVH acts at **acquisition**: `ivh_pre_lock()` is called from `_raw_spin_lock*`
(`kernel/locking/spinlock.c:308,327,345`) on every acquisition. Contention is a
different and much smaller population -- `lock:contention_begin` fires only in
`queued_spin_lock_slowpath` (`qspinlock.c:334`).

Three instruments were tried; the first two are wrong for this point and are
recorded so the numbers are not re-quoted:

| instrument | dentry | dedup | schbench | why it is wrong here |
|---|---|---|---|---|
| ~~`lock:contention_begin`~~ | 572,465 | 1,035 | 248 | contended only -- 0.2-11% of acquisitions. Ranked `parsec_dedup` *below the 64/s idle floor* while it is a top-3 IVH win (+86.86%) |
| ~~`ivh_prelock_calls`~~ | 6,269,192 | 272,228 | 61,516 | only acquisitions ELIGIBLE for IVH: five bails in `ivh_pre_lock()` including `!rcu_preempt_depth()`, and the comment there says "an enormous share of `spin_lock()` callers (dcache, lockref, net, slab)" hold an RCU reader. Reads zero in the PV arm by construction |
| **ftrace function profiler** | 8,657,248 | 1,048,012 | 121,828 | every `_raw_spin_lock*` / rwlock / slowpath entry. **Used above** |

> ~~"PARSEC cannot be placed on this axis -- the instrument is blind to
> userspace synchronisation"~~ **WITHDRAWN.** PARSEC acquires kernel spinlocks
> constantly (page faults, mmap, file I/O, thread creation); it just does not
> *contend*. `parsec_dedup` is 1,048,012 acquisitions/s at 1.21% contended. The
> axis was wrong, not the workloads.

**PARSEC must be run WITHOUT `parsecmgmt`.** That harness is a shell wrapper
which itself acquires ~600,000 spinlocks/s: `parsecmgmt -a status`, doing no
application work at all, measured **599,374/s**. Tracing `parsecmgmt -a run`
therefore measures the wrapper, and all six packages came out within 1.6% of
each other (807k-826k/s) regardless of application. `parsec_blackscholes` read
825,500/s through the harness and **103,644/s** run directly -- 87% harness.
Section 15.1 runs each package's `native.runconf` `run_exec`/`run_args`
directly from its `run/` directory.

**Instrument caveat:** the profiler costs time per call, so a fixed-duration
workload does less work while traced. Counts are exact; absolute throughput
under tracing is not comparable to an untraced run. It is also system-wide with
no PID filter, hence the background subtraction.

## 15.3 Stratified subset for points 7, 8 and 11

Chosen for tier spread AND for demonstrated Gate-2 sensitivity (the
worst/best throughput ratio across thresholds, `campaign/point7_0925_225925.csv`),
because the two axes do not agree: `stressng_dentry` and `dbench_16` are the
cleanest lock-rate measurements in the set but are **flat** on Gate 2
(1.04x, 1.05x), so choosing on rate alone bakes in a null result.

| tier | workload | net acq/s | Gate-2 ratio |
|---|---|---|---|
| HIGH | `hackbench_pipe_thr` | 7,934,408 | **1.48x** |
| HIGH | `ebizzy_mmap` | 2,909,909 | **1.49x** |
| MID | `dbench_16` | 956,552 | 1.05x -- **negative control** |
| MID | `parsec_dedup` | 993,941 | **3.80x** (largest) |
| LOW | `sysbench_mutex` | 115,279 | not measured; **+19.8% confirmed** (5/5, t=21.23) |
| LOW | `parsec_vips` | 165,053 | **1.34x** |

Span 48x. Four have demonstrated Gate-2 sensitivity; `dbench_16` is included
deliberately as a **negative control** -- it is flat (1.05x) with the cleanest
measurement in the suite (spread 1.00x), so if the other five respond and it
does not, a systematic artifact affecting all arms is ruled out. `sysbench_mutex`
has no Gate-2 sensitivity datum but is a confirmed win at the scaled size
(+19.8%, 5/5 pairs, t=21.23) and carries the highest contended share in the
suite (10.72%, 8x the next).

`wis_mmap2` was in this slot and was dropped on 2026-09-28: a single-arm probe
gave **65 migrations and -0.0%** against its recorded +11.2%, so migration
barely fires there and no threshold could modulate it.

Excluded: `nhextend_full` (reserved for the AFL/adaptive-spinning work, not a
migration workload), `stressng_dentry` (flat, and would duplicate HIGH),
`perf_sched_pipe` (bimodal, spread 5.64x over 9 reps), `fsmark_tmpfs` and
`perf_epoll_wait` (would duplicate HIGH), and the five workloads within 2x of
background.

## 15.4 Lock rate does not predict the benefit

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

## 15.5 Limits of this measurement

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


## 15.6 Watch list -- re-check after parameter tuning

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

## 15.7 Harnesses

| file | role |
|---|---|
| `ivh_tools/point15_lockrate.sh` | main pass, 20 workloads x 3 reps |
| `ivh_tools/point15_rerun_anomalies.sh` | +5 reps for UNSTABLE / NEAR-IDLE / SHORT |
| `ivh_tools/point15_report.py` | report, splits the two groups |
| `ivh_tools/ivh_benchmarks.sh` | workload registry with recorded deltas |
| `ivh_tools/point15_0927-220345.csv` | raw data (`_rerun.csv` for the re-runs) |

---

# Appendix A: the exact sysctl state behind the ladder verdict (2026-09-29)

The 2026-09-29 additive-ladder run is what the decision "adaptive spinning ships
as skip + hb + heh + t2 on top of mig+t1" rests on. Until now that
configuration existed only in `ivh_tools/ladder.sh`; this appendix records it as
kernel state. Every value below was produced by APPLYING each arm with
`ivh_tools/ladder_arm.sh` -- which sources `setarm()` extracted verbatim from
`ladder.sh`, so the two cannot drift -- and then dumping all 88
`/proc/sys/kernel/ivh_*` entries.

Reproduce any arm with:

```
bash /root/ivh_tools/ladder_arm.sh mig_t1_heh_t2_sk_hb   # arm 5, the shipped stack
bash /root/ivh_tools/pvbase.sh                            # the stock-PV baseline
```

## A.0 WARNING: `spin_mode 1` alone does not give you stock PV

`ivh_head_bypass_probe` is read inside `pv_wait_node()`
(`qspinlock_paravirt.h:2010`) with **no `ivh_adaptive_mode` gate**. A leftover
`probe=1` from a previous bypass arm therefore keeps firing at
`ivh_adaptive_mode=0`. Measured 2026-09-29 on one hackbench run:

```
spin_mode 1, bypass sysctls left on   ->  ivh_head_bypass_fired delta = 13
spin_mode 1, bypass sysctls zeroed    ->  ivh_head_bypass_fired delta =  0
```

13 fires is small, but a baseline that runs any IVH code is not a baseline.
`ivh_tools/pvbase.sh` zeroes all 17 feature sysctls by hand and asserts eight of
them; use it, never bare `spin_mode 1`.

## A.1 The sysctls that differ across the six states

| sysctl | stock PV | 1 mig+t1 | 2 +HEH | 3 +t2 | 4 +skip | 5 +hb SHIPPED |
|---|---|---|---|---|---|---|
| `ivh_adaptive_mode` | 0 | 2 | 2 | 2 | 2 | 2 |
| `ivh_cs_criterion` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_head_bail` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_head_probe` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_owner_clear` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_owner_enable` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_scan` | 0 | 0 | 1 | 1 | 1 | 1 |
| `ivh_cs_track_enabled` | 0 | 1 | 1 | 1 | 1 | 1 |
| `ivh_head_bypass_enable` | 0 | 0 | 0 | 0 | 0 | 1 |
| `ivh_head_bypass_probe` | 0 | 0 | 0 | 0 | 0 | 1 |
| `ivh_head_bypass_runs` | 0 | 1 | 1 | 1 | 1 | 1 |
| `ivh_pv_beat_threshold` | 2200000 | 11000000 | 11000000 | 2200000 | 2200000 | 2200000 |
| `ivh_pv_evict_enable` | 0 | 0 | 0 | 0 | 1 | 1 |
| `ivh_pv_evict_lookahead` | 0 | 1 | 1 | 1 | 1 | 1 |
| `ivh_pv_evict_node_stamp` | 0 | 1 | 1 | 1 | 1 | 1 |
| `ivh_pv_preempt_src` | 0 | 2 | 2 | 2 | 2 | 2 |
| `ivh_pv_requeue_nosteal` | 0 | 1 | 1 | 1 | 1 | 1 |
| `ivh_pv_tier2_enable` | 0 | 0 | 0 | 1 | 1 | 1 |
| `ivh_universal_eligible` | 0 | 1 | 1 | 1 | 1 | 1 |

Identical in all six states (69 sysctls):

```
ivh_adaptive_irqoff_bail_gate=0
ivh_cap_source=3
ivh_capacity_threshold=1010
ivh_cs_noise_cycles=550000
ivh_cs_owed_ticks=2
ivh_cs_owner_fast=0
ivh_cs_prompt_cycles=19800
ivh_cs_react_hist=0
ivh_cs_recall_hist=0
ivh_cs_tick_period=2200000
ivh_cs_verdict=0
ivh_eval_cooldown_ns=50000
ivh_head_bypass_hold=0
ivh_head_bypass_max=4
ivh_head_bypass_onexit=1
ivh_max_concurrent=8
ivh_migrate_mechanism=0
ivh_migration_timeout_ns=500000
ivh_preempt_event_source=2
ivh_pv_allow=1
ivh_pv_beat_publish_mask=4095
ivh_pv_camp_probe=0
ivh_pv_evict_age_hist=0
ivh_pv_evict_cheap_now=1
ivh_pv_evict_debug=0
ivh_pv_evict_gap_hist=0
ivh_pv_evict_hop_cap=2
ivh_pv_evict_promo_hist=0
ivh_pv_evict_quiet=0
ivh_pv_evict_threshold=1100000
ivh_pv_irqoff_halt=0
ivh_pv_requeue_max=4
ivh_pv_requeue_none=0
ivh_pv_rot_enable=0
ivh_pv_rot_probe=0
ivh_pv_rot_skip_max=4
ivh_pv_skip_point=0
ivh_pv_spin_threshold=32768
ivh_pv_tas=0
ivh_pv_tier1_confirm=0
ivh_pv_tier1_enable=1
ivh_pv_tier1_halt_min=0
ivh_pv_trylock_relaxed=0
ivh_pv_unhalt_avail=1
ivh_pv_unlock_reserve=0
ivh_pv_wait_trace=0
ivh_sched_timeout_ms=1
ivh_selection_trylock=1
ivh_skipcheck_hist=0
ivh_slowpath_wait_measure=1
ivh_steal_source=2
ivh_time_left_source=1
ivh_time_left_threshold_ns=4000000
ivh_tks_carry_ticks=8
ivh_tks_deadband_ns=1000
ivh_tks_duty_pct=100
ivh_tks_idle_sub=0
ivh_tks_on_ns=10000000
ivh_tks_phase_pct=0
ivh_tks_sampler_ns=0
ivh_uc_duty_ns=0
ivh_uc_ema_alpha_q16=868
ivh_uc_enabled=1
ivh_uc_min_avail_pct=10
ivh_uc_min_steal_ns=500000
ivh_uc_used_source=0
ivh_uc_window_ns=200000000
ivh_vact_jump_ns=1500000
ivh_vact_min_preempt_ns=2000
```

## A.2 Notes on the load-bearing values

- **`ivh_pv_beat_threshold`** is shared by tier 2 AND head bypass. At the shipped
  11,000,000 (5 ms) NEITHER fires. Arms 3-5 use **2,200,000 (1 ms)**, where
  bypass was validated at 99.7% taken and tier 2 fires ~64k/run.
- **`ivh_head_bypass_enable=1` is not sufficient** -- `ivh_head_bypass_probe=1`
  is also required, because the bypass code lives inside `ivh_head_observe()`
  whose only call site is gated on the probe.
- **`ivh_preempt_event_source=2`** is required or migration never fires; at 0,
  Gate 2 reads a dead paravirt field.
- **Migration is ON in every IVH arm** (`ivh_universal_eligible=1`,
  `ivh_pv_preempt_src=2`, `ivh_time_left_threshold_ns=4000000`,
  `ivh_max_concurrent=8`). It is the baseline mechanism, not a variable.
- **`spin_mode` must be called FIRST** in any arm setup: it sets
  `tier1_enable=1`, `tier2_enable=1` and forces `beat_threshold=11000000`, so
  every feature write must follow it.
- `ivh_pv_spin_threshold=32768` and `ivh_pv_tier1_enable=1` in all five IVH arms.
- Skip parameters (arms 4-5): `hop_cap=2`, `requeue_max=4`, `skip_point=0`,
  `evict_threshold=1100000`, `lookahead=1`, `nosteal=1`, `node_stamp=1`.

---

# Appendix B: Point 4 -- the migratable-thread census (2026-09-30)

Of the threads a workload spawns, how many can IVH act on at a lock acquisition,
how many cannot, and why. **No kernel build required.**

## B.1 Method

`ivh_pre_lock()` is `__always_inline` and absent from `/proc/kallsyms`, so it
cannot be probed. It is instead probed at its THREE call sites
(`kernel/locking/spinlock.c:308,327,345`) -- `_raw_spin_lock`,
`_raw_spin_lock_irqsave`, `_raw_spin_lock_irq` -- all of which call it BEFORE the
IRQ-disabling acquire, so none is structurally dead. `ivh_eval_cooldown_ok()` IS
a global symbol and is reached only after every static gate passes, so the tids
seen there are the provably migratable population. Tools: `ivh_tools/point4.bt`,
`point4.sh`, `point4_all.sh`.

Two traps, both hit and fixed:
* Linux truncates `comm` to 15 chars, so `memtier_benchmark` never matches --
  use `memtier_benchma`.
* The probe must ATTACH BEFORE the workload starts. kprobe setup takes seconds
  and `fsmark` completes in ~1.5 s under IVH, so an attach-after ordering
  measured nothing and looked like a genuine zero.

## B.2 Results

Full stack, `tlt=10ms`, `cap=8`, `ivh_rcu_guard=0`. Each row filtered to the
workload's own user threads (no kernel threads counted).

```
workload                 pass %  not running %   pinned %   acquisitions   evaluated
ebizzy_mmap               99.1%           0.5%       0.4%     16,929,315     578,201
hackbench_pipe_thr        98.9%           0.7%       0.4%     12,044,516     218,411
dbench_16                 87.8%           5.7%       6.5%      3,610,407     175,797
wis_mmap2                  0.2%           0.8%      99.0%      3,472,822         887
fsmark_tmpfs              98.4%           1.4%       0.2%      7,541,153     234,472
memtier_memcached         97.3%           1.2%       1.2%     27,173,501     519,897
nhextend_full             90.2%           9.0%       0.8%      7,520,986     336,129
parsec_vips               94.1%           1.9%       3.9%        592,344      87,980
parsec_dedup              98.5%           0.5%       1.0%      4,026,602     344,463
tinyconfig                81.1%           5.1%      12.4%        249,240      15,700
parsec_bodytrack          82.0%           9.9%       8.1%        621,832      49,390
parsec_canneal            99.5%           0.0%       0.5%         83,952      47,428
```

Remainder to 100% is nested acquisition (`lock_depth > 0`): tinyconfig 1.4%,
memtier 0.2%, others ~0. Columns are ACQUISITION counts, not thread counts.

## B.3 The two reject conditions

**`not running`** -- `current->__state != TASK_RUNNING`. The caller already did
`set_current_state(TASK_INTERRUPTIBLE)` as part of a prepare-to-wait sequence and
is taking this lock as bookkeeping before its own `schedule()`. Migrating there
would consume the sleep state and dequeue the task on a wakeup nobody will send
-- a lost wakeup. Skipping is safe; IVH retries on the next acquisition while
running. Tracks how much a workload blocks: nhextend 9.0%, bodytrack 9.9% at the
top, ebizzy 0.5% at the bottom.

**`pinned`** -- `cpumask_weight(current->cpus_ptr) <= 1` (`fair.c:13938`, checked
AFTER the cooldown). Three causes:
1. DELIBERATE affinity. `will-it-scale`'s `*_threads` harness pins each worker to
   its own CPU -- verified directly: worker tids read `Cpus_allowed_list` 0,1,2,...
   while only the main thread reads 0-15. That is the entire 99.0%.
2. SELF-INFLICTED. Migration mechanism 0 sets `cpus_mask = {target_cpu}` for the
   duration of a move, so any lock taken inside that window reads as pinned.
   That is the 0.2-1.2% on everything else -- IVH seeing its own migrations.
3. SHORT-LIVED FORKS. tinyconfig's 12.4%: a kernel build forks hundreds of
   processes frequently mid-migration or mid-exit when they take locks.

## B.4 What the census actually shows

1. **Thread eligibility is almost never the constraint.** Eleven of twelve
   workloads have 81-99.5% of acquisitions eligible.
2. **The cooldown is the dominant filter, and it is not a gate.** ebizzy passes
   99.1% of acquisitions and evaluates 3.8% of them; `ivh_eval_cooldown_ns=50000`
   discards the other 96%. Funnel: 16,929,315 acquisitions -> 15,060,621 gated ->
   578,201 evaluated -> ~9,870 migrations.
3. **`wis_mmap2` is structurally unmigratable** -- 887 evaluations out of 3.47M
   acquisitions -- which is why it produced ~480 migrations regardless of any
   parameter in points 7 and 8. Yet an A/B (n=5) attributes its gain to migration
   anyway: pv 253,405 / adaptive-spinning-only 261,383 (+3.15%) / full 302,165
   (+19.24%). ~480 migrations carry +16pp. NOT EXPLAINED; do not cite wis as
   evidence for coverage-driven benefit until it is.
4. **Kernel threads are not excluded.** `ivh_pre_lock()` never tests
   `PF_KTHREAD`. A system-wide probe (no comm filter) during an nhextend run:
   73,182 kernel-thread acquisitions passed every gate and 2,394 reached the
   decision, dominated by `rcu_preempt` (51,925) and `migration/N` (~18,400) --
   IVH evaluating whether to migrate the migration machinery. Per-CPU kthreads
   are rejected only incidentally, by the pinned gate.
5. **nhextend's gate entries are not its lock.** `ivh_adaptive_futex_lock.h` is a
   userspace spin-then-futex lock whose fast path never enters the kernel. Its
   stacks are ~611,000 `open()` and ~611,000 `cpu_clock_sample()` per 8 s run;
   futex appears at a count of 1. The lock under test is invisible to IVH.
