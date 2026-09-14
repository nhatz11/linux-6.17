# CVM-safe steal detection: sub-tick blind spot, and `ivh_steal_source=2`

**Date:** 2026-08-08
**Kernel under test:** `6.17.0-rseqport70-stealfix+` (live), branch `kernel-43-clean`, base commit `6c3874293`
**Host:** INTEL(R) XEON(R) GOLD 6554S, `tsc_khz=2200000`, guest 16 vCPU, `CONFIG_HZ=1000`
**Host contention:** skewed 8-vCPU corunner, active and verified for every run reported here
**Status:** kernel code written and compile-verified. **NOT live-tested — requires a reboot.**

---

## 0. What this document decides

Two things, in order:

1. **Does the sub-tick blind spot of a tick-piggybacked steal detector actually change
   IVH's migration-destination gate decisions?** Measured answer: **no.** Section 3.
2. **Given that, what should the CVM-safe detector actually be?** The design that was
   drafted going in (`(T2-T1) - 0.5*tick_period` on the raw TSC delta) is **wrong in a way
   that matters**, and the correction that makes it theoretically unbiased makes it
   measurably worse in practice. Sections 4-5. What shipped is section 6.

Section 7 is the honest list of what is not yet established, including the open
PV-steal-trust question, which this document does **not** resolve.

---

## 1. Provenance of every number below

All numbers are from live capture on the running kernel. Nothing here is simulated
workload or synthesised steal. What *is* offline is the **replay**: per-tick series were
captured live, then the exact `ivh_uc_tick()` / `ivh_uc_close()` arithmetic
(`kernel/sched/core.c`) was re-executed over them in Python. That is deliberate and it is
the stronger experiment, because it makes the A/B a **controlled** one — both arms see
byte-identical input and differ only in the steal term. Two live runs could not do that;
they would differ in workload noise as well.

Where a number is derived rather than measured, it says so.

### 1.1 Capture method

`ivh_uc_tick()` is called once per tick per CPU from `account_process_tick()`
(`kernel/sched/cputime.c:603`), before the `vtime_accounting_enabled_this_cpu()` early
return, so it is a universal per-tick anchor. A kprobe on it reads the kernel's **own**
per-tick series out of `struct rq` — these are the exact values `ivh_uc_tick()` itself
consumes, not a parallel reconstruction:

```
kprobe:ivh_uc_tick
{
    $off = *(uint64*)(kaddr("__per_cpu_offset") + cpu * 8);
    $rq  = (struct rq *)(kaddr("runqueues") + $off);
    $ks  = kaddr("kernel_cpustat") + $off;
    printf("%d %llu %llu %llu %llu %llu\n", cpu,
           $rq->ivh_uc_prev_tsc,          /* raw TSC stamp, previous tick    */
           $rq->ivh_uc_prev_idle_ns,      /* NOHZ idle  (what the kernel uses) */
           *(uint64*)($ks + 40) + *(uint64*)($ks + 48),  /* kcpustat IDLE+IOWAIT */
           $rq->ivh_uc_prev_steal_ns,     /* PV steal -- GROUND TRUTH        */
           *(uint64*)($ks + 0) + *(uint64*)($ks + 8) + *(uint64*)($ks + 16));
}
```

`ivh_uc_prev_*` are all written at the end of the *previous* `ivh_uc_tick()`, so the four
series are mutually phase-aligned by construction. Ground truth is
`rq->ivh_uc_prev_steal_ns`, which at the live setting `ivh_steal_source=0` is
`paravirt_steal_clock()` — the host-authored `kvm_steal_time` page. No REF_TSC / vPMU
value is used as ground truth anywhere in this document, so the "phantom tax" confound
that inflated the earlier REF_TSC-based sub-tick figure cannot be present.

### 1.2 The two workloads

| tag | workload | why |
|---|---|---|
| **E** | `ivh_exec -v hackbench -T -g 1 -f 8 -l 400000`, ~14 s | comparability with all prior measurements in this thread; **~60% idle**, which turns out to be the hard case |
| **F** | 16 pinned busy loops, 14 s | saturating, **~20% idle**; the regime IVH actually operates in under load |

Both captures span ~18 s (3 s lead-in). Per-CPU tick counts 4.6k-14k. `bpftrace` reported
no lost events on any run.

### 1.3 Host contention was verified, not assumed

A prior measurement in this thread was accidentally taken with the host corunner off. That
is checked explicitly here. Independent evidence, on every run:

- `ivh_exec -v`'s own ground-truth line: **0.0014% - 0.0016%** host-preempted CS cycles
  across four hackbench runs, consistent with the ~0.0018% seen earlier in this thread.
- Direct `/proc/stat` steal accrual under a 6 s all-vCPU busy load, taken before any
  capture:

```
cpu0..cpu7   : 361-367 USER_HZ steal / 6 s  = ~61% stolen
cpu8..cpu15  : 1-2     USER_HZ steal / 6 s  = ~0.2% stolen
```

Strongly skewed and unambiguous. The population is genuinely bimodal, which is what makes
the gate question answerable at all.

---

## 2. Correcting the derivation before using it

The brief supplied a formula to verify rather than trust. Working it through:

A tick fires when a TSC-deadline interrupt is **delivered**, which requires the vCPU to be
running. Let `P` = nominal tick period. A preemption of duration `D` starts at offset `s`
into the interval, `0 <= s < P`. It delays the next delivery only if it spans the deadline,
i.e. `s + D > P`. When it does, the vCPU resumes at `T1 + s + D` and the tick fires
immediately, so the observed delta is:

```
Δ = T2 - T1 = s + D
```

The naive excess is `Δ - P = s + D - P`, which understates the true `D` by `(P - s)`.

- **`D >= P`:** every `s` in `[0, P)` spans the deadline, so `s` is uniform and
  `E[P - s] = P/2`. The unbiased estimator is `Δ - P/2`. **The brief's corrected formula
  and its sign are confirmed.** It is a subtraction of half a period from the raw delta,
  equivalently an *addition* of half a period to the excess `Δ - P`. Both phrasings agree.
- **`D < P`:** the preemption spans the deadline only with probability `D/P`. Given it
  does, `s` is uniform on `(P-D, P)`, so `E[Δ - P] = D/2` and applying the `+P/2`
  correction yields `D/2 + P/2` — a large *over*-estimate. Unconditionally, expected
  recovered mass is `(D/P)(D/2 + P/2) = D²/2P + D/2`, which tends to `D/2` as `D -> 0` and
  meets `D` continuously at `D = P`.

**One refinement to the brief's framing.** The brief describes sub-tick preemptions as
"completely invisible". That is true only of those that resolve before the deadline. Taken
over all phases, a sub-tick preemption of duration `D` is recovered at roughly `D/2`, not
zero. The blind spot is a ~50% attenuation of sub-tick mass, not a total loss. This makes
the blind spot *smaller* than feared, and section 3 shows it does not bind regardless.

---

## 3. Part 1 — the sub-tick blind spot does not change gate decisions

### 3.1 Sub-tick mass (reproduction)

Per-tick PV-steal deltas, split at 1 ms:

| capture | group | sub-1 ms | >=1 ms |
|---|---|---|---|
| E hackbench | contended cpu0-7 | 2.584 s (**7.8%**, n=3362) | 30.366 s (92.2%, n=12794) |
| E hackbench | clean cpu8-15 | 0.125 s (90.6%, n=6824) | 0.013 s (9.4%, n=7) |
| F saturating | contended cpu0-7 | 0.021 s (**0.0%**, n=124) | 74.698 s (100.0%, n=13050) |
| F saturating | clean cpu8-15 | 0.226 s (100.0%, n=28770) | 0.000 s (0.0%, n=0) |

All-CPU aggregate for E is **8.2%** sub-tick, against the **14.0%** measured earlier in
this thread on the same workload with the same real-steal-page method. Same order,
different run; the difference is workload phase and idle fraction, not method. Either
number supports the same conclusion.

The structurally important detail is not the headline percentage but **where the sub-tick
mass sits**: it is ~100% of the steal on the *clean* CPUs, where the absolute quantity is
0.125-0.226 s out of ~18 s (0.7-1.3% of wall time), and only 0-7.8% on the *contended*
CPUs. Sub-tick steal is concentrated exactly where it cannot matter.

### 3.2 Pipeline replay

The exact `ivh_uc_tick()`/`ivh_uc_close()` arithmetic was replayed twice per CPU over the
identical captured series: **(a)** full PV steal, **(b)** PV steal with every sub-1 ms
delta zeroed. Live parameters (`ivh_uc_window_ns=200000000`, `ivh_uc_ema_alpha_q16=868`,
`ivh_uc_min_steal_ns=10000`, `ivh_uc_min_avail_pct=10`, `ivh_uc_used_source=0`) were read
off the running kernel, not assumed.

Steady-state capacity (mean raw pre-EMA window sample, which is what the EMA converges to):

| capture | group | (a) full | (b) sub-tick dropped | delta |
|---|---|---|---|---|
| E hackbench | contended | 485-509 | 533-554 | **+42 to +53** |
| E hackbench | clean | 1020-1021 | 1024 | **+2.7 to +3.2** |
| F saturating | contended | 346-361 | 347-362 | **+0.0 to +0.8** |
| F saturating | clean | 1021-1022 | 1024 | **+2.3 to +2.8** |

The bias is real and in the expected direction (dropping steal raises capacity). It is also
**~8x smaller than the gap between the two populations** (~400-670 points).

### 3.3 Gate verdicts

Published capacities were sampled on a common 10 ms grid across all 16 vCPUs — the same
view the BPF destination scanner has — and all three capacity gates from
`tools/bpf/MY_ivh_atc.bpf.c` evaluated for every (source, destination) pair:
`IVH_CAP_HARDFLOOR = 950`, `IVH_CAP_TOPBAND = 50`, `IVH_CAP_MARGIN = 50` (values read from
the committed source, confirmed present at `6c3874293`).

| capture | verdicts | full-steal accepts | dropped accepts | **changed** |
|---|---|---|---|---|
| E hackbench | 335,280 | 88,062 (26.27%) | 88,022 (26.25%) | **40 (0.0119%)** |
| F saturating | 332,880 | 86,500 (25.99%) | 87,148 (26.18%) | **648 (0.1947%)** |

And every one of those changes is an artefact of the EMA cold start. Excluding the first
5 s (the EMA half-life is 10.4 s, so a 14 s run never fully converges — see 7.2):

| capture | verdicts post-warmup | **changed** |
|---|---|---|
| E hackbench | 215,280 | **0 (0.0000%)** |
| F saturating | 212,880 | **0 (0.0000%)** |

### 3.4 Verdict

**Dropping 100% of sub-tick steal changes zero gate decisions out of 428,160 evaluated,
across both workloads, once the EMA has warmed.** The mechanism is not subtle: capacity is
a heavily smoothed signal feeding gates whose separation is ~400 points, and the sub-tick
bias is worth at most ~53. The blind spot is not a reason to reject a tick-piggybacked
detector, and no higher-frequency heartbeat is warranted to close it. The
self-interference cost of an RT-priority sampling thread would buy nothing here.

This settles Part 1 in favour of implementing the detector.

---

## 4. The design as briefed is wrong: idle, not steal, dominates tick gaps

Applying `(T2-T1) - 0.5*P` to raw TSC tick deltas does not work, and the reason has nothing
to do with sub-tick preemptions.

Under NOHZ an idle vCPU **stops ticking**. An idle gap and a preemption gap therefore have
the *identical* signature: a large raw-TSC distance between consecutive tick deliveries.
Measured distribution of tick deltas in units of nominal period `P`, capture from §1.1:

| bucket | cpu0 (23% stolen) | cpu8 (**0.1% stolen**) |
|---|---|---|
| `<0.9P` | 7.71% | 0.39% |
| `0.9-1.0P` | 20.46% | 42.24% |
| `1.00-1.01P` | 15.28% | 19.66% |
| `1.2-1.5P` | 1.19% | 0.15% |
| `1.5-2P` | 5.16% | 3.22% |
| `2-3P` | 13.74% | 7.08% |
| `3-10P` | 32.77% | 23.73% |
| `>10P` | 3.68% | 3.44% |

**cpu8 has essentially zero steal (0.013 s in 18 s) and yet 34% of its tick intervals
exceed 1.5 nominal ticks.** A detector reading raw TSC gaps alone would report a
mostly-idle vCPU as the most heavily stolen machine in the fleet. This is the dominant
error term by an order of magnitude; the sub-tick question is a rounding error beside it.

Two useful secondary facts from the same table:

- Normal ticks cluster tightly: the `1.01P-1.2P` band is essentially **empty**. There is a
  clean separation between "on time" and "gapped", so a deadband is easy to site.
- Delivery jitter above nominal is inside ~1% of `P` (~10 µs at HZ=1000).

So the estimator must subtract idle first:

```
excess = (T2 - T1) - idle_in_interval - P
```

Every input remains guest-local: raw TSC, the NOHZ idle residency series, and the
compile-time constant `TICK_NSEC`. No steal page, no vPMU.

### 4.1 A circularity trap that was checked and avoided

An appealing variant is `steal = elapsed - idle - kcpustat(USER+NICE+SYSTEM)`. On this
kernel that identity closes to **0.0-0.1%** (verified on capture B, all 16 CPUs), which
looks like an excellent estimator.

**It is circular and was discarded.** `steal_account_process_time()` runs before
`account_process_tick()` and subtracts PV steal from the tick, so `kcpustat` USER/SYSTEM
*already depends on the `kvm_steal_time` page*. The identity closes so beautifully because
both sides are derived from the same host-authored number. In a CVM with no PV steal, that
term changes meaning entirely and the estimator silently loses one tick per
preemption-terminating tick. The shipped form uses the constant `TICK_NSEC`, not
`kcpustat`, and is PV-free by construction.

(`CONFIG_VIRT_CPU_ACCOUNTING_GEN=y` is set but **inactive** — no `nohz_full=` on the
cmdline — so accounting is tick-quantised as this model assumes. Verified from
`/proc/cmdline`.)

### 4.2 Idle source: NOHZ, not kcpustat

Both were captured and tested. `kcpustat` IDLE+IOWAIT is tick-quantised at 1 ms, which
destroys the signal in a workload with sub-millisecond idle episodes:

| idle source | E hackbench ordinal correctness | F saturating ordinal correctness |
|---|---|---|
| NOHZ (`get_cpu_idle_time_us`) | **100.00%** | **100.00%** |
| kcpustat IDLE+IOWAIT | **0.00%** | **0.00%** |

("Ordinal correctness" = fraction of grid samples where every contended vCPU ranks below
every clean vCPU — the same criterion `ivh_uc_gate_recalibration_2026-08-03.md` used.)

kcpustat idle is unusable. NOHZ idle — which `ivh_uc_tick()` already reads — is correct.

---

## 5. The unbiased phase correction is measurably harmful

This is the finding that contradicts the design going in, so it gets its own section.

Per-tick clamping of `excess` at zero rectifies symmetric accounting noise into a positive
mean — the same trap `ivh_ref_carry` exists to avoid in `ivh_ref_accumulate()`. The shipped
form therefore carries a **signed** residual and drains only a strictly positive carry into
the monotonic output. With that in place, sweeping the phase correction `ivh_tks_phase_pct`
(percent of one tick added to a cleared excess), against PV ground truth, on the
**relative** gates (`TOPBAND` + `MARGIN`):

| capture | +0%P | +5%P | +10%P | +25%P | +50%P | +75%P | +100%P |
|---|---|---|---|---|---|---|---|
| E hackbench | 0.094% | 0.072% | **0.051%** | 0.060% | 0.949% | 6.220% | 6.250% |
| F saturating | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% |

(disagreement vs PV truth; lower is better)

**The theoretically unbiased value, +50%, is an order of magnitude worse than 0 in the hard
regime, and +75/100% breaks the signal outright.** The derivation in §2 is not wrong; it is
inapplicable per-event. `+P/2` is only sound on intervals where a preemption genuinely
spanned the deadline, the deadband cannot establish that with certainty, and every false
positive is then inflated by a fixed 500 µs. Averaging over a population where most
intervals are *not* preemptions swamps the correction it was meant to supply.

The deadband is what makes the whole thing viable, and it is not optional. Measured with
the deadband at zero, on the eight **completely unstolen** vCPUs: **27.8 s of phantom steal
booked in a 17 s window.** At a 50 µs deadband the same figure is 0.037 s. The sweep is
flat across 50/200/500 µs — a plateau, not a knife edge — consistent with the empty
`1.01P-1.2P` band in §4.

**Shipped default is `ivh_tks_phase_pct = 0`.** 10% measured marginally better in one
regime and marginally worse in the other; 0 is chosen because it adds no phantom mass at
all, which is the failure mode that actually destroys the signal. The knob exists so the
finding stays falsifiable.

---

## 6. What was implemented

### 6.1 Accuracy of the shipped estimator, exactly as written

Validated in the replay using the **as-implemented** algorithm (bounded signed carry,
monotonic ns output, then the *unmodified* `ivh_uc_tick()` consumption path on top), not
an idealised version of it:

| capture | relative-gate disagreement | ordinal correctness | contended max | clean min |
|---|---|---|---|---|
| E hackbench | **1.51%** | **100.00%** | 936 | 910 |
| F saturating | **0.000%** | **100.00%** | 872 | 977 |

`carry_max` of 1 / 8 / 200 ticks made no material difference (1.58% / 1.51% / 1.51% on E);
8 ships.

#### Correction — the error is not a uniform undercount

An earlier draft of this section claimed the estimator "reads systematically low, recovering
roughly 86% of true steal mass." That figure came from an intermediate variant, and
re-measuring the **shipped** algorithm shows the characterisation was wrong. Retracting it
and replacing it with the actual numbers:

| capture | group | PV truth | shipped estimator | recovery |
|---|---|---|---|---|
| E hackbench | contended | 33.282 s | 20.719 s | **62.3%** |
| E hackbench | clean | 0.113 s | **7.594 s** | **6714%** |
| F saturating | contended | 74.871 s | 61.999 s | **82.8%** |
| F saturating | clean | 0.222 s | 0.003 s | **1.4%** |

The error is **two-sided and regime-dependent**, not a uniform undercount:

- On genuinely stolen vCPUs it reads **low** (62-83%), which is the `(P - s)` phase term
  deliberately not added back.
- On clean vCPUs in the **idle-heavy** regime it reads catastrophically **high** in relative
  terms — 7.594 s of phantom against 0.113 s of truth. In absolute terms that is ~0.95 s per
  vCPU over ~7 s of non-idle availability, i.e. **~13% phantom steal**, which is exactly
  what pushes the clean population down to a minimum of 910 and makes `IVH_CAP_HARDFLOOR`
  the binding gate (§6.2). This is the same ±5% tick-quantisation noise floor noted in §7.2,
  rectified and amplified.
- In the **saturating** regime the phantom essentially vanishes (1.4% of an already tiny
  quantity), which is why that regime shows 0.000% gate disagreement.

Why the gates still work despite this: both errors move the two populations in the *same*
direction relative to each other (contended down-biased, clean down-biased), so the
*ordering* survives at 100.00% and the *relative* gates are unaffected. It is only the
absolute rail that cannot survive, and §6.2 covers that. The honest one-line summary is
**"ordinally reliable, metrically unreliable, and worst on idle-heavy vCPUs"** — not
"systematically low".

### 6.2 The one thing that genuinely does not survive: `IVH_CAP_HARDFLOOR`

Splitting the gates apart:

| capture | phase | **with** `HARDFLOOR` | **relative gates only** |
|---|---|---|---|
| E hackbench | +0%P | **11.48%** | 0.094% |
| E hackbench | +50%P | 26.40% | 0.949% |
| F saturating | +0%P | **0.000%** | 0.000% |

In the saturating regime everything agrees perfectly. In the idle-heavy hackbench regime,
**the entire disagreement comes from the absolute rail**, and it is 100% false-*reject*
(0 false accepts, 24,133 false rejects) — IVH declines migrations it should have made. It
never migrates onto a bad destination. The failure direction is the safe one.

This is exactly the failure `ivh_uc_gate_recalibration_2026-08-03.md` predicted in its own
comment on the constant: *"If `reject_reasons[REJ_CAPACITY_LOW]` ever goes to ~100%, this
has become the binding gate, the reshape has failed, and the answer is recalibration doc
sec 8 (publish steal/elapsed instead) — NOT lowering this number."*

**Lowering the number would not help anyway,** and this is worth stating plainly: under
source 2 in the hackbench regime the contended maximum (936) is *above* the clean minimum
(910). The populations overlap. **No absolute constant on the capacity scale can separate
them in that regime.** The scale origin has moved, which is precisely what the
recalibration doc says a fixed constant cannot survive.

`IVH_CAP_HARDFLOOR` was therefore **deliberately left at 950 and not touched.** Changing it
is a policy decision that also affects the current production `ivh_steal_source=0` path,
the project's own doc says lowering it is the wrong response, and the measurement says
lowering it would not work. This is flagged as required follow-up in §7.1, not silently
patched.

### 6.3 Code

Uncommitted working-tree edits only, per instruction. `git diff --stat`:

```
 kernel/sched/core.c    | 300 +++++++++++++++++++++++++++++++++++++++++---
 kernel/sched/cputime.c |  21 ++++
 kernel/sched/sched.h   |  61 +++++++++
 3 files changed, 367 insertions(+), 15 deletions(-)
```

- **`kernel/sched/sched.h`** — seven `struct rq` fields (`ivh_tks_prev_tsc`,
  `ivh_tks_prev_idle_ns`, `ivh_tks_carry_c` (s64), `ivh_tks_steal_ns` (the monotonic
  output), `ivh_tks_samples`, `ivh_tks_events`, `ivh_tks_skipped`), placed in the `ivh_uc`
  block for the same access-pattern reason the `ivh_ref_*` block gives. Externs for the
  three sysctls, `ivh_tick_steal_accumulate()`, `get_tks_compare()`.
- **`kernel/sched/core.c`** — `ivh_tick_steal_accumulate()`, the estimator; three sysctls
  (`ivh_tks_deadband_ns` = 50000, `ivh_tks_phase_pct` = 0, `ivh_tks_carry_ticks` = 8) with
  a range-checking handler for `phase_pct`; `get_tks_compare()`; `ivh_steal_source`
  validator extended to accept 2; `ivh_uc_steal_ns()` and `get_steal_and_preemptions()`
  converted from a boolean test to a switch.
- **`kernel/sched/cputime.c`** — the call, in `account_process_tick()` immediately after
  `ivh_ref_accumulate()` and **before** `ivh_uc_tick()`, so that at source 2 the value
  `ivh_uc_steal_ns()` reads is this tick's rather than last tick's.

Follows the established `ivh_ref_method` / `ivh_vact_tick` pattern: **produced always,
consumed only when selected.** `ivh_steal_source` still defaults to 0, so nothing changes
behaviourally until it is set to 2. Source 2 deliberately carries **no** precondition
analogous to source 1's `ivh_ref_steal_enabled` check — it needs no perf event and can
never be frozen, which is the entire point.

**No BPF changes were needed or made.** `ivh_cap_of()` reads `rq->ivh_uc_capacity`, which
is steal-source-agnostic. Confirmed `MY_ivh_atc` still builds clean.

### 6.4 Verification performed

- `make kernel/sched/` — **clean, no warnings.**
- `MY_ivh_atc` rebuild — **clean.**
- **Miscompile check.** This project has shipped a `mul_u64_u64_div_u64()` register-aliasing
  bug at exactly `ivh_tsc_ns_to_cycles(TICK_NSEC)`, because `TICK_NSEC == USEC_PER_SEC` at
  HZ=1000 (see `<asm/ivh_tsc_beat.h>`). The new function makes that same call, so the
  disassembly was checked rather than assumed. All four `mul`/`div` pairs in
  `ivh_tick_steal_accumulate()` use distinct registers:

```
ok: mul %r8  / div %r10
ok: mul %r8  / div %r10
ok: mul %r8  / div %r10
ok: mul %rdi / div %r8
```

  The `OPTIMIZER_HIDE_VAR()` in the helper is doing its job. A belt-and-braces
  `tick_c < 1000` sanity floor is also present, matching `ivh_vact_tick()` and
  `ivh_uc_maybe_close_window()`.
- Carry floor overflow: `ivh_tks_carry_ticks` is a plain `proc_doulongvec_minmax` sysctl
  with no range, and `tick_c * ULONG_MAX` would wrap to a *positive* s64 — inverting the
  debt floor into a ceiling. Clamped before the multiply.

---

## 7. What is NOT established

### 7.1 Required before this can be switched on

1. **It has never executed.** Compile-verified and disassembly-verified only. No reboot was
   performed (hard constraint). Every accuracy number in §6.1 comes from replaying the
   shipped arithmetic over live-captured series, **not** from the kernel computing it.
   First live step after reboot: confirm `rq->ivh_tks_steal_ns` advances and tracks the PV
   page, at `ivh_steal_source=0` so nothing consumes it yet:

   ```
   sudo bpftrace -e 'kprobe:ivh_uc_tick {
       $off = *(uint64*)(kaddr("__per_cpu_offset") + cpu * 8);
       $rq  = (struct rq *)(kaddr("runqueues") + $off);
       @tks[cpu] = $rq->ivh_tks_steal_ns; @pv[cpu] = $rq->ivh_uc_prev_steal_ns; }
       interval:s:15 { exit(); }' \
     -c "/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000"
   ```

   Expect `@tks` to read low against `@pv` on the **contended** vCPUs (§6.1: 62-83%
   recovery) and to over-report on the **clean** ones under hackbench. A contended vCPU
   reading *high* means the idle subtraction is not working and is the first thing to check.
2. **`IVH_CAP_HARDFLOOR` must be resolved before `ivh_steal_source=2` is made
   authoritative** (§6.2). Under source 2 the rail becomes the binding gate in idle-heavy
   workloads and no constant value fixes it. The project's own recommended answer is
   recalibration doc sec 8 (publish `steal/elapsed` and make the rail relative). Until then,
   source 2 will under-migrate in that regime — safely, but measurably.
3. **`get_tks_compare()` has no caller.** Shipped uncalled, matching
   `get_vact_compare()`/`get_uc_compare()`. The module needs updating to surface it, or use
   the bpftrace path above.

### 7.2 Caveats on the measurements themselves

- **The EMA never converges within a run.** Half-life is 10.4 s at
  `ivh_uc_ema_alpha_q16=868`; the runs are ~14 s, ~1.3 half-lives. §3.2 therefore reports
  the mean raw pre-EMA window sample as the steady-state proxy, and §3.3 excludes the first
  5 s. This is why the *only* gate flips found in §3.3 were cold-start artefacts. A
  multi-minute run would test the converged regime and has not been done.
- **Two workloads, one host, one boot.** The hackbench/saturating split brackets the idle
  fraction (60% / 20%), which is the variable that actually drives the estimator's error.
  It does not bracket vCPU count, `HZ`, host oversubscription ratio, or hardware. The 50 µs
  deadband in particular is a *measured* property of this guest's delivery jitter and
  should be re-derived on different hardware.
- **The replay reproduces `ivh_uc_tick()` faithfully but is not the kernel.** It was
  cross-checked by the accounting identity `elapsed = idle + steal + used` closing to
  **0.0-0.1%** on the saturating capture across all 16 CPUs, which is strong evidence both
  the capture and the arithmetic model are right. In the hackbench capture the same identity
  leaves a ±5% residual — that is genuine tick-quantisation slop, and it is also the
  estimator's practical noise floor in that regime.
- **`bpftrace` reported no lost events**, but a silently dropped sample would appear as a
  legitimate 2-tick gap and is not otherwise detectable. Per-CPU tick counts were consistent
  with the expected delivery rate given each CPU's steal fraction.

### 7.3 The open question this document does not answer

**Whether the `kvm_steal_time` page is acceptable in the target CVM threat model is
unresolved and is not resolved here.** It is host-authored data. Some CVM designs decline to
trust it even where it is technically exposed; others expose it and accept it. This
implementation is deliberately built so the answer does not matter — `ivh_steal_source=2`
reads neither the vPMU (which TDX/SEV-SNP generally block, the original motivation) nor the
steal page. But the *validation* in this document uses the steal page as ground truth,
which is legitimate here (this guest is a normal KVM guest) and would **not** be available
as a validation channel inside a real CVM.

Practical consequence: **source 2 must be validated against PV ground truth on a
non-confidential guest before being deployed to a confidential one**, because inside the
CVM there is nothing to check it against. That ordering is a requirement, not a preference.

The user needs to confirm with their actual CVM/cloud provider which of the two sources are
exposed and which are trusted. Until that is answered, the correct reading of this work is:
*the PMU dependency is now removable, and the code to remove it exists and compiles.*

---

## 8. Summary

| question | answer |
|---|---|
| Does the sub-tick blind spot change gate decisions? | **No.** 0 of 428,160 post-warmup verdicts, both workloads. |
| Is `(T2-T1) - 0.5*P` on raw TSC a viable detector? | **No** — NOHZ idle produces the identical signature and dominates. Idle must be subtracted first. |
| Is the phase-unbiased `+0.5*P` correction right? | **Derivation confirmed, but it makes the signal worse** (0.05% -> 0.95% gate disagreement). Shipped at 0, knob retained. |
| Does the shipped detector reproduce gate behaviour? | **Yes on relative gates** (0.000% saturating, 1.51% hackbench), 100.00% ordinal in both. But it is **ordinally reliable, metrically unreliable** — up to ~13% phantom steal on idle-heavy clean vCPUs (§6.1). |
| Anything that does not survive? | **`IVH_CAP_HARDFLOOR=950`.** Under source 2 the populations overlap in idle-heavy workloads; no constant separates them. Left untouched, flagged. |
| Is it live-tested? | **No. Compile- and disassembly-verified only. Needs a reboot.** |
| Is PV steal acceptable in the target CVM? | **Still open.** Design avoids depending on it either way. |
