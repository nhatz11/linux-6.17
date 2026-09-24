# IVH evaluation

Two halves, measured 2026-09-22/23 and validated against host-side ground truth:

- **Part I (sections 0-9)** -- does the guest measure steal and active time
  correctly? Needed because the estimator drives the migration gate, and because
  of the standing objection that IVH uses tuned constants rather than
  measurements.
- **Part II (section 10)** -- does IVH make real workloads faster, and under
  what conditions?

---

# Part I: measuring steal and active time on a TDX guest, validated against the host

Date: 2026-09-23
Kernel: `6.17.0-G-LOCK-39-sampler+`, branch `ivh-rebuild-main`
Guest: 16 vCPU TDX confidential VM, `tsc_khz = 2200000`, `CONFIG_HZ=1000`, `nohz=off`
Host: `mars`, 3 × 16-vCPU guests (48 vCPUs total); this guest is pid 1430099
Ground truth: host-side `/proc/<vcpu-tid>/schedstat` — `run_ns` = active, `wait_ns` = steal

---

## 0. Why this work exists

To answer one objection: **that IVH relies on magic numbers that do not truly
measure steal time and active time.** The bar is therefore not "the number
looks plausible" but "the number is a measurement, with an error characterised
against an independent source."

That bar is what condemned the previous estimator, even though its numbers
looked fine.

---

## 1. The old estimator was a scaled event counter, and it aliased

`ivh_tick_steal_accumulate()` (`kernel/sched/core.c`) infers steal from the raw
TSC gap between consecutive scheduler ticks. Driven from the 1 kHz tick it is a
1 kHz sampler. Two findings killed it:

**1.1 ~90% of its output was a tuned constant.** Sweeping `ivh_tks_phase_pct`
past its documented maximum gives a near-perfect straight line:

    ratio = 0.0825 + 0.007168 x phase_pct      R^2 = 0.998

At the shipped `phase_pct=100`, the intercept (0.082) is everything the actual
gap arithmetic contributes; the slope term (0.717) is the phase bonus — a flat
one-tick credit per detected event. The genuine measurement was ~8% of the
reported value. This is the objection in its most literal form.

**1.2 It aliased.** Host preemption arrives at 457/s (cpu3) and 956/s (cpu15)
against a 1000/s sampler — at or past Nyquist. Replaying ONE fixed 10 s
recording while varying only the sampler's phase moves reported/true steal by
**2.16x on cpu3 and 10.67x on cpu15** (0.34 – 3.67), with the underlying
timeline byte-identical. Live, this showed as three consecutive measurements of
the same vCPU under the same load returning 1.31, 1.76, 1.20.

An estimator that answers differently each time you ask it under identical
conditions is not a measurement.

---

## 2. Tooling (all in-guest, no kernel change required)

Building this did NOT require patching the kernel to instrument itself — a
misstep that was caught and abandoned. The tick is a periodic timer on an
absolute grid, so given a gap timeline its delivery times are fully determined
and the estimator can be *computed* rather than measured.

- `ivh_tools/vcpu_trace.c` — SCHED_FIFO busy-spin prober recording every gap
  (~20 ns resolution) with its TSC timeline.
- `ivh_tools/replay_tks.py` — reproduces `ivh_tick_steal_accumulate()`'s exact
  arithmetic over a recording. **Agrees with the live kernel counter to within
  8–10%.** Lets a parameter sweep run offline against a FIXED timeline, so every
  cell sees identical host load instead of a fresh draw — which is what made
  earlier live sweeps unrankable.
- `ivh_tools/host_truth.sh` — host-side `schedstat` collector (read-only).
- `ivh_tools/guest_window.sh` — guest half of a paired window, no prober.
- `ivh_tools/validate_all.sh`, `verify39.sh` — campaign drivers.

---

## 3. What G-LOCK-39 changes

An optional per-CPU hrtimer drives the accumulator instead of the tick.

| sysctl | meaning |
|---|---|
| `ivh_tks_sampler_ns` | sampling period. 0 = tick-driven (default, bit-identical to G-LOCK-38). A preemption shorter than this period can fall entirely between two samples and be missed. |
| `ivh_tks_duty_pct` | fraction of wall time the sampler runs. <100 samples for `ivh_tks_on_ns` then sleeps, scaling the result back up. |
| `ivh_tks_on_ns` | length of one sampling burst when duty cycling. |

`ivh_tks_phase_pct` **must be 0** with the sampler: the bonus corrects for
undersampling and inflates a properly-sampled signal (1.49 vs 0.94 at 20 kHz).
The proc handler refuses a non-zero `sampler_ns` while `phase_pct` is non-zero,
because `goto_mode.sh` sets `phase_pct=100` on every boot and the machine's
default state would otherwise be the inflating one.

### 3.1 Bugs found in review, before boot

An adversarial review of the patch found no hang/panic path but five defects
that silently falsify the number:

1. **Duty scaling applied when no duty cycling occurred.** `on_ns=0` documents
   "disables duty cycling"; the callback honoured it but the accumulator still
   multiplied by 100/duty. Silent 20x over-report. Fixed by scaling from the
   MEASURED on/off TSC split, never the nominal `duty_pct`.
2. **On-window counted in nominal periods.** `hrtimer_forward_now()` skips
   missed deadlines, so under heavy preemption a window covered more wall time
   than configured while the scale factor stayed put — the over-report grew
   *with* the steal being measured. Fixed by the same measured-span change.
3. **`phase_pct` footgun** (above). Now refused.
4. **CPU hotplug.** `hrtimers_cpu_dying()` migrates pinned timers with no
   exemption; an offlined CPU's sampler would land on another CPU and drive
   *its* rq state forever, cancelling real steal to zero. Fixed with
   `container_of` + home-CPU guard and a `cpuhp` online hook.
5. **Unbounded `on_ns`** overflowed to an off-interval of years, freezing steal
   accounting. Bounded.

`carry` is now preserved across the duty gap rather than zeroed — it is signed
debt that cancels over-booked jitter, and dropping it each window edge was a
one-way upward bias.

---

## 4. Host-side validation

Paired 60 s windows: host `schedstat` deltas per vCPU thread against the guest's
`ivh_tks_steal_ns` and `ivh_uc_capacity`, no prober running (so nothing is
perturbed and idle vCPUs are covered — two limits the in-guest prober could not
escape).

Requires `sched_schedstats=1` on the host; it is 0 by default and the counters
stay frozen otherwise.

**Workload:** vCPUs 0–7 heavily contended, 8–15 lightly. The 8/8 split is a host
topology boundary, not the guest's pinning.

### 4.1 The configuration space

Ratios are kernel/host. "active" compares `ivh_uc_capacity` (= `used/avail`)
against the host's `run/(run+wait)` — both are "of the time I wanted the CPU,
what fraction did I get".

| config | contended steal | contended active | light steal | light active |
|---|---|---|---|---|
| 50us duty=50 | 1.159 (+7.4pp) | 1.385 (+13.7pp) | 0.974 (−0.2pp) | 1.024 (+2.2pp) |
| 50us duty=100 | **0.983** (−1.0pp) | 0.704 (−9.1pp) | 0.860 (−1.5pp) | 1.012 (+1.1pp) |
| 100us duty=100 | 0.936 (−3.2pp) | 0.816 (−6.5pp) | 0.120 (−7.5pp) | 1.095 (+8.7pp) |
| **200us duty=100** | **0.907** (−4.0pp) | **0.912** (−3.0pp) | 0.101 (−6.7pp) | 1.087 (+8.0pp) |

### 4.2 Two effects that constrain the choice

**Duty cycling is biased on any vCPU with idle time.** While sampling, the
hrtimer keeps the vCPU awake so it cannot idle, and a vCPU that cannot idle
accrues steal faster than one that can. Extrapolating that inflated rate across
the sleep windows over-reports in proportion to contention — the +16% at
duty=50, consistent to a spread of 0.014 across eight vCPUs. Removing the
extrapolation (duty=100) collapses it to 0.983.

**The instrument costs 11–15 us per sample.** This host has no
`tsc_deadline_timer`, so every hrtimer re-arm is a LAPIC MSR write, which in a
TD guest is a `#VE` → TDVMCALL exit. Measured two independent ways: a busy-spin
prober sees gaps at 11.2 us (cpu3) and 14.7 us (cpu15) mean, arriving at
~1300–1500/s against a 1000/s tick.

The consequence is that the sampler perturbs what it measures, and the host
sees it:

| config | host idle, vcpu 8–15 | host steal, vcpu 0–7 |
|---|---|---|
| 50us duty=50 | 23.9% | 46.6% |
| 50us duty=100 | **0.30%** | **63.3%** |
| 100us duty=100 | 3.4% | 50.0% |
| 200us duty=100 | 8.0% | 43.1% |

At 50 us continuous, nothing is ever allowed to halt: idle collapses to 0.3% and
genuine steal rises by a third. The estimator was accurate *about a machine the
measurement created*.

This also caps the achievable period. At a 50 us setting the sampler achieves
only 117 us; at 100 us it achieves 127 us. **The instrument's own cost floors the
period at roughly the same scale as the gaps to be resolved (~120 us), so there
is no achievable period that both resolves short preemptions and leaves idle
intact.** That is a measurement limit of this platform, not a tuning failure.

---

## 5. Shipped configuration and its error budget

    ivh_tks_sampler_ns = 200000     (200 us, continuous)
    ivh_tks_duty_pct   = 100        (no extrapolation)
    ivh_tks_phase_pct  = 0          (no tuned constant)
    ivh_tks_deadband_ns = 1000

Chosen because it is the only configuration with **every metric except
light-vCPU steal inside 9.3%**, and because errors on contended vCPUs are what
matter in absolute terms — the same percentage is milliseconds there and
microseconds on an idle vCPU.

**Contended vCPUs (the case that matters), per 60 s window:**

| quantity | host | kernel | error | ratio | absolute |
|---|---|---|---|---|---|
| steal | 43.1% | 39.1% | −4.0 pp | 0.907 | −2.4 s / 60 s |
| active | 34.4% | 31.4% | −3.0 pp | 0.912 | −1.8 s / 60 s |

Both **undershoot** by ~9%. For a capacity gate that is the safe direction: it
reports a contended vCPU as slightly healthier than it is, so it under-triggers
rather than thrashing.

It is also the cheapest sampler tested (5 k/s per vCPU vs 20 k/s), the least
perturbing (host steal 43.1%, lowest of the four), and the only one that
approximately keeps its schedule (225 us effective vs 200 us nominal).

**Known weakness:** light-vCPU steal reads 0.101 (0.75% against a host-measured
7.5%). The sampler is blind to preemptions shorter than its period, and those
vCPUs' quanta are ~120 us. In absolute terms this is 6.7 pp of a small number,
and it reports those vCPUs as healthy — which at 7.5% steal they essentially
are. The contended/idle differential still separates unambiguously
(38–40% vs 0.75%).

---

## 6. The remaining defect: active time is limited by guest idle accounting

Steal is measured well; **active is derived, and its divisor is wrong.**

`capacity = used/avail` with `used = avail − steal` and `avail = elapsed − idle`.
Working back from the 50 us/duty=100 window: capacity 21.58% with steal 62.21%
implies a kernel-side idle of ~20.7% of wall, where the host measured **9.09%**.
The guest roughly doubles idle, which shrinks `avail`, which inflates
`steal/avail`, which deflates capacity.

Independently visible: guest `/proc/stat` claimed ~47% idle where the host
measured 9%, making `wall − idle − steal` go **negative**. Guest-side active time
cannot be derived that way on this platform — the guest's tick-based idle
accounting absorbs stolen time as idle.

So the next target is not the steal path but `ivh_idle_ns()`. At the shipped
200 us setting active happens to land at −3.0 pp, but that is the divisor error
partly cancelling the sampler's undershoot, not two independently correct
quantities.

---

## 7. What can and cannot be claimed

**Can:**
- Reported steal is measured gap time, not a scaled event count. `phase_pct=0`
  is viable; the tuned constant is gone.
- Against hypervisor ground truth, contended vCPUs read 0.907 (steal) and 0.912
  (active) — both ~9% under, in the safe direction.
- Run-to-run spread collapsed from 1.20–1.76 (tick) to 0.90–1.07: the aliasing
  is gone, live, not just in replay.
- The contended/idle differential is preserved in every configuration tested.

**Cannot:**
- Cannot claim accuracy on lightly-loaded vCPUs with short-quantum preemption:
  0.101 at the shipped setting.
- Cannot claim active time is independently correct — §6.
- One host, one workload shape, one load level per configuration. The four
  configs were measured under different self-induced loads (host steal 43–63%),
  so cross-config comparison is confounded except via the ratios.
- The ~25% sampler overhead at 50 us is a platform artifact of the missing
  `tsc_deadline_timer`, not intrinsic to the method — but it has not been
  measured on hardware that has one.

---

## 8. Retractions from earlier in this investigation

- "No sysctl setting fixes the fine-quantum vCPU" — wrong; a range artifact.
  Every sweep had floored `deadband` at 10000 and the cliff is at 1000–2000 ns.
- "~90% of the signal is a fudge" — imprecise. `count x tick` is the principled
  estimator for that sampling process; the defect was aliasing, not the form.
- "Capacity got much worse under the sampler" — wrong reference. Compared to
  `true_active`, which capacity does not model; against `1 − true_steal` it
  tracked fine at duty ≥ 50.
- "The box isn't saturated" — it was, at 33.6% real steal. Capacity read 3%
  because of the `ivh_uc_tick` clamp. The instrument used to detect load was the
  thing under test.
- "5 kHz clears Nyquist so 200 us is safe" — conflated preemption *rate* with
  preemption *duration*. The period must also be shorter than the individual gap.
- The +8 pp contended residual was blamed on the prober under-counting sub-50 us
  preemption. Host data disproved it: the prober was right.

---

## 9. How this was produced (reproduction recipe)

Everything here is reproducible from two paired scripts. Nothing depends on a
number remembered from an earlier session.

**Ground truth (host, read-only).** `sched_schedstats` must be 1 on the host; it
defaults to 0 and the counters stay frozen otherwise:

    cat /proc/sys/kernel/sched_schedstats          # must print 1
    echo 1 | sudo tee /proc/sys/kernel/sched_schedstats

Identify this guest's QEMU process (its vCPU threads are named `CPU N/...`):

    for p in $(pgrep -f qemu-system); do
      echo "pid=$p vcpus=$(ps -L -o comm= -p $p | grep -c '^CPU') \
    $(ps -o args= -p $p | grep -o '\-name[= ][^ ]*' | head -1)"
    done

Then `ivh_tools/host_truth.sh <seconds> <pid>`, which samples
`/proc/<tid>/schedstat` per vCPU thread and prints `run_ns`/`wait_ns` deltas as
active/steal/idle percentages of wall.

**Guest half.** `ivh_tools/guest_window.sh <seconds>` over the SAME window —
started within a couple of seconds of the host script, which at 60 s is ~3%
skew. It samples `rq->ivh_tks_steal_ns` and `rq->ivh_uc_capacity` per vCPU via
`ivh_tools/read_vact_rq.py` (offsets verified against the live vmlinux with
`pahole -C rq --hex`). It runs **no prober**, so nothing is perturbed and idle
vCPUs are covered.

**Pairing rule.** Both halves must cover one window under one sampler setting.
Do NOT compare absolute numbers across settings: the sampler's own cost changes
the load (host steal ranged 43–63% across the four configs measured here), so
only the kernel/host RATIO within a window is meaningful.

**Aliasing evidence (§1.2)** comes from `ivh_tools/vcpu_trace.c` +
`replay_tks.py`: record one gap timeline, then replay the estimator's arithmetic
over it while varying only the sampler phase. Because the timeline is fixed,
any change in the output is the estimator, not the workload. The replay tracks
the live kernel counter to within 8–10%, which is what licenses using it.

**Sampler cost (§4.2)** was measured two independent ways: the gap histogram
from `vcpu_trace` (the ~1000/s population whose size matches the tick rate), and
a separate busy-spin prober on an idle vCPU. Both give 11–15 us per timer
interrupt.

**Configurations swept:** (50 us, duty 50), (50 us, duty 100), (100 us, duty
100), (200 us, duty 100), each as one paired 60 s window, plus a tick-driven
baseline. Achieved sampler rate was checked each time from `rq->ivh_tks_samples`
deltas, because the sampler overruns its nominal period on this hardware.

**Provenance caveat.** Every number in §4 and §5 is a single 60 s paired window
per configuration. The ratios are consistent across the eight vCPUs within each
population (e.g. contended steal spread 0.014 at duty=50), which is what gives
confidence — not repetition. Repeating each configuration n>=3 times, and
varying host load independently of the sampler, are both still to do.

---
---

# Part II: what IVH does to real workloads

## 10. Migration and adaptive spinning on real benchmarks

### 10.0 The finding that reframes the older results

`ivh_benchmark_search_2026-07-20.md` records the tinyconfig kernel build as a
migration **loser at -11.8%** (n=3). Re-run 2026-09-23 as 10 interleaved pairs
on an oversubscribed host it is **+7.16% FASTER, 10/10 pairs, t=+11.93**, with
26,872 migrations per build in the ON arm and 0 in the OFF arm.

**Host contention is a hidden variable in every migration verdict in the 2026-07
docs.** Migration exists to move a thread off a contended vCPU. On a quiet host
there is nothing to move away from, so the only measurable effect is the
cache/TLB locality cost -- which is exactly what -11.8% looks like. Both numbers
are correct under their own conditions.

The survey's rejection of tinyconfig as "below the measurement floor (~15s)" was
an argument about **n**, not about the workload: at n=10 pairs it resolves
cleanly, and on linux-6.14 (not 6.6) the build takes ~30s anyway.

### 10.1 Method

Host during these runs: `mars`, **three 16-vCPU guests on one box** (48 vCPUs),
host-measured steal 43-63% on the contended vCPUs. Contention shape 0-7 heavy,
8-15 light -- a host topology boundary, not guest pinning.

- Interleaved A/B, arm order alternated every pair, page cache dropped before
  every run.
- **Both arms `spin_mode 1` (STOCK_PV)**, so `ivh_universal_eligible` is the only
  variable. These are migration **alone**, on stock upstream PV spinning, with
  `ivh_pv_preempt_src=0`.
- `ivh_migrations_done` (global atomic_t, read via /proc/kcore with
  `ivh_tools/migcount.py`) recorded per run. **An A/B where the counter reads 0
  in both arms tested nothing** -- that check is what distinguishes a real null
  from a mechanism that never fired.

### 10.2 Migration alone (2026-09-23, G-LOCK-39, 6 pairs each)

| workload | delta | pairs | t | migrations/run |
|---|---|---|---|---|
| PARSEC dedup | **+77.55%** | 6/6 | 9.63 | 9 152 |
| PARSEC vips | **+55.56%** | 6/6 | 35.91 | 5 720 |
| PARSEC bodytrack | +28.53% | 6/6 | 91.48 | 28 597 |
| PARSEC canneal | +12.55% | 6/6 | 11.43 | 23 418 |
| PARSEC swaptions | +10.93% | 6/6 | 4.75 | 439 |
| PARSEC blackscholes | +10.33% | 6/6 | 3.05 | 273 |
| PARSEC ferret | +8.26% | 6/6 | 4.01 | 2 029 |
| kernel build (tinyconfig -j16) | +7.16% | 10/10 | 11.93 | 26 872 |
| psearchy (MOSBench pedsort) | +5.24% | 8/8 | 3.63 | 324 |
| PARSEC freqmine | +2.03% | 5/6 | 1.50 | 746 | -- **not significant** |

**Provisional, measured only on the uncorrected harness (see 10.5):** PARSEC
streamcluster +35.84%, facesim +23.86%, fluidanimate +4.60%. Re-run before
quoting.

raytrace excluded (needs a display; this is a headless CVM). x264 not built.

### 10.3 dedup and vips: recorded losses that reversed

2026-07 recorded dedup at "+16-33% slower" and vips as "a clear loss". Both are
now large wins. Because the magnitudes are implausible for a scheduling change,
dedup was verified directly:

    migration off   142.99s   output 638M   md5 6f9eb502b0aab026   0 error lines
    migration on     11.74s   output 638M   md5 6f9eb502b0aab026   0 error lines

Byte-identical output, same input, **12x faster**. It is doing the same work.

**Mechanism (hypothesis; fits the data, not yet proven).** dedup's ON arm is
steady at 8.0-11.6s while OFF swings 17.2-169.2s -- a 10x spread. That variance
signature is a bounded-queue pipeline stalling when a partner thread is
descheduled, i.e. lock-holder preemption, which is what IVH exists to fix. On a
quiet host there are no stalls to prevent and only migration's cache cost is
visible, so **the same workload is IVH's worst case or its best case purely as a
function of host load.** vips by contrast is stable in BOTH arms (off 21.1-27.7s,
on 10.2-11.0s), so its win has a different shape and is not explained by this.

**The decisive test has not been run:** stop the co-tenant load and re-run dedup.
If +77% collapses toward the 2026-07 loss, the mechanism is demonstrated rather
than merely correlated.

### 10.4 Migration + adaptive spinning (2026-09-15 campaign, for comparison)

From `ivh_benchmark_campaign_2026-09-15.md`. **Different mechanism** -- that
campaign's IVH arm is `universal_eligible=1` **and** `spin_mode 2` (tier1+tier2,
`preempt_src=2`) -- and a different kernel (G-LOCK-30), half contention on
vCPUs 0-7, 8 blocks each. Not directly comparable to 10.2; listed because it is
the other half of the evidence.

Improved (best variant per family): fs_mark tmpfs **+167%**, perf `sched pipe`
**+147%**, ebizzy mmap **+104%**, stress-ng `dentry` **+100%**, hackbench
pipe-threads **+76%**, perf `epoll wait` +54%, dbench 16c +19%, will-it-scale
`mmap1` +10.9%, schbench +6.9%. Full list in that document (16 improved).

**The variant rule matters:** for ebizzy, dbench and hackbench the *other*
variants are recorded losses -- ebizzy malloc -2%, dbench tmpfs -19%, hackbench
-g20 -12%. Same binary, opposite sign, consistent with the collateral-cost
model: the winning variant blocks (on `mmap_lock`, on fsync, on pipes), the
losing one does not.

That campaign also found **17 regressions**, chiefly 13 saturated will-it-scale
microbenchmarks at -6 to -16% (16 threads, one syscall in a tight loop, no idle
destination to migrate to), plus netperf TCP_RR -44% and iperf3 -34%, which it
marks as fixable with a migration-eligibility gate excluding tight communication
pairs.

### 10.5 A harness flaw, found and corrected

The first PARSEC harness ran every pair as `for a in off on` and never dropped
the page cache, so **the ON arm always ran second against a cache warmed by the
OFF run**. Corrected by alternating arm order per pair and dropping caches before
every run. Effect of the correction:

| workload | flawed | corrected |
|---|---|---|
| dedup | +88.47% | +77.55% |
| vips | +58.44% | +55.56% |
| bodytrack | +27.31% | +28.53% |
| canneal | +16.51% | +12.55% |
| swaptions | +11.41% | +10.93% |
| blackscholes | +15.39% | +10.33% |
| ferret | +12.40% | +8.26% |
| **freqmine** | +10.46% | **+2.03% (ns)** -- collapsed |

Seven of eight survived; freqmine was the artifact. The psearchy harness was
never affected (it drops caches before every run); the tinyconfig harness shares
the fixed ordering but is preceded by a warmup and re-reads the same small source
tree each time.

### 10.6 Pooled averages

Across the **27 improved workloads** in 10.2 and 10.4 combined:

| set | n | arithmetic | median | geometric |
|---|---|---|---|---|
| pooled, both mechanisms | 27 | +43.4% | +20.0% | **+37.8%** |
| migration + adaptive spinning | 18 | +53.1% | +33.5% | +46.5% |
| migration alone | 9 | +24.0% | +10.9% | +22.0% |
| best-variant-only, pooled | 17 | +49.8% | +19.0% | +42.2% |

**Quote the geometric mean or the median, not the arithmetic mean.** These are
speedup ratios: +100% and -50% are the same factor inverted, so an arithmetic
mean over-weights the four triple-digit entries. Geometric mean of the ratios is
the defensible statistic.

**Label it precisely.** This is the average across workloads that *improve*, not
the average effect of IVH; including the regressed and neutral sets the full pool
is roughly +11% geometric over ~70 entries. The scoping is defensible -- the
regressed set is dominated by saturated microbenchmarks -- but the two numbers
read very differently and must not be confused.

**Adaptive spinning appears to roughly double the effect** (46.5% vs 22.0%
geometric), but the two sets are different benchmarks on different kernels, so
that is suggestive, not measured. Running the 10.4 workloads under
migration-alone would make it a real comparison.

### 10.7 What can and cannot be claimed

**Can:**
- Under host oversubscription, migration alone improves 9 of 10 workloads tested,
  8 of them significantly, with the mechanism confirmed firing in every ON run
  and never in an OFF run.
- Two workloads recorded as losses in 2026-07 (dedup, vips) are large wins under
  contention; dedup is verified to produce byte-identical output 12x faster.
- Host contention is a confound in the 2026-07 verdicts and they should be
  re-measured before being cited.

**Cannot:**
- Cannot claim a load-independent benefit. One host, one contention level,
  one workload shape per configuration.
- Cannot claim the lock-holder-preemption mechanism for dedup -- the
  quiet-host control has not been run.
- Cannot compare 10.2 against 10.4 directly: different mechanism, kernel and
  contention level.
- streamcluster, facesim and fluidanimate are uncorrected-harness only.
- These runs are not bit-identical to 2026-07: that used
  `ivh_selection_trylock=0`, current setup uses `1` (changed 2026-08-09 on
  measurement).

### 10.8 Reproduction

Harnesses in `ivh_tools/`: `tinyconfig_ab.sh`, `psearchy_ab.sh`, `parsec_ab.sh`,
`parsec_redo.sh`, `overnight_parsec.sh`, `migcount.py`. Per-run CSVs alongside.

- **Kernel build:** `/root/kernels/linux-6.14-stock`, built out-of-tree with
  `O=`. The tree must stay pristine -- the docs record a session destroying a
  `.config` in-tree and losing three files unrecoverably.
- **psearchy:** `/root/mosbench/psearchy`, `pedsort -t <dbprefix> -c 16 -m 512 <
  files_6x` (~36s, ~86k documents). The metric is pedsort's own
  `throughput: N jobs/hour/core`, higher better. `files_6x` is the supplied
  document list repeated 6x; 8x aborts on a hard `#define NFILES 100000`.
- **PARSEC:** `/root/parsec-benchmark`, `parsecmgmt -a run -c gcc -i native -n 16`.
- Building psearchy or PARSEC from a fresh git checkout hits three 15-year-old
  portability breaks: `gettid()` collides with glibc >= 2.30; `mkprimes` is
  python2 and silently emits an EMPTY `primes.C` (symptom is an undefined
  reference to `primes`/`nprimes`); and git checkouts lose the +x bit on
  `configure` scripts, which parsecmgmt reports as the misleading "Need
  'configure' script or a Makefile".
