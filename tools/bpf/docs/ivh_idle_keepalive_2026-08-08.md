# IVH idle keepalive: keeping `ivh_uc_capacity` measured on idle vCPUs

**Date:** 2026-08-08
**Kernel:** 6.17.0-rseqport70-stealfix+ (branch `kernel-43-clean`), 16-vCPU KVM guest, HZ=1000, `CONFIG_NO_HZ_FULL=y`, no cpuidle driver (bare `HLT`)
**Status of the code:** compiles clean; **has never executed.** See sec 7.
**Prior work this session:** `6c3874293` (ivh_ref deadband + Q4 early exit, committed), and the `ivh_steal_source=2` tick-gap detector (uncommitted, `ivh_cvm_steal_detector_2026-08-08.md`). This work is additive to both and conflicts with neither.

---

## 1. Summary

`rq->ivh_uc_capacity` on an idle vCPU does not merely go stale, it stops being
published entirely. Measured on this guest at idle: **cpu2 published zero
windows in 60 seconds** against a nominal 300, holding a frozen capacity of 951
while the vCPUs that were still ticking read 1019–1021.

The fix is a per-CPU, off-by-default keepalive (`ivh_ka_*`) that briefly and
periodically makes an idle vCPU genuinely demand CPU, so the existing
`ivh_uc_tick()` → window → EMA machinery keeps running on its normal path.

**The single most important finding is that the framing this task started from
is wrong**, and it is wrong in a way that would have produced a mechanism that
costs CPU and publishes nothing. The task specified that the keepalive's job is
"just to force `account_process_tick()` to run … the vCPU doesn't need to stay
busy in a work sense." It does. Sec 2.2 and sec 5.2 establish this analytically
and then measure it across a 40× range.

Recommendation: `ivh_ka_interval_ns=100000000` (100 ms), `ivh_ka_probe_ns=2000000`
(2 ms), and `ivh_uc_min_avail_pct` lowered from 10 to 1. Measured result: full
nominal publish cadence (0.20 s/publish, from *never*), an unbiased capacity
reading, for **1.98% of one idle vCPU** and **+20 HLT exits/sec**. Sec 6.

---

## 2. The problem, verified rather than assumed

### 2.1 Mechanism 1 — proven: idle vCPUs stop calling `ivh_uc_tick()`

`ivh_uc_tick()` has exactly one caller, `account_process_tick()`
(`kernel/sched/cputime.c:624`). I verified the claim carried in the existing
comment at `core.c:1769` by reading the code rather than trusting it:
`account_idle_ticks()` (`cputime.c:654`) is a self-contained function that calls
`irqtime_account_idle_ticks()`, `steal_account_process_time()` and
`account_idle_time()`, and **never calls `account_process_tick()`**. Confirmed.
Under NOHZ, ticks skipped during idle are retroactively accounted through that
path, and `ivh_uc_tick()` is not on it.

`ivh_uc_close()` (`core.c:2473`) is reachable only from `ivh_uc_maybe_close_window()`,
which is reachable only from `ivh_uc_tick()`. No ticks ⇒ no window close ⇒
`rq->ivh_uc_capacity` holds its last value indefinitely. **Proven.**

### 2.2 Mechanism 2 — proven, and the dominant one: the min-avail guard

This is not in the task description and it is the larger effect.

`ivh_uc_tick()` computes `avail_c = elapsed − idle` — the time the vCPU *wanted*
the CPU (`core.c:2643`, and the comment above it is explicit that idle "leaves
the ratio entirely"). `ivh_uc_maybe_close_window()` then refuses to close any
window whose avail is below `ivh_uc_min_avail_pct` (default **10**) of the
window, incrementing `rq->ivh_uc_extended` instead (`core.c:2561`).

An idle vCPU has `avail ≈ 0` by definition. So even on the ticks it *does*
get, every window deadline is refused. This is why the failure is total rather
than merely slow.

Measured, cpu2 over 60 s at idle: `windows +0`, `extended +452`. Four hundred
and fifty-two ticks arrived past a window deadline and every one was blocked.
Since boot (1d 6h uptime) the same CPU showed `windows=3282, extended=505520` —
one publish per 33.5 s against a nominal 0.2 s.

**Correction to the task framing.** "Force `account_process_tick()` to run and
the existing close logic does the rest" is false. Restoring ticks without
restoring *demand* leaves `avail` at zero and the guard blocks every close. A
timer callback that merely calls `ivh_uc_tick()` would publish nothing, forever.

### 2.3 Mechanism 3 — proven by construction: steal is unmeasurable on a halted vCPU

Even setting the guard aside, there is nothing to measure. All three
`ivh_steal_source` values are structurally blind on a halted vCPU:

| source | why it is blind while halted |
|---|---|
| 0 (`paravirt_steal_clock`) | the host charges steal only while a vCPU is **runnable and not running**; a halted vCPU is not runnable |
| 1 (`ivh_ref_steal_ns`) | accumulated by `ivh_ref_accumulate()`, called only from `account_process_tick()` |
| 2 (`ivh_tks_steal_ns`) | accumulated by `ivh_tick_steal_accumulate()`, same caller |

Host contention while this vCPU idles is therefore not just unmeasured, it is
**unmeasurable**. The only way to observe it is to demand the CPU and see how
much arrives. This is exactly what the retired vcap daemon's `SCHED_IDLE`
spinners were for, and it is the reason the answer has to be a spin rather than
a ping. The user's instinct ("keep it active", "spin") was correct and the
cheap alternative is not merely worse, it is non-functional.

### 2.4 One correction to my own framing of the risk

The task describes the danger as a stale *healthy* reading attracting
migrations into hidden contention. On this box the observed staleness ran the
other way — cpu2 frozen at 951 against a true ~1020, i.e. conservatively
*under*-reporting. Nothing in the mechanism prefers either direction; it
reports whatever was last measured, and both directions are reachable.

There is, however, one path that produces false-*healthy* specifically, and it
is the reason for the coverage invariant in sec 4.3: `ivh_uc_close()` at
`core.c:2481` publishes a full `SCHED_CAPACITY_SCALE` when `!avail`. A window
containing no probe at all is not "no measurement", it is an actively wrong
measurement asserting perfect health. **Inferred from code reading, not
separately measured.**

---

## 3. Method

Measurements come from a **userspace emulator** of the probe
(`ka_probe.c`: pin to a CPU, then repeat `{ clock_nanosleep(P); busy-spin D }`),
because the kernel code cannot be booted this session. From the rq's point of
view this is a faithful proxy for the in-kernel probe: a pinned timer wake
followed by a bounded stretch of real, non-idle, tick-restarting guest
execution. It reproduces the design's period arithmetic too — re-arming happens
at probe *end*, so the true period is `P+D` and measured duty is
`D/(P+D)`, which is what the tables below report.

What the emulator does **not** exercise is listed in sec 7.

- Test CPU **2** (worst case: 0 publishes/60 s at baseline). Control CPU **10**, untouched.
- Freshness from `/proc/ivh_debug`'s `ivh_uc_cpu:` line (`windows`, `extended`, `uc_capacity`) sampled before/after each 60 s run.
- Cost: per-CPU busy fraction from `/proc/stat`; wake rate from `bpftrace` on `tracepoint:power:cpu_idle`.
- **Idle entries = HLT = VM exits, 1:1 on this guest**: `/sys/devices/system/cpu/cpuidle/current_driver` is `none`, so idle is a bare `HLT` with no cpuidle state machine and no MWAIT.
- Nominal target: `ivh_uc_window_ns=200 ms` ⇒ 300 windows/60 s ⇒ 0.20 s/publish.
- `ivh_uc_min_avail_pct` was varied live during the sweep and **restored to its default of 10** afterwards (verified).

---

## 4. The design

### 4.1 Shape

A per-CPU self-rearming `delayed_work` (`core.c:2777`–`2939`), the same
mechanism `ivh_uc_avgcap_work` already uses. On each fire the kworker checks
whether this CPU has ticked recently; if not, it spins on the raw TSC for
`ivh_ka_probe_ns`, then re-arms.

Process context in a normal-priority kworker, deliberately not
`system_highpri_wq` and deliberately not a timer/hrtimer callback: the probe
must be able to spin for milliseconds without blocking IRQs or softirqs on the
CPU it is measuring, and it must itself be a **runnable task** — both so the
vCPU leaves idle in the scheduler's own view, and so a host preemption during
the probe is charged against a runnable vCPU where source 0 can see it.

Verified load-bearing assumption: a *pinned* timer queued against an idle timer
base calls `wake_up_nohz_cpu()` (`kernel/time/timer.c:600-603`), and
`add_timer_on()` sets `TIMER_PINNED`. The wake actually happens.

Constraint 1 is satisfied literally: nothing in `ivh_uc_tick()`,
`ivh_uc_close()` or `ivh_uc_maybe_close_window()` was modified. The keepalive
restarts the tick and the existing machinery runs unchanged on its normal path.
Constraint 2 falls out for free — the keepalive never reads or selects a steal
source; whichever one is armed accumulates by itself once ticks resume.

### 4.2 Targeting: no idle-entry hook, deliberately

The task suggested hooking idle entry/exit (`tick_nohz_idle_enter/exit`, or
reusing `rq->ivh_vact_idle_exit_tsc`). I did not, for two reasons:

1. **A direct measurement exists.** `rq->ivh_uc_prev_tsc` is written by
   `ivh_uc_tick()` on every tick, so "now − that timestamp" *is* the staleness
   being bounded. The probe tests the quantity itself rather than inferring it
   from a proxy transition. A CPU that ticked within the last interval is
   skipped for the cost of one comparison (`skipped_fresh`), which is how
   constraint 4 is met.
2. **The hook is the more expensive option.** A bursty vCPU crosses idle
   entry/exit thousands of times a second, to arm and disarm a timer that fires
   ten times a second. An always-armed timer on a busy CPU costs one expiry per
   interval on a CPU that is awake anyway. The hook would put its cost exactly
   where constraint 4 says there should be none.

### 4.3 The coverage invariant

`ivh_ka_interval_ns` must be **≤ half** `ivh_uc_window_ns`, enforced from both
knobs that can break it (`ivh_ka_interval_covers_window()`, `core.c:1231`).
Rationale in sec 2.4: a window with no probe publishes a perfect 1024 into the
EMA, which points an unmeasured idle vCPU at the *top* of the destination
ranking — the precise outcome this mechanism exists to prevent. The
configuration is refused rather than clamped, matching the "reject, don't
clamp" posture already established by `ivh_proc_uc_window_ns()` and
`ivh_ref_proc_steal_source()`.

This is why **1 s and 200 ms are not candidate intervals at the default 200 ms
window** — a correction to the task's suggested sweep. They are measured in
sec 5.2 anyway, and they are also the worst performers on freshness for an
independent reason.

### 4.4 Shadow-first

`ivh_ka_enabled` defaults to **0** and no work is queued at `late_initcall`;
`ivh_ka_proc_enabled()` is the only thing that ever starts a chain. Clearing
the flag stops each chain within one interval by simply not re-arming — there
is no cancel path. Enabling is refused while `ivh_uc_enabled=0`, mirroring the
existing refusal of `ivh_steal_source=1` while `ivh_ref_steal_enabled=0`: with
`ivh_uc` off, `ivh_uc_prev_tsc` never advances, so every probe would fire
forever and measure nothing.

---

## 5. Measurements

### 5.1 Baseline (no probe), 60 s, otherwise-idle guest

```
cpu2  TEST  uc_cap 951->951   windows +0   (NO PUBLISH)  extended +452
cpu10 ctl   uc_cap 1020->1020 windows +2   (30.0 s/pub)  extended +394
cpu2  busy 0.017%    cpu2  HLT 11.3/s      cpu10 HLT 9.6/s
```

Across all 16 vCPUs at idle, baseline HLT rate ranged 6.1–80.6/s (cpu0 highest,
carrying IRQs). Baseline publish cadence across all 16 ranged from **2.5 s to
never**, against a 0.20 s nominal — a 12× to unbounded degradation.

### 5.2 Interval sweep at the default `ivh_uc_min_avail_pct=10`

| interval P | probe D | duty (measured) | windows/60 s | s/publish | model | extended | cpu2 HLT/s | cpu2 busy |
|---|---|---|---|---|---|---|---|---|
| — (baseline) | — | 0.017% | 0 | **never** | — | +452 | 11.3 | 0.017% |
| 1000 ms | 2 ms | 0.250% | 8 | 7.50 s | 8.00 s | +664 | 17.6 | 0.250% |
| 200 ms | 2 ms | 1.084% | 33 | 1.82 s | 1.85 s | +1041 | 38.3 | 1.084% |
| 50 ms | 2 ms | 3.856% | 117 | 0.51 s | 0.52 s | +1842 | 34.3 | 3.856% |
| 20 ms | 2 ms | 9.051% | 270 | **0.22 s** | 0.22 s | +306 | 55.5 | 9.051% |

The `model` column is

> **publish interval ≈ `ivh_uc_window_ns` × `ivh_uc_min_avail_pct` / (100 × duty)`**, floored at the window

which fits all four measured points **within 7% across a 40× range of duty**.
This is the quantitative form of sec 2.2: on an idle vCPU the entire `avail` in
a window is the keepalive's own duty cycle, so freshness is governed by duty
versus the guard threshold and *not* by how often the timer fires. Note the
`extended` column rising with duty right up until duty crosses the threshold,
then collapsing.

Reaching nominal freshness against the default threshold costs **9% of an idle
vCPU**. That is the price of leaving `ivh_uc_min_avail_pct` alone.

### 5.3 Lowering the guard, and probe shape at equal duty (`min_avail_pct=3`)

| interval P | probe D | duty | windows/60 s | s/publish | extended | cpu2 HLT/s | cpu2 busy |
|---|---|---|---|---|---|---|---|
| 50 ms | 2 ms | 3.838% | 288 | 0.21 s | **+8** | 40.0 | 3.838% |
| 100 ms | 4 ms | 3.835% | 288 | 0.21 s | **+16** | **21.5** | 3.835% |

Two results.

**(a) The guard was the whole problem at that duty.** At `min_avail_pct=10`,
3.86% duty gave 0.51 s/publish and `extended +1842`. At `min_avail_pct=3` the
identical probe gave 0.21 s/publish and `extended +8` — nominal cadence, guard
essentially never firing.

**(b) For a given duty, fewer and longer probes are strictly cheaper.**
Identical freshness and identical CPU cost to three decimal places, but the
VM-exit rate is nearly **halved** (40.0 → 21.5 HLT/s) because the probe *count*
halved. Wake rate tracks probe count; CPU cost tracks duty. They are separable,
and the tuning follows: buy duty with probe length, not with cadence — subject
to the coverage invariant.

Note the control CPU also improved (1.58 s/pub) when the guard was lowered, but
**did not** reach nominal. Lowering `ivh_uc_min_avail_pct` alone is not a fix;
both the probe and the threshold change are necessary.

### 5.4 Probe-duration bias (P=100 ms, `min_avail_pct=1`)

Ground truth for cpu2 at idle is ~1019–1020 (what the continuously-ticking
control CPU reads).

| probe D | duty | windows/60 s | s/publish | converged `uc_cap` | bias | cpu2 HLT/s |
|---|---|---|---|---|---|---|
| 1 ms | 1.15% | 267 | 0.22 s | ~970 | **−4.6%** | 47.8 |
| 2 ms | 1.98% | 294 | 0.20 s | 1013 | −0.6% | 31.7 |
| 4 ms | 3.81% | 288 | 0.21 s | 1011 | −0.8% | 33.9 |
| 8 ms | 7.39% | 278 | 0.22 s | 1022 | +0.2% | 21.7 |

A separate run entering with cpu2 already converged at 1022 was **pulled down
to 965 by 1 ms probes** — the effect is a genuine equilibrium shift, not
sampling noise.

Interpretation (**inferred**, mechanism not independently isolated): the probe's
fixed wake-up cost — IPI, kworker dispatch, cache-cold restart, during which
the vCPU is runnable but not yet executing — is charged as steal against the
probe's own duration. At 1 ms that overhead is several percent of the sample;
by 2 ms it is amortised below 1%. The practical rule is firm regardless of
mechanism: **`ivh_ka_probe_ns` ≥ 2 tick periods.** The 1-tick floor is enforced
in `ivh_ka_proc_probe_ns()` and a sub-2-tick setting emits a `pr_warn` citing
this number.

The HLT column here is noisier than sec 5.3 because probe count is constant
across these rows and background activity on the box varied; sec 5.3's
equal-duty pair is the controlled comparison.

---

## 6. Recommendation

**Ship `ivh_ka_interval_ns=100000000` (100 ms), `ivh_ka_probe_ns=2000000` (2 ms),
and set `ivh_uc_min_avail_pct=1`.** These are the compiled-in defaults for the
two `ivh_ka` knobs; `ivh_uc_min_avail_pct` is left at its existing default of
10 and must be lowered explicitly.

| | value |
|---|---|
| freshness | **0.20 s/publish** — full nominal, from *never* at baseline |
| capacity accuracy | 1013 vs ground-truth ~1019 (−0.6%) |
| `extended` | +0 over 60 s |
| CPU cost | **1.98% of one idle vCPU** (from 0.017%) |
| VM exits | 31.7/s vs 11.3/s baseline → **+20.4 HLT/s per idle vCPU** |
| whole-guest cost, 16 vCPUs all idle | ~0.32 pCPU-equivalent; ~+326 HLT/s |

Reasoning for each choice:

- **100 ms interval** is the loosest value satisfying the sec 4.3 coverage
  invariant at the default 200 ms window, and looser is cheaper in VM exits
  (sec 5.3b). It is chosen as a *maximum*, not tuned as an optimum.
- **2 ms probe** is the shortest value that is unbiased (sec 5.4) and it clears
  the one-tick structural floor with margin.
- **`min_avail_pct=1`** against a measured 1.98% duty gives ~2× margin over the
  threshold, so ordinary jitter in probe delivery cannot push a window back
  into the extended state. Setting it to 2 would leave no margin.
- The whole-guest 0.32 pCPU figure is the honest headline cost. The +326 HLT/s
  is negligible beside it (order 1–2 µs of host time per exit ⇒ ~0.05% of a
  host core); **the cost of this mechanism is guest CPU time, not exit rate.**

**If 2% per idle vCPU is judged too expensive**, the correct thing to give up is
freshness, not probe length: raise the interval toward 100 ms's ceiling and
accept the sec 5.2 model's degradation. Do **not** shorten the probe — it buys
little (cost is duty, and the wake rate rises) and it re-introduces the −4.6%
bias.

**Do not ship `ivh_ka_enabled=1` on this evidence.** It is off by default and
should stay off until it has run once. See below.

---

## 7. Exact state, and what is not validated

**Compiles.** `make kernel/sched/` rebuilds `core.o`, `fair.o`, `bpf_sched.o`,
`build_policy.o`, `build_utility.o` with **zero warnings and zero errors**.

**Nothing in this patch has executed.** Per the no-reboot constraint, the
running kernel is the pre-patch build. Every number in sec 5 comes from the
userspace emulator described in sec 3, not from this code.

Proven by measurement (via the emulator, which faithfully reproduces the probe's
rq-visible behaviour):

- the staleness problem, its magnitude, and that the min-avail guard is its dominant cause (sec 2.1, 2.2, 5.1)
- the duty-cycle/freshness law and its fit across 40× (sec 5.2)
- probe-shape separability of CPU cost from wake rate (sec 5.3)
- probe-duration bias below 2 ticks (sec 5.4)
- the cost figures in sec 6

Proven by code reading only:

- steal is unmeasurable on a halted vCPU under all three sources (sec 2.3)
- the empty-window false-healthy path (sec 2.4)
- pinned timers wake NOHZ-idle CPUs (sec 4.1)

**Untested — every line of the new kernel code**, specifically:

1. `ivh_ka_fn()` dispatch, the `skipped_fresh` freshness test, and re-arm chaining.
2. Both probe abort conditions (`need_resched()`, `nr_running > 1`). The claim that they keep the probe off busy vCPUs is **design intent, unverified.**
3. All three sysctl handlers, including every rejection path and both warnings.
4. The `/proc/ivh_debug` `ivh_ka_cpu:` block (format never rendered).
5. Enable/disable chain start and drain, and the single-chain-per-CPU argument.
6. Whether a 2 ms spin in a `system_percpu_wq` kworker has any second-order effect on other work on that CPU. Believed benign; unobserved.

**Known gap, by design not oversight:** a CPU brought online *after*
`ivh_ka_enabled=1` gets no chain — there is no cpuhp callback. Re-writing
`ivh_ka_enabled=1` re-arms all online CPUs and is idempotent for those already
running. Documented at the `ivh_ka_init()` comment.

### First-boot validation sequence

```sh
sysctl -w kernel.ivh_uc_min_avail_pct=1
sysctl -w kernel.ivh_ka_enabled=1
# expect: no pr_warn about duty cycle (1.98% duty vs threshold 1)
grep -E 'ivh_ka_|^ivh_uc_cpu' /proc/ivh_debug
```

Then, on an idle guest, confirm against sec 5.1 and sec 6:

- `windows` advances ~5/s per idle CPU (baseline: 0)
- `ka_probes` ≈ 10/s per idle CPU; `ka_spin_ms` ≈ 20 ms/s
- `ka_skipped_fresh` dominates `ka_probes` on any **busy** CPU — this is the constraint-4 check and is the single most important thing to look at first
- `ka_misdispatched` stays 0
- `extended` stops advancing

Then load the box and confirm `ka_probes` goes to ~0 while `ka_skipped_fresh`
keeps climbing.
