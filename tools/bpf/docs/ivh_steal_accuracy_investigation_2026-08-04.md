# IVH inferred-steal accuracy: root cause + build plan for a switchable estimator

Date: 2026-08-04
Kernel under test: `6.17.0-rseqport69-byevcap+`, branch `kernel-43-clean`
Guest: 16 vCPU KVM guest on INTEL(R) XEON(R) GOLD 6554S (Sapphire Rapids), `tsc_khz = 2200000`
Mode when measured: `tsc-vcap` (`ivh_steal_source=1`, `ivh_ref_steal_enabled=1`,
`ivh_ref_halt_correct=2`, `ivh_ref_carry=0`, `ivh_pv_preempt_src=2`,
`ivh_cap_source=0`, `ivh_universal_eligible=1`), `vcap` + `MY_ivh_atc` running.
**All measurements below are guest-only. No rebuild, no reboot, no host access.
Config was found and left in exactly the state above.**

---

## 0. The complaint, restated precisely

`/proc/vcap_steal_compare` shows `delta_ppm` (inferred vs `paravirt_steal_clock()`)
of roughly **-3% to +1% on cpu0-7** (host-contended) and **+200% to +380% on
cpu8-15** (host-idle). The stated goal is *not* absolute accuracy — it is that the
**relative** error be in a comparable band on all 16 vCPUs.

Two framing corrections that came out of the measurement and that matter for
where anybody looks next:

1. **Write sparsity is not the cause, and it was never plausible here.**
   `ivh_ref_accumulate()` is called from `account_process_tick()`
   (`kernel/sched/cputime.c:566`) on *every* scheduler tick on every CPU. The
   "written only at lock acquisition / spin iterations / halt return" description
   is true of `ivh_tsc_beat` (feeds `is_wait_preempted()`) and `ivh_cs_beat`
   (feeds `is_cs_preempted()`) — different signals with different write sites.
   Measured tick rates during this investigation: 27-35 samples/s on cpu0-7 and
   45-67 samples/s on cpu8-15 (lower than 1000/s only because NO_HZ_IDLE stops
   the tick during idle, which the estimator handles correctly — see §2.2).

2. **There is no per-CPU divergence in the mechanism at all.** The error is a
   *near-constant additive bias per tick*, essentially the same on all 16 vCPUs.
   It only *looks* like two different populations because it is divided by two
   wildly different denominators.

---

## 1. Root cause (established, reproducible)

### 1.1 The bias is a constant ~5-7 us of phantom steal per tick, on every CPU

90 s window, `/proc/vcap_steal_compare` deltas, no synthetic load:

| cpu | d_real (ns) | d_infer (ns) | err % | samples | **phantom ns/tick** |
|-----|------------:|-------------:|------:|--------:|--------------------:|
| 0 | 1 914 112 868 | 1 902 752 085 | -0.6 | 2836 | -4 010 |
| 3 | 1 787 594 656 | 1 793 876 630 | +0.4 | 2506 | +2 510 |
| 7 | 1 732 327 034 | 1 740 165 774 | +0.5 | 2536 | +3 090 |
| 8 | 6 651 191 | 31 149 915 | **+368** | 4745 | +5 160 |
| 12 | 7 016 999 | 33 840 998 | **+382** | 4438 | +6 040 |
| 15 | 7 647 266 | 33 548 160 | **+339** | 4428 | +5 850 |

The last column is the whole story. ~5 us/tick × 50 ticks/s = ~250 us/s of
phantom steal. Against cpu0-7's ~20 000 us/s of real steal that is +1%; against
cpu8-15's ~75 us/s of real steal it is +350%.

### 1.2 What the ~5 us/tick actually is: REF_TSC does not count host-side VM-exit servicing

Per-tick forensics via `ivh_ref_trace=20`, classified row-wise:

**SHORT intervals (`d_tsc` ≈ 1 ms, i.e. the CPU was ticking, `d_idle_c = 0`,
`d_hlt_c = 0`) — median residual `d_tsc - d_ref - d_idle_c`:**

```
cpu0 5.5us  cpu1 5.4  cpu2 5.2  cpu3 5.5  cpu4 5.2  cpu5 5.0  cpu6 5.0  cpu7 4.9
cpu8 7.8us  cpu9 5.7  cpu10 6.5 cpu11 8.0 cpu12 7.5 cpu13 5.6 cpu14 6.4 cpu15 7.5
```

On a full millisecond in which Linux believes the CPU never entered the idle
loop and never took a lock-path halt, REF_TSC counts only ~993-995 us. The
missing 0.5-0.8% is booked as steal on every CPU, including the eight whose real
steal is 0.01% of wall.

**Discriminating "rate error" from "VM-exit error"** — two pinned workloads,
40 s, `ivh_ref_trace=20`, only `d_idle_c==0 && d_hlt_c==0 && d_tsc≈1 ms` rows:

| cpu | workload | n | median residual | p10 | p90 |
|-----|----------|--:|----------------:|----:|----:|
| 13 | pure userspace FP spin (near-zero vmexits) | 525 | **4.72 us/ms (0.47%)** | 4.44 | 5.12 |
| 12 | `mmap`/`memset`/`munmap` 1 MB loop | 520 | 5.38 us/ms (0.54%) | 5.08 | 6.05 |
| 11 | tight `CPUID` loop (1 unconditional vmexit per iteration) | 525 | **821 us/ms (82.1%)** | 818 | 823 |

The CPUID storm converts 82% of wall time into "steal" while the guest is doing
nothing but executing. That settles it: **`d_tsc - d_ref` contains all host-side
time spent servicing this vCPU's VM exits, which is not steal.** The guest PMU is
saved/disabled across every exit, so REF_TSC stops; the TSC (offset-based) does
not.

### 1.3 The bias is quantitatively predicted by the guest's own interrupt count

Every guest interrupt delivery costs one exit/injection round trip. Two
independent windows, `/proc/interrupts` deltas vs `/proc/vcap_steal_compare`
deltas:

| window | cpu8-15 phantom / total irq |
|--------|-----------------------------|
| 90 s | 2638, 2793, 2881, 2847, 2543, 3057, 2625, 3009 ns |
| 150 s | 2617, 2820, 2861, 3069, 2527, 3074, 2896, 3021 ns |

**~2800 ns of phantom steal per local interrupt, stable to ±10% across CPUs and
reproducible across windows.** A two-term least-squares fit on cpu8-15 separates
the classes:

```
phantom_ns  ≈  1003 ns * n_LOC  +  5335 ns * n_other_irq       (n=8, window 2)
```

which is physically sensible: a timer interrupt is a cheap injection; a
`CALL_FUNCTION` IPI costs the sender an ICR-write exit *and* the receiver a
wake-up.

`CAL` traffic is ~40/s on **every** vCPU. `bpftrace` on
`smp_call_function_many_cond` identifies the source as ordinary userspace TLB
shootdowns (`mi-scavenger`/Bun/`gnome-shell`/`redis-server` →
`do_madvise → tlb_finish_mmu → kvm_flush_tlb_multi → native_flush_tlb_multi`),
i.e. ambient system noise that broadcasts to all CPUs — which is exactly why the
bias is uniform across the 16 vCPUs.

---

## 2. Hypotheses that were tested and did *not* hold

### 2.1 `vcap` is not implicated in the comparator, and only marginally in the bias

`/proc/vcap_steal_compare` (`custom_modules/vsched_module.c`) is fed by
`get_inferred_steal()` and `get_real_steal()` (`kernel/sched/core.c:299` and
`:323`), which read `rq->ivh_ref_steal_ns` and `paravirt_steal_clock()`
respectively. Neither touches `vcap`. `get_real_steal()` exists precisely so the
comparator cannot be routed through the switchable
`get_steal_and_preemptions()`. The pipeline is fully kernel-internal.

`vcap` does contribute *some* IPI traffic (12 `generic_exec_single` calls in a
20 s bpftrace window), but it is a small minority of the ~40 CAL/s per CPU, and
the dominant CAL sources are unrelated userspace processes. **Verdict: not the
cause; a negligible contributor to the bias via the exit-overhead path.**

### 2.2 The idle-boundary mismatch hypothesis is FALSE — the idle subtraction is excellent

This was the leading a-priori hypothesis and the data refutes it cleanly. For
LONG (NOHZ-gap) intervals on cpu8-15, the residual `d_tsc - d_ref - d_idle_c`:

```
cpu8  d_tsc 46 977 us  d_idle 46 863 us  residual  -0.9 us
cpu9  d_tsc 32 977 us  d_idle 32 710 us  residual  -1.3 us
cpu11 d_tsc 62 489 us  d_idle 62 284 us  residual  -2.9 us
cpu13 d_tsc 63 988 us  d_idle 63 864 us  residual  -2.2 us
cpu14 d_tsc 68 990 us  d_idle 68 754 us  residual  -6.1 us
cpu15 d_tsc 63 999 us  d_idle 63 845 us  residual  -3.5 us
```

`get_cpu_idle_time_us() + get_cpu_iowait_time_us()` tracks the hardware halt
boundary to within a few microseconds over 20-70 ms idle episodes — an agreement
of ~0.005%. The residual is slightly **negative** (idle marginally
over-subtracted, as expected because the software idle window opens before the
`HLT` and closes after the wake path), and gets clamped to zero. That clamp is a
small rectifier loss, not the +350%. **100% of the phantom steal comes from
SHORT, actively-ticking intervals.**

### 2.3 The lock-path halt correction is NOT near-zero on the idle CPUs

`rq->ivh_ref_hlt_ns` deltas over a 60 s window: **310-340 ms/60 s on cpu8-15** vs
**97-150 ms/60 s on cpu0-7** — i.e. 2-3x *larger* on the lightly-loaded CPUs
(~0.53% of wall). `rq->ivh_ref_poll_ns` is 0 everywhere, so
`ivh_ref_halt_correct=2` is behaving identically to `=1`. The correction is
already doing real work on cpu8-15: without it, their inferred steal would be
~10.6e9 ns instead of ~2.0e9 ns against a real 0.75e9 ns, i.e. +1300% rather than
+170%. It is load-bearing and correct, just insufficient. The only write sites
are `ivh_pv_wait()` / `pv_wait_early()` in `arch/x86/kernel/kvm.c` — this is
genuine qspinlock-slowpath halting, not mis-attributed idle.

### 2.4 `ivh_ref_carry` was found at 0, not 1; enabling it helps ~26% and is not sufficient

Live A/B, 90 s each, same host contention:

| | cpu0-7 err % | cpu8-15 err % | cpu8-15 phantom ns/tick |
|---|---|---|---|
| `ivh_ref_carry=0` (found state) | -0.6 … +0.5 | +262 … +382 | 4610-6040 |
| `ivh_ref_carry=1` | -1.5 … +0.6 | +190 … +273 | 3370-4550 |

Carrying the residual recovers the clamped-away negative idle residuals from
§2.2 and cuts the per-tick bias by ~26%. It is a real improvement and costs
nothing, but it cannot touch the exit-overhead term, which is strictly positive.
**Restored to 0 after the test.**

### 2.5 cpu14 is not an anomaly in the estimator

In the snapshot that flagged it, cpu14's `real_steal_ns` was ~2x its neighbours
(1.11e9 vs 0.50-0.59e9) — the *numerator* was large, not the inference. Over the
subsequent 60 s, 90 s and 150 s windows cpu14's real steal was the **lowest** of
cpu8-15 (4.89e6 ns/60 s), and its phantom-per-tick (6.82 ns/tick group median
6.4 us SHORT-tick residual) sits mid-pack. Its `ivh_ref_skipped` is 1, same as
every other CPU. **cpu14 accumulated a one-off burst of genuine steal earlier in
the boot, which inflated its denominator and shrank its `delta_ppm`. Nothing
about the mechanism differs there.**

### 2.6 Counter health is fine

`ivh_ref_skipped` is **1 per CPU since boot** (the seeding sample) against
120 000-250 000 samples. Fixed counter 2 is not being multiplexed or stolen.
`ivh_ref_poll_ns` is 0 everywhere. Nothing here is a measurement artifact.

---

## 3. Offline validation of the proposed correction

Apply `infer' = max(0, infer - K_irq * n_irq)` to the raw window data:

**Window 1 (90 s):**

| K (ns/irq) | cpu0-7 err % | cpu8-15 err % | worst \|err\| |
|---:|---|---|---:|
| 0 (today) | -1.0 … +0.5 | +300.0 … +380.7 | 380.7 |
| 2400 | -1.8 … +0.2 | +21.4 … +72.1 | 72.1 |
| **2800** | **-2.0 … +0.2** | **-38.5 … +28.2** | **38.5** |
| 3200 | -2.1 … +0.1 | -98.4 … -15.7 | 98.4 |

**Window 2 (150 s, independent):**

| K (ns/irq) | cpu0-7 err % | cpu8-15 err % | worst \|err\| |
|---:|---|---|---:|
| 2600 | -2.5 … +0.1 | -10.6 … +50.4 | 50.4 |
| **2800** | **-2.6 … +0.1** | **-39.7 … +29.2** | **39.7** |
| 3000 | -2.7 … +0.1 | -68.7 … +7.9 | 68.7 |

The two windows agree to within 1.5 percentage points at K=2800. The **two-term**
model (`1003 ns/LOC + 5335 ns/other`) does better still on window 2:
**cpu0-7 -2.8 … +0.3%, cpu8-15 -19.3 … +27.3%.**

So a single, physically-motivated, self-consistent correction takes the worst
relative error from **+381%** to **~±27%** while leaving the heavily-loaded
population inside ±3%. That is the result this plan is built on.

---

## 4. Build plan for the next kernel

### 4.1 Structure: a new estimator-method knob, three-valued, shadow-first

Do **not** overload `ivh_steal_source` (0 = paravirt, 1 = inferred). Add a
sibling that selects *how* the inferred number is computed, matching the
`ivh_pv_preempt_src` / `ivh_cap_source` idiom:

```
kernel.ivh_ref_method    0 = today's estimator, byte-for-byte (DEFAULT)
                         1 = method 1 + exit-overhead deadband, SHADOW
                             (computed and published to rq->ivh_ref_steal2_ns
                              and /proc/vcap_steal_compare, but
                              get_steal_and_preemptions() still returns
                              rq->ivh_ref_steal_ns)
                         2 = exit-overhead deadband AUTHORITATIVE
                             (rq->ivh_ref_steal_ns becomes the corrected value;
                              raw stays visible in rq->ivh_ref_steal_raw_ns)
```

At `=0` every line of the new code is dead and the validated baseline is
untouched. At `=1` the new method can be A/B'd against real steal for hours
inside one boot before anything downstream consumes it — the same discipline
`ivh_vact_residual` and `ivh_uc_shadow` already use, and the reason the
`ivh_vact_residual` scale-error regression (`kernel/sched/core.c:1201-1228`) was
catchable at all.

### 4.2 Method 1 (`ivh_ref_method >= 1`): exit-overhead deadband

In `ivh_ref_accumulate()`, after `sub_c` is formed and **before** the carry
block, compute a per-interval overhead estimate from guest-visible interrupt
counters and fold it into the same subtraction:

```c
/* new per-rq state: ivh_ref_prev_loc, ivh_ref_prev_oth (u64) */
u64 loc = __this_cpu_read(irq_stat.apic_timer_irqs);
u64 oth = (u64)__this_cpu_read(irq_stat.irq_call_count)
        + __this_cpu_read(irq_stat.irq_resched_count)
        + __this_cpu_read(irq_stat.irq_tlb_count)
        + __this_cpu_read(irq_stat.x86_platform_ipis)
        + __this_cpu_read(irq_stat.irq_spurious_count)
        + __this_cpu_read(irq_stat.__nmi_count);
d_loc = clamped_delta(loc, rq->ivh_ref_prev_loc);
d_oth = clamped_delta(oth, rq->ivh_ref_prev_oth);
ovh_c = mul_u64_u32_div(d_loc * ivh_ref_exit_loc_ns
                      + d_oth * ivh_ref_exit_oth_ns,
                        tsc_khz, USEC_PER_SEC);
sub_c += ovh_c;
```

Notes, all of which are load-bearing:

- Use `<asm/hardirq.h>`'s `irq_stat` fields directly, **not**
  `kstat_cpu_irqs_sum()`. The latter loops over every `irq_desc` and is far too
  expensive for a per-tick hardirq-context hook. Device interrupts are excluded
  by this choice; measured on this guest they are negligible on the CPUs that
  matter (cpu8: 17 202 total irqs / 150 s, of which LOC 10 536 + CAL 6537 +
  RES 60 + TLB 13 = 17 146, i.e. 99.7%). Document that a guest with heavy device
  IRQ affinity on one CPU would need the sum extended.
- `irq_stat` is a per-CPU struct and this is the owning CPU with IRQs off, so no
  locking and no cross-CPU read.
- Both deltas are clamped non-negative for the same reason `d_hlt_c` is: a live
  write of the coefficients must not wrap.
- Publish `rq->ivh_ref_ovh_ns` cumulatively and print it on `/proc/ivh_debug`'s
  `ivh_ref_cpu:` line (append two fields; the format is not consumed by `vcap`,
  only by human/`ivh_exec -v` readers — unlike `/proc/vcap_info`, which is frozen
  at 4 lines and crashed `vcap` with `std::invalid_argument` on 2026-07-13 when a
  5th field was added). Do not change `/proc/vcap_info`.

Two new sysctls, defaults from §1.3:

```
kernel.ivh_ref_exit_loc_ns   default 1000   (ns of host time per timer injection)
kernel.ivh_ref_exit_oth_ns   default 5300   (ns of host time per IPI/other)
```

Setting both to 0 makes method 1 identical to method 0, which is the
kill-switch.

### 4.3 Method 1 must ship with `ivh_ref_carry` defaulted ON

The deadband is strictly positive and the residual after subtracting it goes
negative on idle intervals (§2.2 and §3, where K=3000 already drives cpu8-15
negative). Without the carry, those negatives are rectified away and the
estimator re-acquires a positive bias, defeating the correction. `ivh_ref_carry`
already exists, already carries a floored signed debt, and the live A/B in §2.4
shows it is safe. **Flip its default to 1 in the same build**, and keep the knob
so the 0 case is still A/B-able.

### 4.4 Method 2 (`ivh_ref_method = 3`, optional, structurally cleaner)

If method 1's coefficients prove host-dependent (see §5), the fallback that does
not use REF_TSC at all is **tick-deadline lateness**: the guest tick is armed at
an absolute deadline, so on an executing vCPU consecutive
`account_process_tick()` calls are one `TICK_NSEC` apart and the excess is steal.
This arithmetic already exists in-tree — `ivh_vact_tick()` under
`ivh_vact_residual=1`, with its history and its `OPTIMIZER_HIDE_VAR()` repair
documented at `kernel/sched/core.c:1089-1228`. It is immune to the exit-overhead
bias by construction because it never reads a PMU counter.

Its known limit: resolution is bounded by tick-delivery jitter (tens of us), and
NOHZ intervals need the programmed expiry, not `TICK_NSEC`, as the baseline.
Evidence that this is workable anyway: the real steal on cpu0-7 arrives in
strikingly quantized ~2008 us chunks (LONG-interval residuals measured at 2008.6,
2008.9, 2007.8, 2008.4, 2008.1 us on cpu0/1/2/3/7 — the host scheduler's
preemption granularity), which is two orders of magnitude above the jitter floor.
Specify this as a stretch item; do not block the build on it.

### 4.5 Numbered validation plan (go/no-go)

Run each gate for **>= 30 minutes** under (a) idle and (b) hackbench, with real
host contention confirmed first (cpu0-7 `/proc/stat` steal delta must be
>= 1% of wall while cpu8-15 is <= 0.1%; if that separation is absent the whole
comparison is meaningless — this is the same precondition every measurement in
this project has used).

1. **G1 — no regression at default.** `ivh_ref_method=0`: `/proc/vcap_steal_compare`
   `delta_ppm` and `/proc/ivh_debug` `ivh_ref_*` totals must match this document's
   §1.1 numbers within the run-to-run spread already recorded (cpu0-7 within ±3%,
   cpu8-15 +200…+400%). **FAIL if not: the patch changed the baseline.**
2. **G2 — shadow correctness.** `ivh_ref_method=1`: `rq->ivh_ref_steal_ns` must be
   bit-identical to a `ivh_ref_method=0` run's trajectory (the authoritative path
   is untouched in shadow mode), while `ivh_ref_steal2_ns` diverges.
   **FAIL if the authoritative value moves at all.**
3. **G3 — the accuracy gate, the one this whole exercise is for.**
   `ivh_ref_method=1`, `ivh_ref_carry=1`, over >= 30 min:
   - cpu0-7: `|err%| <= 5%` on every CPU.
   - cpu8-15: `|err%| <= 60%` on every CPU. (Target band from §3, with margin:
     both independent windows landed inside ±40%; 60% is the ship gate.)
   - **Stretch/aspiration:** cpu8-15 inside ±30%, matching §3's two-term fit.
   - **FAIL and do not promote to `=2` if any cpu8-15 CPU exceeds ±100%,** i.e.
     if the correction has not at least cut the error by 4x.
4. **G4 — absolute-floor gate (the honest form of the target, see §5).**
   Phantom rate `(d_infer - d_real)/wall` must be `<= 500 ns/s` (0.05% of wall) on
   every CPU, measured over the 30 min window. Today it is ~250 000-300 000 ns/s
   on cpu8-15. **This is the primary numeric gate; G3 is its consequence.**
5. **G5 — monotonicity, non-negotiable.** `rq->ivh_ref_steal_ns` (and
   `steal2_ns`) must never step backward. Verify by sampling
   `/proc/vcap_steal_compare` at 10 Hz for the full run and asserting
   non-decreasing per CPU. A single backward step underflows `read_vcap_steal()`
   in `NHextend.c` into a multi-exabyte "steal". **FAIL hard.**
6. **G6 — cost.** `ivh_ref_method=1` adds ~8 per-CPU loads and two multiplies per
   tick. hackbench throughput at `ivh_ref_method=1` vs `=0` must be within run
   noise (this project's established bar: within 2%, 5 runs each).
7. **G7 — coefficient robustness.** Sweep `ivh_ref_exit_oth_ns` over
   {4000, 5300, 6500} and `ivh_ref_exit_loc_ns` over {500, 1000, 1500} live and
   record the cpu8-15 band. If the band is flat over that range the calibration
   is robust; if `|err%|` swings by more than 2x, the coefficients are
   host-specific and method 2 (§4.4) should be preferred. Also re-run the
   `CPUID`-loop probe from §1.2 (`tools`-local `vmx.c`, 525 samples on one pinned
   CPU) — with the correction on, a CPUID storm must **no longer** report 82% of
   wall as steal. That is the single sharpest functional test of the fix.
8. **G8 — promotion.** Only after G1-G7 pass: `ivh_ref_method=2` and confirm
   `vcap`'s `cpu_capacity` map and IVH Gate 1 "go" rates do not shift by more
   than the 2026-07-30 `ivh_vact_residual` episode's tolerance
   (`kernel/sched/core.c:1143-1148`). A steal number that suddenly drops by 90%
   on eight vCPUs *will* move Gate 1; that is the intended effect, but it must be
   observed, not assumed.

### 4.6 What is explicitly out of scope for this build

- Do not touch `ivh_this_cpu_steal_ns()` in `cs_enter()`/`cs_exit()`
  (`kernel/locking/spinlock.c`). It stays exact host ground truth
  unconditionally; it is the yardstick IVH is evaluated against.
- Do not change `/proc/vcap_info`'s 4-lines-per-CPU format.
- Do not change `ivh_ref_halt_correct`'s default (2 is correct and load-bearing,
  §2.3).

---

## 5. What guest-side evidence can and cannot settle

**Guest-side fixable.** The dominant term — REF_TSC not counting host-side VM-exit
servicing — is fully characterised from inside the guest (§1.2, §1.3), is
predicted to ~±10% by counters the guest already maintains, and reproduces across
independent windows. §3 shows the correction works offline on real data. Nothing
in §4.2 needs host access to implement or to validate.

**Intrinsically limited, and the user's framing should be adjusted here.** On
cpu8-15 the real steal is ~7-14 ms per 90-150 s window, i.e. ~0.008% of wall.
Reaching ±10% *relative* error there requires per-tick accuracy of ~0.3 us — below
the estimator's own noise floor (the spin-loop p10-p90 spread in §1.2 is already
0.7 us). **A relative-error band as tight as cpu0-7's ±1% is not reachable on a
vCPU with 0.01% steal, by any method, and chasing it will produce over-fitting
rather than accuracy.** The meaningful and achievable target is the **absolute**
phantom floor of G4 (<= 500 ns/s), which automatically yields consistent relative
error on any vCPU whose real steal is above ~0.05% of wall — and which reduces
today's 300 000 ns/s to a 600x smaller number.

**Would need host-side corroboration.**

1. `real_steal_ns` itself has never been independently verified. KVM derives it
   from `current->sched_info.run_delay`, which counts *runnable-but-not-running*
   only. Host-side time spent in the KVM exit handler on this vCPU's behalf is by
   construction **not** in `run_delay` — so the ~2800 ns/irq this document treats
   as "phantom" is, from a capacity-planning point of view, arguably real lost
   vCPU time that the host simply declines to call steal. Deciding which
   definition IVH wants requires the host's own view (`perf kvm stat` /
   `kvm_exit` tracepoints on the host, per-vCPU exit counts and handler
   durations). **If the answer is "the exit overhead is real lost time", then the
   correct fix is not to subtract it from the inferred number but to stop using
   `paravirt_steal_clock()` as the reference — and this whole delta_ppm column is
   measuring the wrong thing.** That question cannot be answered from inside the
   guest and should be put to the host operator before G8 promotion.
2. The residual ±27% spread on cpu8-15 after correction (§3) may be real host
   jitter (which CPU the corunner VM lands next to) rather than estimator error.
   Distinguishing them needs host-side per-pCPU placement data.
3. The strikingly quantized ~2008 us preemption chunks on cpu0-7 (§4.4) look like
   a host scheduler tunable. Confirming that would validate method 2's
   resolution assumption directly.

---

## Appendix: reproduction

All scripts and raw captures used for this document are throwaway; the
measurements are reproducible with the in-tree knobs alone:

```sh
# per-tick forensics (RESTORE TO 0 WHEN DONE)
echo 20 | sudo tee /proc/sys/kernel/ivh_ref_trace
sleep 40; echo 0 | sudo tee /proc/sys/kernel/ivh_ref_trace
sudo dmesg | grep ivh_ref_trace     # d_tsc/d_ref/d_idle_c/d_hlt_c/sub_c/steal_c per tick

# window comparison
cat /proc/vcap_steal_compare > a; cat /proc/interrupts > ia
sleep 90
cat /proc/vcap_steal_compare > b; cat /proc/interrupts > ib
# phantom_per_irq = ((b.infer-a.infer) - (b.real-a.real)) / (ib.total - ia.total)

# the sharpest single probe: CPUID storm on an idle vCPU must NOT read as steal
#   for(;;) __asm__ volatile("cpuid":::"eax","ebx","ecx","edx");
# pinned with taskset; today it reports 82% of wall as steal.
```
