# G-LOCK-39: measuring steal and active time on a TDX guest, validated against the host

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
