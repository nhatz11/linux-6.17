# IVH / TSC: final state-of-the-project report

**Date:** 2026-08-02
**Tree:** `/home/nick/kernels/linux-6.17-rseqport`, branch `kernel-43-clean`, HEAD `a0bfc3784` (clean)
**Booted kernel:** `6.17.0-rseqport68-TSC-ver2+` (carries the Bug 4 `OPTIMIZER_HIDE_VAR()` repair — confirmed empirically below)
**Scope:** synthesis of the TSC-replacement work, its validation status, its open tunables, its publishability, and a concrete plan for retiring vcap.

---

## 0. How to read this document

Every substantive claim carries an evidence tier. This matters more than usual because parts of this
report may end up underwriting a publication claim.

| Tier | Meaning |
|---|---|
| **[STRUCTURAL]** | Proven by reading the source in this tree, re-verified during this pass. The strongest tier — it cannot be workload-dependent. |
| **[LIVE]** | Read out of `/proc/ivh_debug` or `sysctl` on the currently-booted kernel during this pass. Real measurement, but see the provenance caveat in §0.1. |
| **[REPORTED]** | Measured in an earlier session and recorded in this tree's docs/comments. Not re-run during this pass. Trustworthy but not independently re-confirmed here. |
| **[DERIVED]** | Arithmetic performed during this pass on **[LIVE]** histogram data. As sound as the input. |
| **[BELIEVED]** | A reasoned inference with a stated mechanism, not a measurement. |
| **[UNTESTED]** | Not measured, by anyone, yet. Stated so it is not mistaken for a result. |

### 0.1 Provenance caveat on all [LIVE] counters

Every counter in `/proc/ivh_debug` is **cumulative since boot** and is fed under whatever sysctl
configuration happened to be live at the time. This boot has hosted several configurations, including
the failed vcap-retirement experiment. So the aggregates below mix populations, and **none of them is
a controlled A/B**. They are strong enough to *redirect* effort and to *falsify* specific claims; they
are not strong enough to be published as headline results without a controlled re-measurement. Where a
[LIVE] number materially contradicts a [REPORTED] one, I say so explicitly rather than silently
preferring one.

What the counters *do* have going for them: they are enormous (5×10⁸ heartbeat samples, 2.8×10⁸ CS
checks, 3.4×10⁷ destination-gate evaluations), and two independent derivations agree with each other
to three significant figures (§2.3), which is a real cross-check on the arithmetic.

---

## 1. Current state of TSC, and how well it works versus steal time

### 1.1 What was actually replaced

Three consumers in IVH's decision path formerly depended on KVM steal-time data. All three now have a
TSC-derived alternative, and all three are **currently selected** on the booted kernel.

| # | Consumer | Site | Knob(s) | Live value |
|---|---|---|---|---|
| 1 | Adaptive spinning's "is my MCS predecessor's vCPU preempted" | `is_wait_preempted()`, `kernel/locking/qspinlock_paravirt.h:338` | `ivh_pv_preempt_src` | **2** (TSC authoritative) |
| 2 | Capacity input that vcap consumes and relays into Gate 1 | `get_steal_and_preemptions()`, `kernel/sched/core.c:261` | `ivh_steal_source`, `ivh_ref_halt_correct` | **1**, **2** |
| 3 | Gate 2's preemption-timing signal | `ivh_gate_time_left_reject()`, `kernel/sched/fair.c:13284` | `ivh_preempt_event_source`, `ivh_vact_residual` | **2**, **1** |

All six sysctl values confirmed **[LIVE]** via `sysctl -a | grep ivh`.

### 1.2 Structural proof, per consumer

**Consumer 2 is the cleanest. [STRUCTURAL]** `get_steal_and_preemptions()` (`core.c:261-283`) tests
`READ_ONCE(ivh_steal_source)` at line 264 and, when nonzero, returns `rq->ivh_ref_steal_ns` at line 275
and **returns at line 276** — an unconditional early return that never reaches the
`paravirt_steal_clock()` call at line 279. There is no path through this function at
`ivh_steal_source=1` that touches host-written steal data. This is airtight.

**Consumer 3 is also clean. [STRUCTURAL]** At `ivh_preempt_event_source=2`, `ivh_gate_time_left_reject()`
selects `rq->ivh_vact_last_preempt_tsc`, `rq->ivh_vact_idle_exit_tsc` and `rq->ivh_vact_last_active_c`
(`fair.c:13296`, `13309-13313`). I traced every write site of all three fields: they are written only
from `ivh_vact_tick()` (`core.c:1438-1482`) and `account_idle_time()` (`cputime.c:252`). Both write
`ivh_raw_tsc()` and tick-derived arithmetic exclusively. No steal-derived field is reachable.

**Consumer 1 needs a precision correction that the project's own framing has been eliding.**
`is_wait_preempted()` (`qspinlock_paravirt.h:338-403`) opens with, at line 340:

```c
bool kvm = vcpu_is_preempted(cpu);
```

This is **unconditional** — it executes at `ivh_pv_preempt_src=2` as well. It reads
`__kvm_vcpu_is_preempted()` (`arch/x86/kernel/kvm.c:807`), i.e. the `preempted` field of the
host-written `kvm_steal_time` struct. What is conditional is only the **return**: line 399-402 returns
`beat` (TSC) at `src==2` and `kvm` otherwise.

So the honest statement for consumer 1 is: **the decision is TSC-derived; the steal bit is still read,
and is used only to feed the validation counters and the ROC histograms** (`lines 379-397`). That is
the same category as the deliberately-preserved ground-truth counter in §1.4 — an evaluation input,
not a decision input — but it is *not* the same shape as consumers 2 and 3, which branch away
structurally and never touch the host data at all. **A publication claim must not describe all three
identically.** The correct phrasing is "no steal-time data enters any IVH decision," not "IVH does not
read steal-time data."

Also worth noting for the same reason: `vcpu_is_preempted()` reads the steal struct's `preempted`
**bit**, which is a different mechanism from `kvm_steal_clock()`, the steal **nanosecond counter**. The
prior session's bpftrace validation instrumented `kvm_steal_clock`. That trace therefore establishes
nothing about the `preempted` bit either way — it did not need to, because the bit's only remaining
role is instrumentation, but the two should not be conflated when writing this up.

### 1.3 The dynamic-trace result, and one structural fact that strengthens it

**[REPORTED]** A bpftrace kprobe on `kvm_steal_clock` with full kernel-stack capture, across ~168,000
calls during a full no-opt + IVH hackbench pass, attributed 100% of calls to two expected callers
(idle-loop accounting and `steal_account_process_time()`), and zero to `get_steal_and_preemptions()` or
any IVH decision path.

**[STRUCTURAL] — a new supporting fact found during this pass.** `CONFIG_PARAVIRT_TIME_ACCOUNTING is
not set` in both `.config` and `/boot/config-6.17.0-rseqport68-TSC-ver2+`. That compiles out the
`paravirt_steal_clock()` call in `update_rq_clock_task()` (`core.c:2262`), which is otherwise the third
in-tree caller. So the trace found two callers because **two is all that exists in this build** — the
complete static caller set is `{steal_account_process_time, kvm_steal_clock's own idle path,
get_steal_and_preemptions, get_real_steal}`, and the last two are the switchable/evaluation ones. The
dynamic result and the static caller enumeration now corroborate each other, which is materially
stronger than either alone. This is worth putting in a paper.

### 1.4 What was explicitly NOT eliminated — state it precisely

**[STRUCTURAL]** `ivh_this_cpu_steal_ns()` (declared `include/linux/bpf_sched.h:221`, defined
`arch/x86/kernel/kvm.c:435`, which is a direct `kvm_steal_clock(smp_processor_id())` wrapper) is still
called from `cs_enter()`/`cs_exit()` at `kernel/locking/spinlock.c:423` and `:528`. It is real host
steal time, unconditionally, by explicit early user directive, and it is the yardstick IVH is evaluated
against. `core.c:213-216` documents this as deliberate and says the knob must never touch it.

Consequently:

- ✅ **Accurate:** "No steal-time data is an input to any IVH decision."
- ❌ **False:** "There is no steal time anywhere in the kernel."
- ❌ **False:** "IVH never reads steal time."

The distinction is not pedantry. A reviewer who greps the tree will find `paravirt_steal_clock` in
`spinlock.c`'s call chain within about ninety seconds, and a paper that claimed the strong form would
lose credibility on the spot. The defensible claim is the *decision-path* one, and it happens to be
both true and the interesting one.

### 1.5 The PV spinlock mechanism itself is still active, by design

**[STRUCTURAL]** `kvm_spinlock_init()` (`arch/x86/kernel/kvm.c:2610`) still registers, at lines
2657-2660:

```c
pv_ops.lock.queued_spin_lock_slowpath = __pv_queued_spin_lock_slowpath;
pv_ops.lock.queued_spin_unlock        = ...;
pv_ops.lock.wait                      = ivh_pv_wait;
```

and `pv_ops.lock.vcpu_is_preempted` is registered at line 847. `CONFIG_PARAVIRT_SPINLOCKS=y`.

The claim under construction is about **paravirtualized steal-time data dependency**, not about
paravirtualization as a mechanism. IVH's wait path still executes a real `HLT` that traps to the host,
and still issues `KVM_HC_KICK_CPU` hypercalls. Those are host interactions by construction; a guest
cannot yield a vCPU without one. **Conflating "no PV steal-time data" with "no paravirtualization"
would be a genuine error in a publication claim**, and it is the single easiest thing for a reviewer to
attack. The framing that survives review is: *the guest no longer needs to trust host-written shared
memory to make its scheduling decisions; it still uses architectural trap-based mechanisms to yield and
wake, as any guest must.*

### 1.6 Verdict on §1, with an honest confidence assessment

**What is solid:**

- The structural argument for consumers 2 and 3 is airtight and workload-independent. **[STRUCTURAL]**
- Consumer 1's decision is TSC-derived, with the read-but-unused caveat of §1.2. **[STRUCTURAL]**
- The static caller enumeration for `paravirt_steal_clock` is complete and small. **[STRUCTURAL]**
- The dynamic trace corroborates it across ~168k calls. **[REPORTED]**
- Performance with all three flipped matched historical real-steal performance: IVH 11.66-12.23 s vs
  no-opt 15.43-15.82 s across 4 rounds, and the healthy baseline was reconfirmed at 11.1-11.8 s vs
  15.3-15.6 s after the failed experiment was reverted. **[REPORTED]**

**What is thin, and should be stated as thin:**

1. **One workload.** Every performance number is hackbench. `<asm/ivh_tsc_beat.h>:404-418` already
   says hackbench is a poor instrument for sensitivity because the preempted population arrives in
   bursts (3-second runs put sensitivity anywhere from 0% to 55% for a *fixed* configuration). The
   throughput numbers are stable; the signal-quality numbers are not, on this workload.
2. **One contention pattern.** The live steal distribution is strikingly bimodal: **[LIVE]**
   `ivh_ref_cpu` shows cpu0-7 at ~550-567 s cumulative inferred steal each and cpu8-15 at ~13-15 s —
   a ~40× split. That is one specific corunner placement. A uniform-contention host, or an
   oversubscribed-everywhere host, is a different regime and is untested. **[UNTESTED]**
3. **One boot's TSC drift observation.** **[LIVE]** `ivh_beat_min_age_all: -3528` cycles, per-CPU range
   -3528 to -580. All negative, all small, none drifting positive — which is the healthy signature the
   header at `ivh_tsc_beat.h:97-105` says to look for. But that header explicitly says *"do not declare
   TSC comparability closed until this line has been watched across a multi-hour run,"* and a
   single-boot snapshot is not that.
4. **Single-host.** One physical machine, one TSC implementation, one hypervisor build. **[UNTESTED]**
   elsewhere.

**What would make it more airtight, in descending order of value per unit effort:**

- **A second workload with a genuinely different lock profile.** This is the single highest-value
  missing piece and it recurs in §3 and §4. Something with long, contended, sustained critical
  sections — not hackbench's 128 ns median hold (**[LIVE]** `ivh_obs_cs_time_avg_ns: 185`).
- **A multi-hour run watching `ivh_beat_min_age_percpu`.** Cheap: no rebuild, no reboot, just a
  sampling loop. Closes the drift question, which is currently the most credible technical objection to
  cross-vCPU TSC comparison.
- **A uniform-contention corunner pattern.** Changes the regime that Gate 1 operates in and is the
  configuration under which "all CPUs stolen" — the case `GATE_NOT_BETTER` exists to handle — actually
  occurs.
- **A second host.** Establishes that nothing depends on this machine's TSC/invariant-TSC behaviour.

---

## 2. Tunables introduced with the TSC changes

A note that applies to all four: this project has a real, reusable calibration method already built —
paired age histograms split by host ground truth, at `qspinlock_paravirt.h:379-385` and the CS
equivalent. That method means **two of the four sweeps below can be answered right now, offline, from
data already sitting in `/proc/ivh_debug`, with no rebuild, no reboot, and no benchmark run.** I did
exactly that during this pass, and the results are in §2.1 and §2.3. This is worth internalising: the
instrumentation is better than the use being made of it.

### 2.1 `ivh_pv_beat_threshold` — inherited, never tuned, and the live data says it is badly wrong for this consumer

**Shipped value: 3,300,000 cycles (~1500 µs).** **[STRUCTURAL]** `arch/x86/kernel/kvm.c:1287`, recalibrated
from `tsc_khz` at `late_initcall` (`kvm.c:1632`). The header (`ivh_tsc_beat.h:66-71`) is explicit that
this is the *cycle-equivalent of `is_cpu_preempted()`'s 1500 µs*, chosen so Phase 1 would be a
controlled comparison — **"1500 us was never a calibration"** (`ivh_tsc_beat.h:384`).

**What the live counters say at the shipped value. [LIVE]**

```
ivh_beat_agree_true   41,980        ivh_beat_false_neg   9,101,906
ivh_beat_false_pos    92,436        ivh_beat_agree_false 513,221,172
```

**[DERIVED]** from those four numbers:

- sensitivity = 41,980 / 9,143,886 = **0.459 %**
- FPR = 92,436 / 513,313,608 = **0.0180 %**
- precision = 41,980 / 134,416 = **31.2 %**

**The signal is currently near-blind.** At `ivh_pv_preempt_src=2` — which is live right now —
`is_wait_preempted()` misses **99.5 %** of the preemptions the host reports. `pv_wait_early()`'s
preemption criterion is, for practical purposes, switched off.

**The full ROC, computed during this pass from the live age histograms. [DERIVED]**
(`ivh_beat_age_hist_running[]` / `_preempted[]`, 513,313,608 running and 9,143,886 preempted samples,
prevalence 1.75 %; ~2.2 GHz assumed for the µs column.)

| threshold (cyc) | ~µs | sens % | FPR % | precision % |
|---:|---:|---:|---:|---:|
| 131,072 | 60 | 94.97 | 13.52 | 11.1 |
| 262,144 | 119 | 89.68 | 6.95 | 18.7 |
| 524,288 | 238 | 79.95 | 5.77 | 19.8 |
| 1,048,576 | 477 | 61.58 | 4.01 | 21.5 |
| 2,097,152 | 953 | 28.87 | 1.62 | 24.1 |
| **3,300,000** | **1500** | **0.46** | **0.018** | **31.2** | ← shipped (measured, not interpolated)
| 4,194,304 | 1907 | 0.18 | 0.006 | 35.4 |

**Reading it.** The shipped threshold sits on a cliff: sensitivity falls from 28.9 % at 953 µs to
0.46 % at 1500 µs. It is (near) the *precision*-optimal point, which is presumably why nothing looked
obviously broken — but it buys that precision by almost never firing. Dropping to **2,097,152 cycles
(~953 µs)** buys a **63× sensitivity improvement** (0.46 % → 28.9 %) for 1.6 % FPR and *better*
precision than several lower settings. Dropping to 262,144 (~119 µs) reaches 89.7 % sensitivity at
6.95 % FPR.

**Note the prevalence: 1.75 %, not the 0.134 % quoted elsewhere in this project's notes.** Those are
different populations — 0.134 % was queue-head checks against lock *holders*; 1.75 % is
`pv_wait_early()` checks against MCS *predecessors* — and this boot ran under heavier real contention
than the run that produced 0.134 %. The arithmetic-ceiling argument that constrains `is_cs_preempted`
(§2.4, §3) is **an order of magnitude less binding here**, which is precisely why precision can reach
24-35 % on this consumer.

**What a sweep would need to measure, and what is at stake.** The ROC above is the *statistical* half
and it is already done. The missing half is the *cost asymmetry*, which the ROC cannot supply: at this
call site a false positive means going to `pv_wait()` (HLT + later kick) against a predecessor that was
actually running — wasted latency plus a hypercall — while a false negative means continuing to spin
against a predecessor that is descheduled — wasted cycles, bounded by `SPIN_THRESHOLD`. Those costs are
not symmetric and nobody has measured their ratio. **Until that ratio is known, the ROC alone cannot
name an optimum.** The sweep to run is: `ivh_pv_preempt_src=2`, three or four threshold values spanning
953 µs → 1500 µs, hackbench throughput plus `ivh_pv_wait_calls` and `ivh_lock_steals` at each.
Sysctl-only; no rebuild.

**Stakes:** currently *low but nonzero and unquantified*. IVH beats no-opt by 3-4 s with this consumer
effectively blind, which bounds its contribution from above — but it also means the shipped
configuration has never been shown to be *better* than simply disabling the consumer. **[BELIEVED]** the
upside is modest; **[UNTESTED]** whether it is positive at all.

### 2.2 `ivh_vact_window_ns` / `ivh_vact_jump_threshold` — calibration not done, and the prior numbers are formally void

**Shipped: 100,000,000 ns (100 ms) and 3,300,000 cycles (~1.5 ms).** **[STRUCTURAL]**
`kernel/sched/bpf_sched.c:320` and `:312`; **[LIVE]** both confirmed at their defaults.

The window feeds the tumbling-window capacity at `core.c:1537-1549`:
`capacity = used·1024/(used+stolen)`, recomputed when the window expires and the accumulators reset.

**Critical status flag. [STRUCTURAL]** `core.c:1215-1222` states that the `ivh_vact_residual=1` capacity
figures **and the entire jump-threshold sweep** recorded at `core.c:1165-1168` were measured on builds
that may have carried the Bug 4 miscompile, and instructs: *"Treat those tables as unvalidated until
re-measured on a build carrying the `OPTIMIZER_HIDE_VAR()` repair."* **No such re-measurement has been
done.** So the only existing calibration data for both of these knobs is formally void, and the shipped
values are inherited from a period when the arithmetic underneath them was wrong by six orders of
magnitude.

**What a sweep must measure.** `ivh_vact_window_ns` controls the variance/latency trade of the capacity
estimate: too short and it is noisy (a single descheduling dominates a short window); too long and it
lags the contention it is supposed to detect. The right instrument is *not* throughput — it is the
**separation** of `ivh_vact_capacity` between the known-stolen vCPUs (cpu0-7) and the known-quiet ones
(cpu8-15), which this host hands over for free thanks to the 40× bimodal steal split (§1.6). Sweep the
window across, say, 20/50/100/200/500 ms and record per-CPU `vact_capacity` from
`/proc/ivh_debug`'s `ivh_vact_cpu:` lines; the best window is the one that maximises separation. This
is a **passive** measurement — `ivh_vact_tick()` is deliberately ungated (`core.c:1314-1320`), so the
signal is produced whether or not anything consumes it, and the sweep changes no behaviour.

**Stakes: high, and this is now the critical path.** §6 shows the compression of `ivh_vact_capacity` is
what broke the vcap-retirement attempt. `ivh_vact_window_ns` is the most likely single knob to affect
that compression, and it has never been swept on a correct build.

### 2.3 `ivh_cs_beat_threshold` — swept once against the wrong form; I answered the re-sweep from live data, and the shipped value is already near-optimal

**Shipped: 220,000 cycles (~100 µs).** **[STRUCTURAL]** `arch/x86/kernel/kvm.c:1407`. The header
(`ivh_tsc_beat.h:435-439`) calls it *"a STARTING POINT FOR A SWEEP, not a committed value"* and says the
authoritative calibration is the separation point of `ivh_cs_age_hist_running[]` against
`ivh_cs_age_hist_preempted[]`. The concern in the dispatch brief is correct: the recorded sweep
(`ivh_tsc_beat.h:371-376`) was run against **form 2**, and the default is now **form 0**.

**I ran that calibration during this pass, from the live histograms. [DERIVED]**
(167,864,283 running / 3,511,556 preempted samples; prevalence 2.05 %.)

| threshold (cyc) | ~µs | sens % | FPR % | precision % |
|---:|---:|---:|---:|---:|
| 8,192 | 3.7 | 78.56 | 2.677 | 38.0 |
| 65,536 | 30 | 76.80 | 2.529 | 38.9 |
| 131,072 | 60 | 75.51 | 2.406 | 39.6 |
| **220,000** | **100** | **~73.9** | **~2.24** | **~40.9** | ← shipped
| 262,144 | 119 | 73.05 | 2.168 | 41.4 |
| 524,288 | 238 | 67.45 | 1.908 | **42.5** |
| 1,048,576 | 477 | 56.16 | 1.578 | **42.7** |
| 2,097,152 | 953 | 45.03 | 1.298 | 42.1 |
| 4,194,304 | 1907 | 23.81 | 0.966 | 34.0 |

**Cross-validation, and it is a good one. [LIVE]** `/proc/ivh_debug` independently reports
`ivh_cs_sensitivity_pct: 73.8758%` at the shipped threshold. My histogram-derived interpolation for
220,000 cycles is ~73.9 %. Two independent derivations — a running counter and a post-hoc histogram
integration — agreeing to three significant figures. That validates both the instrumentation and my
arithmetic, and it is the reason I am willing to quote the rest of these tables.

**Answer: the shipped 220,000 is already good.** Precision peaks at 42.7 % around 1,048,576 cycles
versus ~40.9 % at the shipped value — a 1.8-point gain for a 17.7-point sensitivity loss. **There is no
meaningful win available from re-sweeping this knob, and the concern that it was calibrated against the
wrong form turns out to be immaterial.** One item closed.

**A structural ceiling worth recording. [DERIVED]** Sensitivity saturates at **78.57 %** and cannot
exceed it at *any* threshold, because bucket 0 of `ivh_cs_age_hist_preempted[]` holds **752,393
samples — 21.4 % of all preempted samples — with no CS stamp at all** (the `age < 0` sentinel from
`ivh_cs_age()`, `ivh_tsc_beat.h:555-563`). That is exactly the coverage limitation the header predicted
at `ivh_tsc_beat.h:316-326`: the CS stamp is published from `cs_enter()` and therefore covers only
outermost, non-interrupt acquisitions, while the holder-identity table covers every acquisition in the
kernel. **The 21.4 % is now a measured number rather than a predicted mechanism**, and it is the hard
ceiling on form 0. Raising it requires stamping more acquisition sites, not tuning.

### 2.4 `ivh_capacity_threshold` / `IVH_CAP_FLOOR` — calibrated against vcap's scale, with fresh evidence that the mismatch is destructive

**Shipped: `ivh_capacity_threshold = 1010`** (`kernel/sched/bpf_sched.c:23`, **[LIVE]** confirmed 1010) and
**`IVH_CAP_FLOOR = 850`** (`tools/bpf/MY_ivh_atc.bpf.c:289`), mirrored in-kernel as `IVH_BPF_CAP_FLOOR
850UL` (`kernel/sched/fair.c:121`). **[STRUCTURAL]** The mirror is a hand-maintained duplicate — `fair.c:1521-1525`
warns that the BPF program is a separate compilation unit with no shared header and that retuning one
without the other silently makes the comparator model a gate that no longer exists. Verified in sync
during this pass (850 == 850).

Both were calibrated against **vcap's** `rq->cpu_capacity` distribution. §6 documents what happens when
they are pointed at Part C's distribution without recalibration: not an inefficiency, a **measured
performance regression**.

**Live evidence of the scale mismatch, sampled during this pass. [LIVE]** From `/proc/ivh_debug`'s
`ivh_vact_cpu:` lines, on the idle post-experiment system:

| CPU group | cumulative inferred steal | vcap `cpu_capacity` | Part C `vact_capacity` |
|---|---:|---:|---:|
| cpu0-7 (heavy corunner contention) | ~550-567 s each | **442-514** | **1024** |
| cpu8-15 (light) | ~13-15 s each | **1016-1017** | **1024** |

**Part C reports maximum capacity (1024) on all sixteen vCPUs, including the eight carrying ~40× more
steal than their neighbours.** vcap separates the two groups by a factor of ~2. Part C separates them by
zero.

**Caveat, stated because it matters. [BELIEVED]** The system was idle when I sampled. Part C's tumbling
window (`core.c:1537-1549`) reflects only the last 100 ms and resets its accumulators, so at idle it
*should* read high — there is genuinely no steal happening right now. vcap's number is an EWMA and
plausibly retains history from the last benchmark run. So this snapshot **overstates** the compression
relative to what would be seen under load. It is nonetheless consistent in direction with the under-load
observation (892-1024 for Part C against 383-463 for vcap on the same CPUs), and the two together are
enough to establish the mismatch as real. A clean under-load re-sample is the honest way to quantify it.

**What a sweep must measure, and what is at stake.** Not throughput. The correct measurement is the
**ROC of `ivh_vact_capacity` as a classifier** against a vcap-derived "this vCPU is in trouble" label,
from which the correct floor and threshold for the new scale fall out directly. This is the P0b work in
§6, and there is a built-in instrument for it that has never been used properly (§6.2). **Stakes:
this is the entire vcap-retirement question.**

---

## 3. Open contributions to LHP mitigation using TSC

### 3.1 First, a direct correction the user has asked for repeatedly: `is_cs_preempted` is NOT broken

**The lookup mechanism is built, wired, and working. Every part of this is [STRUCTURAL] or [LIVE].**

- The API exists: `ivh_lock_set_holder()` / `ivh_lock_clear_holder()` / `ivh_lock_holder_cpu()`,
  `include/linux/ivh_lock_holder.h:129-143`, workers at `arch/x86/kernel/kvm.c:1513` and `:1545`.
- It is wired at **nine** qspinlock ownership-transfer sites: stamps at
  `include/asm-generic/qspinlock.h:121,151`, `kernel/locking/qspinlock.c:321,501,524`,
  `kernel/locking/qspinlock_paravirt.h:106,1000,1061`, `arch/x86/include/asm/qspinlock.h:247`; clears at
  `asm-generic/qspinlock.h:180`, `qspinlock_paravirt.h:1139,1185`, `arch/x86/include/asm/qspinlock.h:78,128`.
- The consult site exists and calls the lookup: `ivh_cs_head_check()`, `qspinlock_paravirt.h:494`,
  calling `ivh_lock_holder_cpu(lock)` at line 509 (and again at 623 for the race re-verify), invoked
  from the queue-head spin loop at line 1016.
- **[LIVE]** `ivh_holder_stamps: 2,920,625,593` against `ivh_holder_clears: 2,595,980,124` — a ratio of
  **1.125 : 1**. Before the Bug 1 fix this was ~680,000 : 1 (`arch/x86/include/asm/qspinlock.h:92-94`).
  The table tracks.
- **[LIVE]** `ivh_holder_self: 0`, `ivh_holder_unknown_collision: 225,555` against
  `ivh_holder_unknown_empty: 140,078,010` — collisions are 0.16 % of unknowns at
  `ivh_holder_bits=16`, i.e. the table geometry is not a limitation; the residual unknowns are the
  irreducible handoff window.
- **[LIVE]** `ivh_cs_checks: 277,805,964` — the consult site has executed a quarter of a billion times
  and produced a coherent confusion matrix.

**There is no technical blocker. The lookup is a trivial lookup and it works.** The reason
`is_cs_preempted` is not authoritative was a cost-benefit judgement, not a defect — and §3.2 shows that
judgement now needs revisiting.

### 3.2 The cost-benefit judgement against `is_cs_preempted` rests on pre-Bug-1 numbers, and the live data no longer supports it

This is the most consequential finding of this pass, so I want to be careful about how strongly I put it.

The recommendation to keep `is_cs_preempted` in shadow mode rests on three figures recorded in
`<asm/ivh_tsc_beat.h>:337-418`, all measured on **6.17.0-rseqport67**:

| quantity | [REPORTED], rseqport67 | **[LIVE]**, rseqport68-TSC-ver2+ | change |
|---|---:|---:|---:|
| form 0 sensitivity | 34.15 % | **73.88 %** | 2.2× |
| form 0 FPR | 0.203 % | 2.24 % | 11× worse |
| form 0 precision | 18.36 % | **40.9 %** | 2.2× |
| prevalence | 0.134 % | **2.05 %** | 15× |

**rseqport67 predates the Bug 1 fix.** On that kernel the holder-identity table was clearing at
~680,000 : 1, i.e. it was structurally broken — stale slots everywhere. The precision and sensitivity
figures that the "do not make it authoritative" recommendation was derived from were measured *through a
broken holder table*. On the current kernel, with the table tracking at 1.125 : 1, form 0 shows **~41 %
precision at ~74 % sensitivity**.

The argument in the header at `ivh_tsc_beat.h:404-418` was: *at 0.134 % prevalence and 0.2 % FPR, false
positives outnumber true ones ~4:1, so 18 % precision is close to the arithmetic ceiling for ANY
predicate here, and no amount of tuning moves it.* That reasoning is sound — **but its input is a
prevalence that has since been measured 15× higher.** At 2.05 % prevalence the ceiling moves
proportionally, and the measured 41 % precision is exactly what the corrected arithmetic predicts.

**Caveats, and they are real. [LIVE]/§0.1** These are cumulative counters, not a controlled A/B. Some
of the 2.05 % prevalence increase is genuinely a heavier-contention boot rather than the Bug 1 fix.
`ivh_lock_holder_enabled` currently reads 0, so the counters were accumulated during earlier windows in
this boot whose exact configuration I cannot reconstruct. I am **not** claiming form 0 is now good
enough to switch on. I am claiming, and I think this is solidly supported:

> **The recorded cost-benefit conclusion is based on superseded measurements and should be re-derived
> before it is treated as settled.** The re-derivation is one controlled run: `ivh_lock_holder_enabled=1`,
> `ivh_cs_preempt_src=1` (shadow — behaviour unchanged), zero the counters, one hackbench pass, read the
> matrix. **No rebuild, no reboot.**

That is a cheap experiment with a genuinely open outcome, and it is the highest-value single action
available right now.

**What has *not* changed:** the FPR is 11× worse (0.203 % → 2.24 %), and the cost asymmetry of a
false-positive queue-head bail — full hash + halt + kick on both waiter and holder, plus a reopened
lock-stealing window — is unchanged and still unquantified. **[LIVE]** `ivh_head_bail_early: 5,629,255`
and `ivh_lock_steals: 767,964,884` are the counters that would quantify it; the latter is documented as
unconditional so it has a src=0 baseline. So the honest position is "the statistical case improved
substantially; the cost case is still unmeasured," not "switch it on."

### 3.3 (a) A workload with sustained lock-holder preemption — still the missing piece

Unchanged and still the most important gap. **[LIVE]** `ivh_obs_stolen_pct: 0.0011 %` — roughly one
critical section in 90,000 is host-preempted. **[LIVE]** `ivh_obs_cs_time_avg_ns: 185`, and
`ivh_obs_cs_hist` puts the mass at buckets 4-7 (16-255 ns). hackbench simply does not hold locks long
enough, often enough, for LHP to be the dominant effect.

What is needed: contended, *long* critical sections under vCPU oversubscription. Candidates worth
scoping — kernel-side: a `will-it-scale`-style mmap/page-fault contention test, or `dbench` on tmpfs;
userspace-side: the project's existing spinlock workload driven at higher hold times, or a PARSEC
member with known lock-heavy phases (`dedup`, `ferret`). **[UNTESTED]** — none has been run against the
current build.

This gates §4's ability to claim anything about the *mechanism's* benefit rather than its cost, and it
gates evaluating `is_cs_preempted` on its merits. It is the difference between "we removed a
dependency without losing performance" (defensible today) and "we improved LHP mitigation" (not
demonstrated on any workload where LHP matters).

### 3.4 (b) The `ivh_pv_beat_threshold` sweep

Per §2.1. The statistical half is done and is in this document. The remaining work is the cost-asymmetry
measurement and a confirming throughput sweep at 2-4 threshold values. Sysctl-only. **This is a
genuinely publishable micro-result on its own**: "the threshold inherited from the existing in-tree
preemption predicate is 63× off the sensitivity knee for the wait-predicate population, because the two
predicates instrument populations whose stamp-republish rates differ by an order of magnitude." That is
a clean, mechanistic, quantified finding, and the mechanism is already articulated at
`ivh_tsc_beat.h:357-364`.

### 3.5 (c) The two-signal AND-combination, worked through properly

The idea: require **both** `is_wait_preempted()` (heartbeat staleness on the predecessor) **and**
`is_cs_preempted()` (CS-stamp age on the holder) before acting.

**Why it is more attractive than it first looks — the two signals are near-independent by
construction. [STRUCTURAL]** They instrument different populations via different mechanisms:

- `is_wait_preempted()` asks about the **MCS predecessor**, which is *spinning*, so its stamp is
  refreshed by `ivh_beat_publish_in_spin()` at the `ivh_pv_beat_publish_mask` cadence
  (**[LIVE]** 0xfff) — microseconds.
- `is_cs_preempted()` form 0 asks about the **lock holder**, which is *executing inside the critical
  section*, and measures CS-hold *duration* from a stamp written once at `cs_enter()`.

Different subjects, different refresh mechanisms, different failure modes. `ivh_tsc_beat.h:357-364`
identifies exactly this as why forms 1 and 2 fail — and it is the same property that makes an AND of
form 0 with the heartbeat *sound*, because the errors should not be correlated.

**The arithmetic, if independence holds. [DERIVED], and flagged as the weak step.** Taking the live
figures — beat at a retuned 953 µs (sens 28.9 %, FPR 1.62 %) and CS form 0 at shipped (sens 73.9 %,
FPR 2.24 %):

- AND sensitivity ≈ 0.289 × 0.739 = **21.4 %**
- AND FPR ≈ 0.0162 × 0.0224 = **0.036 %**
- at 2.05 % prevalence, precision ≈ (0.0205 × 0.214) / (0.0205 × 0.214 + 0.9795 × 0.00036) = **92.5 %**

A predicate at ~92 % precision changes the calculus entirely, because the false-positive cost that
currently vetoes an authoritative queue-head bail is what the AND is *specifically* suppressing.

**Why this is [BELIEVED] and not [DERIVED-reliable]:** the independence assumption is doing all the
work, and it is not free. Both signals ultimately derive from the same physical event (the host
descheduling a vCPU), so their errors are plausibly positively correlated, which would push the real AND
FPR above the product and the precision below 92 %. **The multiplication above is an optimistic bound,
not a prediction.**

**But it is directly measurable, cheaply, with what already exists.** The two predicates can be
evaluated together in shadow mode (`ivh_pv_preempt_src=1`, `ivh_cs_preempt_src=1`) and their joint
confusion matrix binned. That requires one new 2×2 counter pair — a small kernel patch, but a rebuild.
**Before spending a rebuild:** the existing `ivh_beat_age_hist_*` and `ivh_cs_age_hist_*` pairs are both
already split by host ground truth, so a *lower bound* on correlation can be estimated offline from the
data in hand. Do that first.

I rate this the **most promising genuinely-novel research direction** in the project. It is the one
idea here that could turn `is_cs_preempted` from a shadow curiosity into a working mechanism, it has a
clear mechanistic rationale, and the first validating step is nearly free.

### 3.6 (d) Two further well-scoped items from the actual code state

**The 21.4 % stamp-coverage gap (§2.3) is addressable and is a real contribution.** Form 0's sensitivity
ceiling is set by holders with no CS stamp — inner-lock holders and holders in hardirq/softirq context,
per `ivh_lock_holder.h:29-43`. The holder-identity table already covers those (it is stamped at the
qspinlock layer and catches every acquisition in the kernel); it is only the *CS stamp* that does not.
Publishing a CS stamp from the qspinlock layer as well — or maintaining a per-CPU acquisition-depth
stamp — would close a measured 21.4 % of the preempted population. That is a concrete, bounded,
well-motivated kernel change with a number attached to its payoff.

**The `mul_u64_u64_div_u64()` / `<asm/div64.h>` miscompile is independently publishable. [STRUCTURAL]**
Documented at `arch/x86/include/asm/ivh_tsc_beat.h:182-219`. The `asm("mulq %2; divq %3" : "=a"(q) :
"a"(a), "rm"(mul), "rm"(div) : "rdx")` construct ties `a` to `%rax`, and `mulq` destroys `%rax` before
`divq` reads operand 3 — a fact the constraint language cannot express. When the compiler can *prove*
the `mul` or `div` operand equals `a`, it satisfies both from `%rax` and emits `divq %rax`, dividing a
product by itself. At `CONFIG_HZ=1000`, `TICK_NSEC == USEC_PER_SEC == 1000000`, so GCC coalesced them
and the shipped binary had a nominal tick period of **one cycle**. The header notes a disassembly sweep
of the whole vmlinux found exactly one victim — because equal-valued operands are rare. This is a latent
correctness bug in a core arch header affecting any caller with numerically equal operands. It is worth
an LKML report independent of IVH, and the local `OPTIMIZER_HIDE_VAR()` fix versus the upstream
`div64.h` fix is a discussion the header already frames well (`ivh_tsc_beat.h:215-219`).

---

## 4. Is this publishable as-is?

**The claim under evaluation:** *"IVH's core functions (migration decision, and an in-kernel
capacity/timing mechanism that should no longer need the vcap daemon) are not paravirtualized — the
first in-guest solution to this class of paravirtualization dependency."*

Broken into its three parts:

### 4.1 "IVH's migration decision is not paravirtualized" — **defensible today, with one wording fix**

**[STRUCTURAL] + [REPORTED].** Gate 1's capacity input and Gate 2's timing signal are both confirmed off
real steal at the live settings (§1.2). The static caller set for `paravirt_steal_clock()` is small,
enumerated, and excludes every IVH decision path; `CONFIG_PARAVIRT_TIME_ACCOUNTING` is off, eliminating
the third potential caller; the dynamic trace across ~168k calls corroborates. Performance is
maintained (§1.6).

**Required wording fixes, both from §1:**

1. Say **"no steal-time data is an input to any IVH decision,"** not "IVH does not read steal time."
   `ivh_this_cpu_steal_ns()` at `spinlock.c:423,528` and the shadow read at `qspinlock_paravirt.h:340`
   are both real reads, both evaluation-only, and both trivially greppable by a reviewer.
2. Say **"no paravirtualized steal-time *data* dependency,"** not "not paravirtualized."
   `pv_ops.lock.wait = ivh_pv_wait` is still registered (`kvm.c:2660`) and the wait path still HLTs and
   still issues `KVM_HC_KICK_CPU`. Claiming otherwise is straightforwardly false and is the easiest
   possible target for a reviewer.

With those two fixes this part stands up. **One caveat to disclose rather than hide:** consumer 1 is
currently operating at 0.46 % sensitivity (§2.1), i.e. the TSC substitute for the wait predicate is
near-blind at the shipped threshold. The dependency *is* removed and performance *is* maintained — but a
careful reader will ask whether the dependency was load-bearing in the first place, and the honest
answer is that on hackbench, for this consumer, it largely was not. Better to state that and note the
retuning opportunity than to have it surfaced in review.

### 4.2 "An in-kernel capacity/timing mechanism that no longer needs vcap" — **cannot be claimed today**

This is the part that must change, and the language matters.

The **timing** half is fine: **[STRUCTURAL]** Gate 2 runs on Part C at `ivh_preempt_event_source=2`,
`ivh_vact_residual=1`, both live, with no vcap involvement.

The **capacity** half is not, and this is no longer a "not yet demonstrated" situation:

- **[LIVE]** `ivh_cap_source = 0`. Gate 1's capacity input is `rq->cpu_capacity` — **vcap's field**
  (`ivh_gate_capacity()`, `fair.c:13260-13263`). vcap the daemon is running and is in the loop. What is
  TSC-sourced is the *input vcap consumes*, not vcap's role.
- **[REPORTED]** The direct attempt to remove vcap — BPF gates repointed at `rq->ivh_vact_capacity`,
  `ivh_cap_source=2` — produced a **measured performance regression**: IVH 16.2-16.3 s versus no-opt
  15.4-15.7 s, i.e. IVH went from beating no-opt by ~4 s to *losing* to it, with migrations spiking to
  ~72,700/round against a ~48-53 K healthy baseline.
- **[LIVE]** and this is the important part: **the counters that predicted this were already populated
  before the experiment ran.** See §6.2.

**The claim to make instead:** *"Part C computes a capacity signal entirely in-kernel from raw TSC and
the tick, with no daemon and no host-written data. It is directionally correct following the Bug 4
repair. It has **not** been shown to be a viable replacement for vcap's capacity in IVH's destination
selection; a direct attempt regressed throughput, the root cause (scale compression) is understood, and
the recalibration is scoped but not done."*

That is not merely "untested." It is **attempted, failed, root-caused, and not yet fixed** — and a paper
that said "should no longer need vcap" while this result sat in the project's own logs would be making a
claim its own data contradicts.

### 4.3 "The first in-guest solution to this class of paravirtualization dependency" — **unverifiable from here**

**I cannot assess this from the codebase, and neither can anyone else.** It is a novelty/prior-art claim
and it requires a literature review, which is work entirely outside this tree. Nothing I read during
this pass bears on it either way.

Specific prior art that a reviewer *will* raise and that must be addressed explicitly:

- The extensive **LHP mitigation literature** (preemptable ticket locks, PLE/pause-loop-exiting,
  co-scheduling, gang scheduling, Oracle/IBM/VMware work on spin detection).
- **Existing steal-time-free preemption detection** — some hypervisor-agnostic schedulers already infer
  descheduling from timer/TSC discontinuity. "Detect preemption from a TSC gap" is not itself novel; the
  claim must be sharper than that.
- **`CONFIG_PARAVIRT_TIME_ACCOUNTING` alternatives** and any prior guest-side capacity inference.

**[BELIEVED]** the defensible novelty is narrower and more specific than "first in-guest solution":
something closer to *"a complete removal of steal-time-data dependency from an in-guest LHP-mitigation
scheduler's decision path, with the removal validated both structurally and by dynamic tracing, and with
the resulting signals characterised by ROC against host ground truth."* The **paired-histogram-split-by-
host-truth validation method** (`qspinlock_paravirt.h:365-378`) may itself be the most novel
contribution — it is a clean, reusable technique for validating any guest-side substitute for a
host-provided signal, and I have not seen it framed that way elsewhere. That framing is both more
defensible and, arguably, more interesting.

### 4.4 Overall verdict

**Publishable today, with reframing:**

- Removal of steal-time-data dependency from IVH's migration decision, validated structurally and
  dynamically, at maintained performance on hackbench. **Solid.**
- The validation *methodology* (paired ROC histograms split by host ground truth; the structural +
  dynamic + config-level triangulation of §1.3). **Solid, and possibly the strongest contribution.**
- The `is_wait_preempted` threshold finding (§2.1, §3.4) and the mechanistic explanation of why an
  inherited threshold is 63× off for a different stamp-republish population. **Solid.**
- The `<asm/div64.h>` miscompile (§3.6). **Solid, and independently publishable.**

**Needs work inside this codebase first:**

- The vcap-retirement claim. §6. Currently contradicted by direct measurement.
- Evaluation on a workload where LHP actually occurs. §3.3. Without it, the paper can claim "dependency
  removed at no cost" but not "LHP mitigation improved."
- A controlled re-measurement of `is_cs_preempted` post-Bug-1 (§3.2), which may materially change what
  can be claimed about the mechanism.

**Needs work entirely outside this codebase:**

- The prior-art/novelty review for §4.3. Nothing in the tree can settle it.
- Multi-host and multi-hypervisor validation, if the portability argument is to be load-bearing.
- The TDX applicability argument (§5), if made, needs real TDX hardware.

**One-line honest summary:** *the dependency-removal result is real, well-validated, and publishable; the
daemon-removal result is not, and the difference should be visible in the abstract rather than buried.*

---

## 5. TDX confidential-VM applicability (bonus)

**Everything in this section is [BELIEVED] reasoning.** `CONFIG_INTEL_TDX_GUEST=y` is set in this build
but **this machine is not running under TDX** — it is a KVM guest with `KVM_FEATURE_STEAL_TIME` and
`KVM_FEATURE_PV_UNHALT` advertised. Nothing here has been tested on TDX hardware.

### 5.1 Why the argument is architecturally strong

TDX's threat model treats the hypervisor as **untrusted**. KVM steal time is a `struct kvm_steal_time`
in guest memory that the **host writes**. Under TDX, private memory is encrypted and integrity-protected
against the host, so the steal-time page must live in **shared** (unprotected) memory for the host to
write it at all — meaning it is, by construction, attacker-controlled data under TDX's own threat model.

A guest scheduler that makes decisions from that data is trusting the untrusted party. A malicious or
merely buggy host can report arbitrary steal, and IVH's Gate 1 would migrate threads on command. This is
not a hypothetical objection — it is the direct consequence of the model TDX defines.

**So the argument is not "TSC is nicer than steal time under TDX." It is that the paravirtualized
channel is architecturally inconsistent with TDX's threat model regardless of whether the mechanism
works.** That is a genuinely strong framing and it is the best case for this work's relevance beyond
performance. A guest-side signal derived from the guest's own TSC and its own timer interrupts requires
trusting only the hardware, which under TDX is exactly what the guest *does* trust.

### 5.2 Does TDX give a guest-usable, cross-vCPU-comparable TSC? Being precise

This matters because Part C and the heartbeat both compare TSC values **across vCPUs**
(`ivh_beat_age()`, `ivh_tsc_beat.h:148-153`, reads `per_cpu(ivh_tsc_beat, cpu).stamp` from a remote CPU
and subtracts the local `rdtsc()`).

**What is actually guaranteed under TDX (to my understanding, and this is the part most in need of
verification against the TDX module spec rather than taken from me):**

- TDX requires **invariant TSC** on the platform, and the TDX module virtualises TSC for the TD with a
  per-TD offset and frequency. The TSC is **not** directly host-writable while the TD runs — the TDX
  module controls `TSC_ADJUST`/offset, and guest writes to `IA32_TSC`/`TSC_ADJUST` are restricted.
- The TD sees a **single, consistent TSC frequency** across all its vCPUs, fixed at TD-build time and
  reported through TDX's CPUID virtualisation rather than through the usual (host-controllable)
  calibration path.
- Consequently **cross-vCPU TSC comparison within one TD is on firmer ground under TDX than under
  ordinary KVM**, because the offset is applied per-TD rather than per-vCPU and the host cannot skew one
  vCPU's TSC relative to another's without going through the TDX module.

**[BELIEVED]** this is a *better* environment for the heartbeat design than the current one, not a
worse one. The cross-vCPU drift guard (`ivh_beat_min_age`, `ivh_tsc_beat.h:97-105`) that exists because
ordinary KVM offers weaker guarantees would still be worth keeping, but would be expected to sit flatter.

**What does *not* change:** a vCPU that is not scheduled still does not execute, and its TSC continues to
advance. That is the entire basis of the staleness signal and it is architecture-independent. TDX
changes nothing about it.

### 5.3 The vPMU point — and it is the strongest technical detail in this section

**[STRUCTURAL]** This is worth stating carefully because it is a real design advantage that was arrived
at for unrelated reasons.

- **Consumer 2** (`ivh_ref_accumulate()`, feeding `rq->ivh_ref_steal_ns`) **does** depend on
  `CPU_CLK_UNHALTED.REF` — a **vPMU** counter.
- **Part C** (`ivh_vact_tick()`, `core.c:1322-1550`) **does not**. I verified this during this pass:
  every quantity it computes comes from `ivh_raw_tsc()` (a bare `rdtsc()`, `ivh_tsc_beat.h:221-224`),
  `TICK_NSEC`, and `get_cpu_idle_time_us()`/`get_cpu_iowait_time_us()` via
  `ivh_vact_idle_delta_c()`. **No PMU counter is read anywhere in Part C.**
- The **heartbeat** (`ivh_tsc_beat_publish()`, `ivh_tsc_beat.h:129-132`) is likewise a bare `rdtsc()`.

**Why this matters specifically for confidential computing:** vPMU access is frequently restricted or
disabled in confidential VMs *precisely* to prevent side-channel leakage — performance counters are a
classic side channel, and exposing them to a TD (or exposing a TD's counters to the host) undermines the
isolation the technology exists to provide. TDX in particular has had restricted/absent PMU support.

So: **the design that survives to TDX is the one that was already chosen as the final design.** Part C
plus the raw-TSC heartbeat need only `rdtsc()` and the timer interrupt, both of which a TD has
unconditionally. The REF_TSC-based consumer 2 is the one that would need replacing — and Part C is
already a candidate replacement for it (which is exactly what §6 is about). **The vcap-retirement work
and the TDX-portability story are the same piece of work.** That is a genuinely useful connection and it
strengthens the motivation for §6 considerably: retiring vcap is not just tidiness, it is what makes the
design TDX-viable.

### 5.4 Prediction, and what must be verified on real hardware

**[BELIEVED] prediction:** the heartbeat and Part C should work on TDX, and the architectural argument
for preferring them over steal time is *stronger* there than under ordinary KVM. Consumer 2 as currently
built (REF_TSC/vPMU) would likely need replacing; Part C is the natural replacement.

**Must be verified on real TDX hardware before any of that is trusted — none of it can be settled from
this tree:**

1. **Is `rdtsc()` in a TD trapped or native?** If the TDX module intercepts `rdtsc` (rather than using
   hardware TSC offsetting), the per-spin-loop publish at `ivh_pv_beat_publish_mask` cadence becomes
   catastrophically expensive. This is the single most important thing to check, and it is measurable in
   about ten minutes on real hardware.
2. **Cross-vCPU TSC comparability in practice.** Run the existing `ivh_beat_min_age_percpu` drift guard
   in a TD for hours. The instrument already exists.
3. **Does `KVM_FEATURE_STEAL_TIME` even get advertised to a TD?** If not, the *baseline* IVH cannot run
   under TDX at all — which would make the TSC work not an improvement but a **precondition**. That
   would be a much stronger paper claim, and it is worth checking early because it changes the framing.
4. **Is the vPMU available?** Determines whether consumer 2 survives or must be replaced by Part C.
5. **PV spinlock availability.** Does a TD get `KVM_FEATURE_PV_UNHALT`? `ivh_pv_wait()`'s HLT path and
   `ivh_pv_kick()`'s hypercall are architecturally fine (TDs exit to the host constantly), but the
   *feature advertisement* and its cost profile need confirming — `kvm.c:2241,2503,2539,2595` all branch
   on it.
6. **Tick delivery jitter under TDX.** Part C's core premise (`core.c:1104-1109`) is that consecutive
   `account_process_tick()` calls are one `TICK_NSEC` apart on a vCPU that is executing. TD entry/exit
   overhead is higher than ordinary VM exit; if that inflates tick jitter materially, `ivh_vact_debt_c`'s
   one-tick floor (`core.c:1237`) may be too tight and Part C's accuracy degrades.

**Honest framing for a paper:** this is a well-motivated *hypothesis* with a clear mechanism and a
concrete verification plan, not a result. Present it as future work or as a motivating argument — not as
a demonstrated capability.

---

## 6. What it will actually take to retire vcap

### 6.1 What is true and what is not

**True. [STRUCTURAL]** Part C computes a complete capacity signal (`rq->ivh_vact_capacity`,
`core.c:1537-1549`) entirely in-kernel from raw TSC and the tick, with no daemon and no host-written
data. It is produced unconditionally (`core.c:1314-1320`, deliberately ungated so it can be compared
against what it would replace). Post-Bug-4 it is directionally correct — **[LIVE]** it reads 1024 rather
than the pre-fix 0.

**Not true.** It has never been successfully wired to replace vcap end-to-end. **[LIVE]**
`ivh_cap_source = 0`; Gate 1 reads vcap's `rq->cpu_capacity` (`fair.c:13262`), and the BPF program reads
`select_rq->cpu_capacity` at `MY_ivh_atc.bpf.c:424,434` and `:658` — **verified clean during this pass**,
confirming the experiment's revert.

### 6.2 The decisive finding: the calibration instrument already existed, had already been run, and had already said no

This reframes the experiment substantially, and it is the most useful thing in this section.

**[STRUCTURAL]** `kernel/sched/fair.c:13541-13567` implements a **destination-set agreement comparator**.
It models, per candidate CPU, exactly the two BPF gates that read a capacity number —
`GATE_CAPACITY_LOW` (`cap > IVH_BPF_CAP_FLOOR`) and `GATE_NOT_BETTER` (`cap > source cap`) — **against
both vcap's numbers and Part C's**, and bins the pair into four counters. It sits behind
`ivh_decision_shadow` and changes no behaviour.

Its own stated acceptance criterion, at `fair.c:13529-13532`:

> *"If `pass_tsc_only` and `pass_real_only` are both small relative to `pass_both`, the capacity
> replacement is behaviourally equivalent and `ivh_cap_source=2` is safe; if either is large it is not,
> and the DIRECTION says which way it is biased."*

**[LIVE]** The counters on the booted kernel:

```
ivh_cap_pass_both:       7,549,318   (22.02 %)
ivh_cap_pass_real_only: 10,736,130   (31.31 %)
ivh_cap_pass_tsc_only:   5,014,544   (14.63 %)
ivh_cap_pass_neither:   10,986,678   (32.04 %)
```

**[DERIVED]:**

- `pass_real_only / pass_both` = **1.42×** — not small. **Criterion failed.**
- `pass_tsc_only / pass_both` = **0.66×** — not small. **Criterion failed.**
- Jaccard(real, tsc) = **32.4 %** — the two destination sets agree on under a third of their union.
- Part C **misses 58.7 %** of the destinations vcap accepts, and accepts 5.0 M that vcap rejects.

And the Gate 1+2 decision comparator (`fair.c:13375-13383`), same boot:

```
ivh_dec_agree_go: 842,972   ivh_dec_tsc_only_go: 1,764,872   ivh_dec_real_only_go: 1,624,552
```

**[DERIVED]** Jaccard of the "go" sets = **19.9 %**, and Part C **over-triggers** by 140,320 net —
reversed in sign from the 60 % *under*-trigger recorded pre-Bug-4 at `core.c:1077-1082`.

**The conclusion is uncomfortable but valuable:** the kernel's own purpose-built acceptance test for this
exact flip had already been run on this exact boot, and had already returned a decisive **no** — by a
wide margin, on both criteria, with the direction of bias correctly indicated. The regression was
predictable from data already sitting in `/proc/ivh_debug` before the experiment started. **The problem
was not that calibration was skipped for lack of a tool; the tool exists, is well-designed, was
populated, and was not read.**

**Caveat, per §0.1. [LIVE]** These are cumulative and their exact accumulation window is unknown. But
they are post-Bug-4 (this boot has the fix, confirmed by `vact_capacity` reading 1024 rather than 0),
they total 34.3 M evaluations, and the effect size is far too large to be a provenance artefact.

### 6.3 Root cause, and why it produced *more* migrations rather than fewer

**[LIVE]** The compression is direct: on the eight vCPUs carrying ~40× the steal of their neighbours,
vcap reads 442-514 while Part C reads 1024 (§2.4; under load, 383-463 versus 892-1024).

**[BELIEVED], mechanism traced through the actual gate code.** `GATE_NOT_BETTER`
(`MY_ivh_atc.bpf.c:430-437`) rejects a destination unless `select_rq->cpu_capacity > ctx->source_capacity`
— a **relative** comparison. Its behaviour depends entirely on there being meaningful separation between
CPUs. On a scale where nearly every CPU is pinned near the 1024 ceiling:

- a source that dips even slightly below the ceiling finds **many** destinations nominally "strictly
  better," because they are all sitting at 1024;
- those destinations are at 1024 not because they are healthy but because **Part C cannot see their
  steal**;
- so more destinations pass, more migrations complete, and they complete *onto genuinely-stolen vCPUs*.

That is precisely the observed signature: **[REPORTED]** ~72,700 migrations/round against a ~48-53 K
baseline, with throughput regressing. `pass_tsc_only = 5.0 M` — destinations Part C accepts and vcap
rejects — is the direct counter-level fingerprint of it.

**A structural reason to expect the compression, which makes this more than an empirical observation.
[STRUCTURAL]** The two capacity numbers are computed by **fundamentally different estimators of the same
quantity**:

- **vcap:** `used / (used + stolen)` where `stolen` is a **continuously accumulated** steal quantity.
- **Part C:** `used·1024 / (used + stolen)` (`core.c:1541-1543`) where `stolen` comes from
  `ivh_vact_gap_split()` (`core.c:1225-1239`) and is a **thresholded excess** — steal is only booked
  when `avail - tick_c + debt > 0`, i.e. only when an inter-tick gap exceeds one nominal tick period
  after idle removal, with a debt carry floored at `-tick_c`.

A thresholded-excess estimator systematically under-reports steal that is finely distributed relative to
the tick period — which is exactly the regime hackbench creates. **So the compression is a predictable
consequence of the estimator's shape, not a tuning accident.** That distinction is what drives the
scoping in §6.5.

### 6.4 The fourth flip — confirmed the hard way

**[REPORTED] + [STRUCTURAL].** Reverting `ivh_cap_source` to 0 did **not** undo the regression, because
the BPF program's field selection is a compile-time source edit
(`MY_ivh_atc.bpf.c:424,434,658`) and BPF compiles independently of kernel sysctls. There is no sysctl
that reaches it.

So there are **four** authoritative flips, not three, and the BPF-side one:

- has **no runtime kill switch** — reverting it requires editing, `clang -target bpf`, and reload;
- is **not covered by any of the three-valued `0/1/2` shadow-mode idioms** the kernel side uses so
  carefully (`ivh_pv_preempt_src`, `ivh_cs_preempt_src`, `ivh_cap_source`, …);
- needs its **own** acceptance process.

**Recommendation. [BELIEVED]** Before the next attempt, add a BPF-map-backed source selector so the BPF
side gains the same 0/1/2 shadow semantics as the kernel side. This is a small change to the BPF program
plus a userspace map write — **no kernel rebuild** — and it converts the riskiest flip in the system from
"edit, recompile, reload, hope" into "write a map key." Given that this flip has now cost one regression
and one confusing revert, the change pays for itself immediately.

**Also unmodelled, and worth knowing before the next attempt. [STRUCTURAL]** The kernel comparator models
only the two gates in `process_cpu()` (`MY_ivh_atc.bpf.c:304-507`). It does **not** model:

- `ctx->average_capacity`, which comes from `average_capacity_all` (`core.c:197`), **written by vcap**
  via `set_average_capacity_all()` and passed to BPF at `fair.c:13600`. Even with every `cpu_capacity`
  read repointed at Part C, **this comparator baseline would still be vcap's number, on vcap's scale** —
  a latent scale-mixing bug in any partial migration.
- The `cap_sum`/`cap_cnt` instrumentation at `MY_ivh_atc.bpf.c:342`.
- The separate `sched/cfs_latency_select` program (`test32`, `MY_ivh_atc.bpf.c:777-819`), which reads
  `cpu_capacity` at lines 741, 749 and 765. Different hook, not the IVH migration path, untouched by the
  experiment — but a *full* vcap retirement has to account for it.

### 6.5 Scoped plan

**P0a — read what is already there (hours, zero risk).** Enable `ivh_decision_shadow=1`, zero the
counters, run one clean hackbench pass under the corunner pattern, and read `ivh_cap_pass_*` and
`ivh_dec_*`. This produces a *provenance-clean* version of §6.2's numbers. **Do this before anything
else** — it costs nothing and it re-establishes the baseline on a known configuration.

**P0b — the calibration that was skipped (days).** Build the ROC of `ivh_vact_capacity` as a classifier
against a vcap-derived "in trouble" label, exactly as §2.4 describes. Then:

1. Retune `IVH_CAP_FLOOR` and `ivh_capacity_threshold` for Part C's *actual* distribution — noting that
   `IVH_CAP_FLOOR` must be changed in **two** places (`MY_ivh_atc.bpf.c:289` and `fair.c:121`) or the
   comparator silently models a gate that no longer exists.
2. Model the **destination-set-empty rate** offline. If retuning the floor upward to compensate for
   compression empties the destination set too often, IVH stops migrating at all — a different failure
   from the storm, and one the counters can predict before a single live run.
3. Sweep `ivh_vact_window_ns` (§2.2) as the primary decompression lever, scoring on cpu0-7 versus
   cpu8-15 separation rather than throughput.

All of this is offline/passive. **No kernel rebuild.**

**P1 — the `GATE_NOT_BETTER` decision (a live decision point, still unsettled).** The gate exists to stop
lateral migrations to equally-throttled vCPUs (`MY_ivh_atc.bpf.c:431-433`). Its premise is meaningful
inter-CPU separation. Three options:

- **Keep it** — only viable if P0b restores separation.
- **Drop it** — but `MY_ivh_atc.bpf.c:287` already records a case where it was judged *"provably
  redundant and left off,"* so its value is not established for all configurations anyway. Dropping it
  makes `GATE_CAPACITY_LOW`'s **absolute** threshold the sole capacity gate, and §6.3's analysis says an
  absolute comparison degrades far more gracefully under compression than a relative one.
- **Replace it with a margin** — `dest > source + δ` — which is a middle path that suppresses tie-driven
  churn without requiring wide separation. **[UNTESTED]**, but cheap to model offline with the P0b data.

**[BELIEVED]** the absolute/relative distinction is the crux. Gate 1's absolute threshold was already
known to be closer to workable than the relative destination comparison, and §6.3 explains mechanically
why: an absolute threshold degrades to "always pass" or "always fail" under compression, while a relative
one degrades to *arbitrary* — and arbitrary is what produces a migration storm.

**P2 — the BPF-side source selector (§6.4).** Small, no kernel rebuild, removes the sharpest edge.

**P3 — only then, live.** With floors retuned, window swept, `GATE_NOT_BETTER` decided, and a runtime
kill switch in place: `ivh_cap_source=2` plus the BPF map flip, one round, watching migration count as
the leading indicator. **Migration count is the canary** — it moved 1.4× before throughput moved, and it
is visible in `ivh_migrations_done` without finishing a benchmark.

### 6.6 Honest estimate: is this "some tuning away" or a real open research question?

**Neither, cleanly — and the split is the answer.**

**The threshold retuning is tuning.** P0b and P1 are a few days of offline work with instruments that
already exist, and they will very likely produce *something* that runs without regressing. **[BELIEVED]**
confidence: high.

**Getting a *relative* comparator to work on Part C's signal is a real open question.** The evidence:

1. Part C reads 1024 on vCPUs carrying 566 s of cumulative steal (**[LIVE]**) — that is not a threshold
   being slightly off, it is **zero discriminating information** between the two groups at that instant.
2. The compression has a **structural cause** (§6.3): thresholded-excess versus continuous accumulation.
   Threshold retuning does not change an estimator's dynamic range — it only moves where you cut it. If
   the signal genuinely lacks separation, no floor value creates it.
3. The one knob that *could* change the dynamic range — `ivh_vact_window_ns` — has never been swept on a
   correct build, and its prior calibration data is formally void (`core.c:1215-1222`). So the cheapest
   possible fix is genuinely untried, which is the main reason for optimism.

**[BELIEVED]** the likely outcome, in descending probability:

- **Most likely:** Part C's capacity works as an **absolute** gate (Gate 1, `GATE_CAPACITY_LOW`) after
  P0b retuning, and `GATE_NOT_BETTER` must be dropped or margin-replaced. Partial vcap retirement:
  Gate 1 and the destination floor go in-kernel; the relative comparison does not survive in its current
  form.
- **Plausible:** a wider `ivh_vact_window_ns` plus removing the sub-tick threshold (accumulating
  fractional excess rather than gating on `avail > tick_c`) restores enough dynamic range for the
  relative comparator. This is a **Part C arithmetic change**, i.e. a kernel rebuild, and it is the
  scenario where full retirement becomes possible.
- **Cannot be excluded:** the tick-sampled estimator is fundamentally too coarse to resolve
  hackbench-scale steal distribution, and full retirement needs a different aggregation entirely.
  **[UNTESTED]** — and note this would be a *finding*, not a failure: "a tick-rate in-guest estimator
  cannot resolve the steal granularity that a continuously-sampled daemon can" is a publishable negative
  result with a clear mechanism.

**Bottom line for the paper:** do not claim vcap retirement. Claim the migration *decision*'s
independence from steal-time data (§4.1), which is solid, and present Part C as a validated in-kernel
*timing* mechanism (Gate 2, which genuinely works today) plus an in-kernel capacity signal whose
integration is characterised, root-caused, and **in progress**. That is an honest, complete, and still
interesting story — and §5.3 gives it a forward-looking motivation (TDX viability) that does not depend
on the retirement being finished.

---

## 7. Corrections to the briefing, found by checking against the tree

Recorded because the instruction was that the tree wins.

1. **"All three consumers branch unconditionally away from real steal."** Consumers 2 and 3 do
   (**[STRUCTURAL]**). Consumer 1 does **not** — `is_wait_preempted()` reads `vcpu_is_preempted()`
   unconditionally at `qspinlock_paravirt.h:340` and only the *return* is conditional. Decision-path
   claim holds; "does not touch" does not. §1.2.
2. **`is_cs_preempted` precision "~18-25 %" and prevalence "~0.134 %".** Those are **[REPORTED]**
   rseqport67 figures, measured through a broken holder table (pre-Bug-1). **[LIVE]** on the current
   kernel: sensitivity 73.88 %, precision ~41 %, prevalence 2.05 %. The cost-benefit conclusion needs
   re-deriving. §3.2.
3. **`ivh_cs_beat_threshold` "swept once but only against form 2, not re-verified against form 0."**
   True as history, but I ran the form-0 calibration from live histograms during this pass: the shipped
   220,000 cycles is already within ~2 precision points of optimal. Item closed. §2.3.
4. **The P0b calibration was "skipped in favour of speed."** More precisely: the calibration
   *instrument* (`fair.c:13541-13567`) already existed, was already populated on this boot, and had
   already failed its own stated acceptance criterion by 1.42× and 0.66×. The data was there and was not
   read. §6.2.
5. **`<asm/ivh_tsc_beat.h>:284-296` still describes form 1 as "the default, better founded."** Stale —
   the 2026-07-30 measurement block below it (line 337 onward) and `kvm.c:1344` both make form 0 the
   default. **[LIVE]** `ivh_cs_predicate_form: 0`. Comment-only inconsistency; worth a cleanup pass so a
   future reader does not trust the first paragraph.
6. **The bpftrace trace found "two expected callers."** Supported and strengthened: **[STRUCTURAL]**
   `CONFIG_PARAVIRT_TIME_ACCOUNTING` is not set, so `update_rq_clock_task()`'s `paravirt_steal_clock()`
   (`core.c:2262`) is compiled out. Two callers is the complete static set for this build, not merely
   what the trace happened to catch. §1.3.

---

## 8. Recommended next actions, ranked by value per unit effort

| # | Action | Cost | Rebuild? | Value |
|---|---|---|---|---|
| 1 | Controlled re-measure of `is_cs_preempted` post-Bug-1 (`ivh_lock_holder_enabled=1`, `ivh_cs_preempt_src=1`, one pass) | hours | no | May reopen the whole predicate question (§3.2) |
| 2 | P0a: clean `ivh_decision_shadow=1` run for provenance-clean `ivh_cap_pass_*` | hours | no | Baseline for all of §6 |
| 3 | `ivh_pv_beat_threshold` throughput sweep at 953 µs / 1500 µs | hours | no | 63× sensitivity available; publishable micro-result (§2.1) |
| 4 | Multi-hour `ivh_beat_min_age_percpu` watch | passive | no | Closes the strongest technical objection to cross-vCPU TSC (§1.6) |
| 5 | P0b: `ivh_vact_capacity` ROC + `ivh_vact_window_ns` sweep | days | no | The vcap-retirement critical path (§6.5) |
| 6 | Offline correlation estimate for the AND-combination | days | no | Gates the best novel idea in the project (§3.5) |
| 7 | BPF-side source selector (map-backed 0/1/2) | days | no (BPF only) | Removes the sharpest operational edge (§6.4) |
| 8 | Find and run a sustained-LHP workload | weeks | no | Gates every mechanism-benefit claim (§3.3, §4.4) |
| 9 | Prior-art / novelty literature review | weeks | n/a | Gates the §4.3 claim entirely |
| 10 | LKML report for the `<asm/div64.h>` miscompile | days | n/a | Independent contribution (§3.6) |

Note that **items 1-7 require no kernel rebuild.** The instrumentation built into this tree is
substantially better than the use currently being made of it, and the highest-value next steps are
almost all reads of data the kernel is already producing.
