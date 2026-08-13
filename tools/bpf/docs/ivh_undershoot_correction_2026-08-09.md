# The tick-gap estimator's undershoot is one tick period per preemption event: `ivh_tks_phase_pct=50` is overturned, `=100` is exact, and a multiplier is the wrong instrument

**Date:** 2026-08-09 session (wall clock 2026-08-12, first session on the rebooted kernel)
**Kernel under test:** `6.17.0-rseqport72-plzwork+`, **live**, `/proc/cmdline` contains `nohz=off`; branch `kernel-43-clean`, base commit `6c3874293`
**Host:** INTEL(R) XEON(R) GOLD 6554S, `tsc_khz=2200000`, guest 16 vCPU, `CONFIG_HZ=1000`
**Host contention:** skewed corunner, verified live three times (sec 1.2)
**Status:** everything in sec 2–6 is **live-measured on the running kernel**. Sec 7 is **written and compiled but never executed** and says so at every mention.

---

## 0. The four answers, up front

| question asked | answer |
|---|---|
| **Is the pre-reboot "`phase_pct=50` hurts gate accuracy" finding still true?** | **Overturned.** Its stated mechanism — amplifying false positives by 500 µs each — no longer exists. Under `nohz=off` the false-positive rate on genuinely unstolen vCPUs is **zero events, 0.0 ms of phantom steal**, at *every* `phase_pct` from 0 to 100, measured under real hackbench. Gate separation and hackbench wall time are unharmed at 100. Sec 5. |
| **Is the undershoot proportional or constant-additive?** | **Neither. It is additive *per preemption event*, at exactly one tick period each.** Measured `(true steal − estimate) / ivh_tks_events = 1.000` tick, invariant across a **5.6× range of true steal** (594 ms → 3311 ms per window), on 8 vCPUs, reproduced in 5 independent blocks. Because event count scales with steal, it *presents* as a constant ratio (0.64–0.66) — which is why one sample looks proportional — but the governing law is additive in event count. Sec 3. |
| **Idea A — "add half a tick every time we add steal time".** | **Right mechanism, wrong magnitude.** It is exactly `ivh_tks_phase_pct` (sec 2.3), and the correct value is **100, not 50**. The in-tree half-tick derivation assumes a tick re-armed *relative* to its own delivery; this kernel arms an **absolute** periodic hrtimer, for which the loss is a whole tick. At 50 the estimate reaches 0.82–0.83 of truth; at **100 it reaches 0.998–1.001**. Sec 3.3. |
| **Idea B — "multiply the measurement by 1.5×".** | **Wrong instrument, and it cannot be made to work.** The gain required to reach truth is not a property of the estimator, it is a property of the *guest load shape*: **1.52× on continuously-runnable load and 22.7× on hackbench, same host, same window length, same true steal magnitude.** A fixed multiplier fitted to either is badly wrong on the other; `phase_pct` self-adapts because it scales with the measured event count. Sec 6. |

**Final recommendation:** set **`ivh_tks_phase_pct=100`**. It is an existing, live-adjustable sysctl (no reboot, no code), it makes the estimator **exact** on continuously-runnable load, it improves hackbench recovery **2.1×** with IVH running, and it costs nothing on the operational side — gate separation and wall time are statistically unchanged or slightly better. On the two-axis framing the task asked for: **it improves absolute accuracy substantially and does not hurt gate-decision quality.** That is the tradeoff position, stated for quotation.

One line to change in `/home/nick/IVH`:

```diff
-set_ivh_sysctl ivh_tks_phase_pct 0           # 0, not 50; the "unbiased" value measured WORSE
+set_ivh_sysctl ivh_tks_phase_pct 100         # 2026-08-09: exact correction under nohz=off; see
+                                             # tools/bpf/docs/ivh_undershoot_correction_2026-08-09.md
```

**The residual, stated honestly:** on fine-grained blocking load (hackbench) `phase_pct=100` gets absolute recovery to 0.24–0.44 of truth, not 1.00. A **second, larger and structurally different** deficit term dominates there — the estimator's `− d_idle` subtraction converting the vCPU's own idle time into carry debt that cancels real steal. That is a genuine, citable limitation of the estimator as it ships, it is **not** reachable by any existing sysctl, and the one-line kernel fix for it is written and compiled in sec 7 but **has never executed** and must not be reported as validated.

---

## 1. Method

### 1.1 Instrumentation

`ivh_tks_steal_ns` has no `/proc/ivh_debug` line (`get_tks_compare()` still has no in-tree caller), so it was read directly out of the runqueues with `bpftrace`, per CPU, alongside its `samples`, `events` and `carry_c` companions:

```
$b = kaddr("runqueues"); $o = *(uint64 *)(kaddr("__per_cpu_offset") + cpu*8);
((struct rq *)($b + $o))->ivh_tks_steal_ns
```

**Ground truth is the `kvm_steal_time` page itself**, not `/proc/stat`: the same per-CPU trick against `kaddr("steal_time")` offset 0, giving nanosecond resolution instead of `/proc/stat`'s 10 ms USER_HZ quantisation. `/proc/stat`'s steal column was captured in the same snapshots and agrees throughout (e.g. contended mean 2216 ms vs steal-page 2215.2 ms in one hackbench window), so the two are cross-validated rather than assumed.

Both snapshots also carry `nsecs` taken inside the same `BEGIN` block, so **every window length in this document is measured, not assumed** — an early version of the harness took its snapshots outside the load and had `bpftrace`'s ~1 s attach latency inside the window; that produced the only self-contradictory block of the session and was discarded and rebuilt, not reported.

`ivh_tick_steal_accumulate()` is called **unconditionally** from `account_process_tick()` (`cputime.c:587`) — it does not depend on `ivh_steal_source`. Every accuracy number in sec 3 and sec 4 was therefore taken with **IVH switched off entirely**, so no migration feedback loop touches the measurement. Sec 5's numbers are separate and deliberately taken with the full stack running.

### 1.2 The corunner, verified three times

All 16 vCPUs driven to a busy loop, `/proc/stat` steal delta:

| when | cpu0-7 | cpu8-15 |
|---|---|---|
| session start | 64.3 – 67.7 % | 0.17 – 0.34 % |
| mid-session | 67.6 – 67.8 % | 0.00 – 0.20 % |
| after the anomalous block in sec 3.4 | 67.6 – 67.8 % | 0.20 % |

Stable and strongly skewed throughout. **A correction to an assumption worth recording:** the guest's steal is a function of *aggregate guest demand*, not a fixed per-vCPU property. Busy loops on 1 / 4 / 8 / 16 vCPUs give contended-vCPU steal of **0.2 % / 19–30 % / 44–46 % / 67.8 %**. An idle-guest steal sample reads ~0 % and proves nothing; the first check of the session was made that way and was invalid. This property is used constructively in sec 3.2 to sweep true steal over a 5.6× range without introducing any idle time.

### 1.3 Workloads

- **CR (continuously runnable):** pure spin loops, one per vCPU, zero idle. Isolates the tick-phase mechanism.
- **HB (hackbench):** `hackbench -T -g 1 -f 8 -l 400000` — fine-grained pipe blocking, ~35 % idle on the contended vCPUs. This is the project's reference workload and the hard case.
- Guest-health control, same batch: `taskset -c 8-15 hackbench …` → **11.17 s, 10.78 s**. Consistent with the 10.85–10.88 s recorded in `ivh_solution_search_2026-08-09.md` sec 9, so the guest itself has not moved across the reboot.

---

## 2. What `ivh_tks_phase_pct` actually does — read, not assumed

The task asked whether Idea A is really the existing knob or something subtly different. Reading `core.c:2426-2434` (post-edit line numbers; the logic is unchanged from what was measured):

```c
excess_c = (s64)avail_c - (s64)tick_c;
if (excess_c > (s64)ivh_tsc_ns_to_cycles(READ_ONCE(ivh_tks_deadband_ns))) {
        unsigned long pct = READ_ONCE(ivh_tks_phase_pct);
        if (pct)
                excess_c += (s64)div64_u64(tick_c * pct, 100);
        rq->ivh_tks_events++;
}
carry = rq->ivh_tks_carry_c + excess_c;      /* unconditional */
```

Three things follow, and they matter for judging Idea A precisely:

1. **The addition is per qualifying tick interval, not per drain.** `ivh_tks_events` is incremented on exactly the same condition, so `phase_pct` adds `pct%` of a tick **once per counted event**. That is what makes `ivh_tks_events` the correct denominator for the deficit in sec 3 — the two are the same population by construction.
2. **It is applied *before* the carry**, so it survives the signed-carry arithmetic rather than being added to the output. A negative carry can still absorb it. This is *broader* than "every time we add steal time" in the user's phrasing (it fires whether or not the carry subsequently drains) and is the correct place for it.
3. **The deadband gates only the phase correction and the event counter, never the accumulation.** At `phase_pct=0` the deadband is provably inert, which `ivh_final_tsc_only_build_2026-08-08.md` sec 6.3 established and this reading confirms. The corollary is the reverse of how it has been used: **the deadband only starts doing anything once `phase_pct` is nonzero**, so shipping `phase_pct=100` also brings the deadband into service for the first time.

So Idea A *is* this knob, at value 50. The only thing the user's phrasing gets wrong is the magnitude, and sec 3.3 shows why.

**Range:** the handler `ivh_tks_proc_phase_pct()` rejects values above 100 with `-EINVAL` (confirmed live — a write of 150 returns `Invalid argument`). Since sec 3 shows the exact correction is 100, the existing clamp is right at the boundary and needs no change.

---

## 3. The shape of the undershoot: one tick per preemption event

### 3.1 The arithmetic identity, then the measurement

Over any window, summing the estimator's own per-tick excess telescopes:

```
Σ excess_i = (wall − N_ticks × TICK) − Σ d_idle_i
true steal  = wall − runtime
⇒ deficit   = true_steal − Σ excess_i = N_ticks × TICK − busy_time
```

With **zero idle**, `N_ticks = busy_ms + n_bursts`: this kernel arms the scheduler tick as an `HRTIMER_MODE_ABS_PINNED_HARD` hrtimer forwarded by `TICK_NSEC`, so a preemption that spans one or more deadlines fires **one** interrupt immediately on resume and `hrtimer_forward()` skips the rest. That resumption tick consumes no runtime, so each preemption burst contributes exactly one extra tick — and therefore exactly one `TICK_NSEC` of deficit.

**Predicted: `deficit / n_bursts = 1.000 tick`.** `ivh_tks_events` counts the qualifying intervals, i.e. the bursts.

### 3.2 Measured, CR load, `phase_pct=0`, true steal swept 5.6×

True steal per vCPU was varied by varying how many contended vCPUs are busy (sec 1.2), which keeps idle at exactly zero at every point.

| busy vCPUs | true steal / vCPU | estimate | **ratio** | events | **deficit / event** |
|---|---|---|---|---|---|
| 4 | 594 – 2144 ms | 385 – 1384 ms | 0.638 – 0.648 | 209 – 771 | **0.998 – 1.007** |
| 6 | 2799 – 2817 ms | 1833 – 1851 ms | 0.651 – 0.659 | 957 – 980 | **0.999 – 1.004** |
| 8 | 3291 – 3312 ms | 2166 – 2181 ms | 0.654 – 0.661 | 1118 – 1142 | **1.000 – 1.003** |
| 6 (rep 2) | 2787 – 2817 ms | 1824 – 1836 ms | 0.647 – 0.657 | 956 – 988 | **1.000 – 1.005** |
| 6 (rep 3) | 2812 – 2827 ms | 1787 – 1797 ms | 0.632 – 0.639 | 1016 – 1039 | **0.998 – 1.001** |

**44 per-vCPU observations. Every single one lies in 0.998 – 1.007 ticks per event**, while the true steal underlying them varies by 5.6× and the raw recovery ratio stays pinned near 0.65.

This is the answer the task asked for, and it is neither of the two offered shapes:

> The undershoot is **not** a flat per-CPU constant (it tracks event count over a 5.6× range) and it is **not** intrinsically multiplicative (nothing in the mechanism scales with steal magnitude). It is **additive in the number of preemption events, at exactly one tick period each.** It *appears* proportional in any single measurement because a busier host produces proportionally more preemption bursts — which is precisely the trap a one-sample ratio walks into.

This also reconciles the two apparently conflicting priors: the pre-reboot report's "near-constant ~0.178 s offset regardless of true steal" was a NOHZ-era artefact (the estimator was then dominated by un-ticked busy time, not by tick phase), and the orchestrator's fresh 0.25–0.29 ratios are reproduced here exactly — sec 5 gets 0.206 mean under the same conditions — but they are a *consequence* of event count, not evidence of a proportional law.

### 3.3 The correction is 100 %, and the in-tree half-tick derivation is wrong for this kernel

`core.c`'s comment derives the unbiased correction as half a tick, from the model "interval = phase + preemption, and the pre-deadline part is invisible with expectation half a tick". That derivation is correct for a tick **re-armed relative to its own delivery**. This kernel arms an **absolute** periodic hrtimer, so the tick *after* a burst lands on the next fixed multiple of `TICK_NSEC`, not one full tick after resume. Working the two affected intervals through, the expected deficit is `E[ψ] + P(φ+ψ<1) ≈ 0.5 + 0.5 = 1.0` tick, not 0.5.

Measured, CR load, 8 contended vCPUs, all values reproduced in ≥2 independently ordered blocks (ascending and descending, to exclude drift):

| `ivh_tks_phase_pct` | contended ratio (estimate / truth) | deficit / event | clean-vCPU phantom |
|---|---|---|---|
| 0 | **0.652, 0.656** | 1.000 | 0.0 ms, 0 events |
| 25 | 0.732, 0.744 | 0.742, 0.749 | 0.0 ms |
| 50 | **0.819, 0.826, 0.828** | 0.500, 0.501 | 0.0 ms |
| 100 | **1.000, 1.000, 1.000, 1.000, 1.001** | −0.000 – 0.000 | 0.0 ms |

Perfectly linear in `pct`, as the mechanism requires. **At `phase_pct=100` the TSC-only estimator reproduces the hypervisor's own steal accounting to within 0.1 %**, with no access to the steal page, no vPMU, and no hypercall. For the paper's evaluation section that is the headline number, and it is reproduced in five independent blocks (18 per-vCPU observations at 100, all in 0.998–1.001).

### 3.4 One anomalous block, recorded rather than dropped

One `N=6, phase_pct=100` block read 1.89–4.09 instead of 1.000, with true steal simultaneously reading 780–1856 ms where its neighbours read ~2800 ms. The corunner check immediately afterwards was unchanged (67.6–67.8 %). It did not reproduce in three consecutive re-runs of the identical configuration (0.998–1.001 every time). It is recorded as an unexplained transient — most likely the host-side variance that `ivh_solution_search_2026-08-09.md` sec 9 identifies as the largest uncontrolled variable in this project — and it is the reason every headline number here carries ≥2 independent reproductions.

---

## 4. The second deficit term: idle subtraction, and why hackbench is the hard case

### 4.1 The measurement

Under hackbench the same estimator behaves completely differently. IVH **off**, contended vCPUs, three reps in both orderings:

| `phase_pct` | recovery ratio, rep 1 / 2 / 3 | mean | clean-vCPU phantom |
|---|---|---|---|
| 0 | 0.039 / 0.010 / 0.082 | **0.044** | 0.0 ms (18–22 events) |
| 50 | 0.223 / 0.027 / 0.149 | **0.133** | 0.0 ms |
| 100 | 0.476 / 0.124 / 0.123 | **0.241** | 0.0 – 0.1 ms |

Monotone in the mean — `phase_pct=100` is a **5.5× improvement** — but the run-to-run spread is large and the ordering inverts in rep 3. Stated plainly: **on fine-grained blocking load the phase correction helps substantially in expectation but is not the dominant term, and a single hackbench window cannot resolve it.**

### 4.2 Why: `− d_idle` turns idle into debt

From the identity in sec 3.1, with idle present, `N_ticks = busy + idle + n_bursts` (under `nohz=off` an idle vCPU still ticks every millisecond), so

```
deficit = N_ticks × TICK − busy = idle + n_events
```

The per-event term is what `phase_pct` fixes. The **idle** term is new, larger, and structural: `excess_i = elapsed_i − d_idle_i − TICK`, so an idle tick with no steal yields `excess = −1 tick` — a full tick of negative carry — and that debt cancels real steal measured later in the window. Directly confirmed in the same windows (per-CPU `/proc/stat` captured in the same snapshots):

| | window | true steal | idle | busy | estimate at `pct=100` | residual deficit | residual / idle |
|---|---|---|---|---|---|---|---|
| HB rep 1 | 6508 ms | 2458 ms | 1418 ms | 2500 ms | 1170 ms | 1289 ms | **0.91** |
| HB rep 2 | 6838 ms | 2311 ms | 2442 ms | 2142 ms | 286 ms | 2025 ms | **0.83** |
| HB rep 3 | 6519 ms | 2167 ms | 2500 ms | 2075 ms | 267 ms | 1900 ms | **0.76** |

The residual that survives a perfect phase correction is **0.76 – 0.91 × the vCPU's own idle time**, and the run-to-run variation in recovery (0.12 → 0.48) is explained by the run-to-run variation in idle (2500 → 1418 ms), not by anything about steal. Accounting closes: `idle + busy + steal ≈ window` on every row.

This is the correct citable limitation for the paper: **the tick-gap estimator is exact on continuously-runnable load and is compressed on load that blocks, by an amount equal to the guest's own idle time, because the estimator subtracts idle from a signal whose idle-interval gaps are already steal.**

### 4.3 The one existing knob that touches it, and its cost

`ivh_tks_carry_ticks` bounds the debt the idle term can bank, so it is a live, no-reboot partial lever. At `phase_pct=100`, hackbench, contended vCPUs:

| `carry_ticks` | contended recovery | **clean-vCPU phantom steal** |
|---|---|---|
| 1 | **0.810** | 54.4 ms (25× the true 2.2 ms) |
| 2 | 0.485 | 29.2 ms (11×) |
| 8 (shipped) | 0.476 | **0.0 ms** |
| 32 | 0.313 | 0.0 ms |

It genuinely buys accuracy — 0.48 → 0.81 — but it is the **first setting in this whole session that reintroduces phantom steal on a genuinely unstolen vCPU**, which is the exact failure mode `nohz=off` was spent to remove. In capacity terms 54.4 ms over a 6.8 s window is only ~0.8 %, so it is not obviously fatal, but it was not benchmarked for gate quality and **is not recommended without that work.** It is recorded as a lead, not a result. Note also that `ivh_final_tsc_only_build_2026-08-08.md` recorded this sweep as "flat across 1/8/200" — that was flatness in *wall-clock throughput*, and it is not flat in absolute accuracy; both statements are true and they measure different things.

---

## 5. Operational half: does `phase_pct=100` cost anything?

Full IVH stack up (`/home/nick/IVH`: `ivh_steal_source=2`, `ivh_cap_source=3`, `ivh_uc_used_source=0` WALL, `ivh_selection_trylock=1`, `ivh_capacity_threshold=1010`, `MY_ivh_atc` + `vcap_probe -p 200 -s 5000`, `ivh_cfg=3`). Each arm change is followed by a **discarded warm-up run**, because `ivh_uc_capacity` is an EMA with a 10.4 s half-life at the 200 ms window and a single run is only ~1.5 half-lives.

### 5.1 Hackbench wall time — interleaved, both orderings

| block | order | `pct=0` | `pct=100` |
|---|---|---|---|
| 1 | 0 then 100 | 11.80, 11.84 | **11.47, 11.27** |
| 2 | 100 then 0 | 12.33, 12.07 | **11.93, 11.78** |
| 3 | 0, 50, 100 | 12.15, 11.99 | 12.00, 12.19 |

| arm | n | mean | sd |
|---|---|---|---|
| `pct=0` | 6 | **12.03 s** | 0.20 |
| `pct=50` | 2 | 11.79 s | — |
| `pct=100` | 6 | **11.77 s** | 0.35 |
| **IVH off**, same batch | 3 | **16.12 s** (16.03, 16.26, 16.07) | 0.13 |
| guest health, `taskset -c 8-15` | 2 | 11.17, 10.78 s | — |

`phase_pct=100` is faster in blocks 1 and 2 (which are ordered oppositely, so this is not drift) and is not in block 3. Over all six pairs the difference is **−0.26 s, t ≈ 1.58, p ≈ 0.15 — not significant.**

**The honest claim is therefore "no cost", not "a win":** `phase_pct=100` is 2.1 % faster in the mean with n=6, which this benchmark cannot separate from noise, and it is decisively not the 26 %-disagreement catastrophe the pre-reboot record predicted. IVH itself is doing 16.12 → ~11.8 s against a 10.98 s uncontended-only floor, in the same batch, at both settings.

### 5.2 The gate-quality metric, and a caveat about the in-kernel one

**The in-tree shadow comparator cannot answer this question on the current configuration**, and this needs recording because the pre-reboot 26.4 % figure came from it: `ivh_decision_shadow`'s `pass_real` arm reads `rq->cpu_capacity`, which is fed by `vcap` — and `vcap` has been retired in favour of `vcap_probe`, which publishes nothing. Live check: `cpu_capacity` reads a flat **1024 on all 16 vCPUs**. Any `ivh_uc_pass_*` / `ivh_dec_*` ratio computed today is comparing against a constant. **The 1.51 % / 26.4 % numbers in `core.c:324` should not be re-derived from that path without first restoring `vcap`.**

So gate quality was measured directly against steal-page ground truth instead: sample `ivh_uc_capacity` per vCPU every 200 ms through each run (45 samples/run), with the contended (0–7) / clean (8–15) split as truth.

| rep (interleaved) | `pct=0` separation | `pct=100` separation |
|---|---|---|
| 1 | 51.6 | 51.5 |
| 2 | 54.5 | 60.5 |
| 3 | 62.0 | 63.2 |
| **mean** | **56.0** | **58.4** |

Clean-population capacity: 1014–1022 at `pct=0`, 1009–1023 at `pct=100`. Destination-set inversions and clean-below-hardfloor rates were identical (1 sample in 45, a first-sample artefact) at every setting.

**Separation is unchanged — very slightly better at 100.** A first, non-interleaved sweep of this metric showed separation apparently *falling* with `phase_pct` (81 → 55 → 51); that was monotone session drift (contended capacity rose 936 → 975 straight through the ordered sequence regardless of arm) and is discarded. It is noted because it is exactly the kind of artefact that would have produced a confident wrong answer.

### 5.3 The direct falsification of the old finding

The pre-reboot mechanism was named explicitly in `core.c:319-324`: the correction "is only valid on intervals where a preemption really did span the deadline… and it multiplies every false positive by 500 µs". That mechanism requires false positives. Measured on the eight genuinely-unstolen vCPUs (0.2 % real steal), under real hackbench, at every setting tested:

| `phase_pct` | clean-vCPU events / 6.5 s window | clean-vCPU estimated steal | clean-vCPU true steal |
|---|---|---|---|
| 0 | 12 – 22 | **0.0 ms** | 2.6 – 4.1 ms |
| 50 | 12 – 24 | **0.0 ms** | 2.6 – 8.2 ms |
| 100 | 15 – 24 | **0.0 – 0.1 ms** | 3.0 – 3.5 ms |

And in 11 of 12 IVH-on runs, clean-vCPU estimated steal was **0.0 ms** at every `phase_pct` (one 74 ms outlier at 100, in a run whose clean capacity still published 1009). Under `nohz=off`, `excess_i ≤ 0` structurally on any unpreempted vCPU, so the false positives the old default was protecting against do not occur. **The protection is now paying a 35 % accuracy penalty for a risk that no longer exists.**

---

## 6. Idea B — the multiplier — and why the data rules it out

The task asked to settle proportional-vs-additive *before* implementing a multiplier. Sec 3 settles it: additive per event. Three consequences, all from measured numbers:

**(a) The required gain is a property of the workload, not the estimator.** At `phase_pct=0`, on the same host, in the same session, with the same window length and comparable true steal:

| load shape | recovery | gain needed to reach truth |
|---|---|---|
| continuously runnable (CR) | 0.656 | **1.52×** |
| hackbench, IVH off | 0.044 | **22.7×** |
| hackbench, IVH on | 0.206 | **4.9×** |

A 1.5× multiplier is very nearly exactly right for CR — which is presumably where the user's intuition comes from — and is off by a factor of 15 for hackbench. There is no constant that serves both. Worse, *within* the hackbench arm alone the required gain varies run to run from 12× to 100× (recovery 0.010 – 0.082), because it is really tracking that run's idle fraction.

**(b) Where a multiplier would be right, `phase_pct` is already exactly right, and derived rather than fitted.** On CR load a 1.52× gain and `phase_pct=100` land on the same answer. But `phase_pct` gets there by adding one tick per *measured* event, so it self-adapts when the burst structure changes; a fitted gain does not. `phase_pct=100` is 1.000 on CR and 0.241 on hackbench; a gain of 1.52 fitted on CR gives 1.000 on CR and **0.067** on hackbench.

**(c) It does not deliver the differentiation the user wants, because that differentiation is already present.** The stated intent was "keep clean vCPUs low, pull contended vCPUs toward truth". Measured: clean vCPUs read **0.0 ms** and contended read 0.64–0.66 of truth, with the ratio *constant across contended vCPUs*. The estimator is already proportionally faithful; it is the **scale** that is wrong, and only on a per-event basis. A multiplier would scale a quantity that is already correctly ordered — and would scale any clean-vCPU phantom by the same factor the moment one appears (e.g. the 54 ms at `carry_ticks=1` becomes 1.2 s at 22×).

**No multiplier sysctl was implemented.** Implementing one would have meant a kernel change (and therefore an unverifiable result this session) for a mechanism the data says is the wrong shape, when an existing live knob does the job exactly. That is a deliberate decision, not an omission.

---

## 7. The kernel change for the residual idle term — **written, compiled, NEVER EXECUTED**

The one deficit term no existing sysctl reaches is sec 4.2's idle subtraction. The fix is to stop subtracting idle once the tick is periodic, because under `nohz=off` the raw inter-delivery gap *is* the delay on an idle vCPU as much as a busy one — a halted vCPU becomes runnable at its own timer deadline and the host's failure to run it from that instant is charged by KVM as steal. Corroborating measurement already in hand: in the first CR block, contended vCPUs that were **idle** carried 136–158 ms of real steal-page steal in a 5 s window and the estimator reported **0.0 ms** for them, because the idle subtraction consumed all of it.

Implemented in `kernel/sched/core.c` as `ivh_tks_idle_sub`, shadow-first (default **1** = byte-for-byte the historical estimator; **0** = no subtraction, to be set only together with `nohz=off`):

- new sysctl definition + rationale comment alongside the other `ivh_tks_*` defaults;
- three lines in `ivh_tick_steal_accumulate()`: the delta is still computed and still seeds `prev_idle_ns`, so the two modes are switchable at runtime with no discontinuity in the series;
- `ctl_table` entry, deliberately **without** `.extra1`/`.extra2` — `SYSCTL_ZERO`/`SYSCTL_ONE` are `int *` and `proc_doulongvec_minmax()` dereferences its extras as `unsigned long *`, which would read four bytes of adjacent rodata as the high half of the bound. Every other unsigned-long IVH knob omits them for the same reason.

It is **not** conditioned on `tick_nohz_active` directly: that would silently change behaviour on a tickless boot of a kernel whose whole tks pipeline was calibrated with the subtraction in place.

The stale `phase_pct` rationale comment at `core.c:315-326` was also corrected in place, since it is the first thing a reader hits and it currently asserts a finding this document overturns.

**Verification performed, and its exact limit:**

```
CC      kernel/sched/core.o      (zero warnings, zero errors)
AR      kernel/sched/built-in.a
```

Disassembly-checked as well, because this project has been bitten by a miscompile on exactly this arithmetic before:

```
cf72:  mov    ivh_tks_idle_sub(%rip),%rdx
cf79:  test   %rdx,%rdx
cf7c:  cmovne %rax,%rdx
```

— branchless, and correct in both directions: `%rax` holds the idle delta, `%rdx` holds the loaded flag, so a zero flag leaves `%rdx = 0` and a nonzero flag selects the delta. The surrounding `mul`/`div` `TICK_NSEC` conversion and its `< 1000` sanity floor are intact; the old `OPTIMIZER_HIDE_VAR` miscompile has not returned.

**This code has never run.** Not one instruction of the `idle_sub=0` path has executed; on the running binary the sysctl does not exist. Its predicted effect — closing the sec 4.2 residual and taking hackbench recovery from ~0.24 toward ~1.0 — is a **prediction from an arithmetic identity that has been validated only in its `idle=0` special case (sec 3)**. It requires a rebuild and reboot, and until then it is "designed, compiles, unverified" and must be reported as such.

---

## 8. Honest confidence

| claim | verdict | confidence |
|---|---|---|
| Deficit = exactly 1 tick per `ivh_tks_events`, on continuously-runnable load | **True** | **High** — 44 per-vCPU observations, all 0.998–1.007, across a 5.6× steal range, 5 blocks, both sweep directions |
| `phase_pct=100` makes the estimator exact (0.998–1.001) on that load | **True** | **High** — 18 observations, 5 independent blocks |
| The undershoot is proportional | **False** — it is per-event additive; it only *looks* proportional | **High** |
| The undershoot is a flat per-CPU constant | **False** — that was a NOHZ-era artefact | **High** |
| A fixed multiplier can fix it | **False** — required gain is 1.52× vs 22.7× on the same host | **High** |
| "`phase_pct=50` hurts gate accuracy" still holds post-`nohz=off` | **Overturned** | **High** on the mechanism (false-positive rate is measured at zero); **Moderate-high** on the operational consequence (n=6 per arm) |
| `phase_pct=100` costs nothing operationally | **True** | **Moderate-high** — wall time −0.26 s (p≈0.15, i.e. no detectable cost), separation 56.0 → 58.4, zero clean phantom in 11/12 runs |
| `phase_pct=100` *improves* hackbench wall time | **Not demonstrated** | 2 of 3 blocks favour it; not significant at n=6 |
| The residual hackbench deficit is the idle subtraction | **True** | **Moderate-high** — residual is 0.76–0.91× measured idle across 3 windows, and the arithmetic identity predicts it; not isolated by a direct intervention |
| Removing the idle subtraction would fix it | **Predicted, untested** | **Moderate** — the identity is validated in its `idle=0` case only; the code has never executed |
| `carry_ticks=1` is a usable partial substitute | **Not demonstrated** | It buys 0.48 → 0.81 accuracy but reintroduces clean-vCPU phantom and was not gate-benchmarked |
| Clean vCPUs produce zero phantom steal under `nohz=off` | **True** | **High** — 0.0 ms in every hackbench window measured, at every `phase_pct`, IVH on and off. This is the `nohz=off` fix working, independently confirmed |

**Overall.** The undershoot had a single dominant cause on continuously-runnable load, it is now identified exactly and corrected exactly by an existing runtime knob, and the correction is free operationally. On the project's own reference workload a second and larger cause remains, it is identified and quantified, and its fix is written but unverifiable without a reboot. For the paper's evaluation section the defensible pair of statements is: *the TSC-only estimator reproduces hypervisor-reported steal to within 0.1 % on continuously-runnable load, and is compressed to 0.24–0.44 of truth on fine-grained blocking load by an amount equal to the guest's own idle time.*

---

## 9. Exact state left on the machine

- **No reboot, no kernel install, no BPF rebuild or reload.** Running kernel is still `6.17.0-rseqport72-plzwork+`.
- **Sysctls restored to the `/home/nick/IVH` canonical state:** `ivh_tks_phase_pct=0`, `ivh_tks_carry_ticks=8`, `ivh_tks_deadband_ns=50000`, `ivh_universal_eligible=1`, `ivh_steal_source=2`, `ivh_cap_source=3`, `ivh_uc_used_source=0`, `ivh_capacity_threshold=1010`, `ivh_selection_trylock=1`, `ivh_uc_min_steal_ns=500000`, `ivh_decision_shadow=0`. **The recommended `phase_pct=100` was deliberately NOT left set** — changing the operating point is the user's call, and sec 0 gives the one-line diff.
- **Daemons:** one `MY_ivh_atc`, one `vcap_probe -p 200 -s 5000`, `ivh_cfg=3`. IVH was launched during this session via `/home/nick/IVH` and left running. `setup.sh` reported `no cached module for 6.17.0-rseqport72-plzwork+` (`vsched_module.ko` was never built for this kernel); this is **inert for the current pipeline** — the module backs the retired `vcap`'s procfs nodes, and `vcap_probe` plus in-kernel `ivh_uc_capacity` do not use them. Migrations, capacity publication and the 16.12 → 11.8 s effect all work without it. Flagged rather than fixed.
- **No stray guest load.** All spinners, duty generators and hackbench instances exited and were verified gone.
- **`kernel/sched/core.c`** modified in the working tree, uncommitted, on top of the pre-existing uncommitted work: the `ivh_tks_idle_sub` sysctl (sec 7) and the corrected `phase_pct` rationale comment. **Nothing committed, nothing pushed.**
- **`tools/bpf/MY_ivh_atc.bpf.c` not touched** — its diff is the pre-existing one from earlier sessions.
- `/home/nick/IVH`, `/home/nick/ivh_mode.sh`, `/home/nick/ivh_verify.sh`, `/etc/default/grub` **not touched.**
- Object files under `kernel/sched/` are newer than the installed kernel as a consequence of the compile check; normal, and inert for the running system.
