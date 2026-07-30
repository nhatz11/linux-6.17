# TSC-based replacement design for steal-time consumers #2 and #3

Date: 2026-07-25. Branch `kernel-43-clean`, repo
`/home/nick/kernels/linux-6.17-rseqport`. Static analysis only — no code
changes in this pass, no build, no boot.

## Scope (explicit, per correction)

This project's steal-time infrastructure has three call sites that read
host-preemption/steal information. **Consumer #1 —
`ivh_this_cpu_steal_ns()` in `cs_enter()`/`cs_exit()`
(`kernel/locking/spinlock.c`) — is out of scope and untouched.** It backs
`ivh_exec -v`'s HP% output and the Hot-Threads EWMA feed, and stays as
exact host-reported ground truth precisely because it's the yardstick IVH's
own effectiveness is evaluated against.

This document covers only:

- **Consumer #2**: `vcpu_is_preempted(prev->cpu)` in `pv_wait_early()`
  (`kernel/locking/qspinlock_paravirt.h:317`) — the cross-CPU,
  edge-precise "is my MCS predecessor preempted right now" check.
- **Consumer #3**: `get_steal_and_preemptions()`
  (`kernel/sched/core.c:191`) — feeds `/proc/vcap_info`, consumed by
  `vcap` (`/home/nick/vsched_main/vcapacity/main.cpp`) and by
  `NHextend3.c`.

Prior TSC-related notes checked first, per instruction: this doc builds on
(does not duplicate) `tools/bpf/docs/ivh_tpause_ipi_and_tsc_next_steps_2026-07-22.md`
(Tasks 2/3, mostly about `steal_account_process_time()` and `vcap`'s
`capacity_adj`), `tools/bpf/docs/ivh_six_goals_report_2026-07-22.md` §6
(the only prior pass that names `pv_wait_early()` and
`get_steal_and_preemptions()` directly and sketches a TSC-only heuristic
for the former), and `tools/bpf/docs/ivh_halt_ipi_and_tsc_next_steps_2026-07-23.md`
Point 3 (the `rq->clock_preempt`/`is_cpu_preempted()` heartbeat, a
different existing field this doc leans on for consumer #2). Two
corrections to the record are made below (§2.1 precision math not
previously worked out; §3.1 `/proc/vcap_preempted` does not exist as a
separate file).

---

## 1. What each consumer actually reads, confirmed against source

### 1.1 Consumer #2 — `pv_wait_early()`

```c
/* kernel/locking/qspinlock_paravirt.h:263-296 */
static inline bool pv_wait_early(struct pv_node *prev, int loop)
{
    if ((loop & PV_PREV_CHECK_MASK) != 0)   /* PV_PREV_CHECK_MASK = 0xff, line 38 */
        return false;
    if (READ_ONCE(prev->state) != VCPU_RUNNING)
        return true;
    ...
    return vcpu_is_preempted(prev->cpu);
}
```

`vcpu_is_preempted(cpu)` on this KVM guest resolves to
`__kvm_vcpu_is_preempted()` (`arch/x86/kernel/kvm.c:806-830`), which for
an arbitrary **remote** `cpu` does:

```c
struct kvm_steal_time *src = &per_cpu(steal_time, cpu);
return !!(src->preempted & KVM_VCPU_PREEMPTED);
```

This is a single per-CPU memory byte, not a syscall or vmexit — any CPU
can read any other CPU's `steal_time.preempted` byte because it's a plain
percpu structure. The byte itself is **host-written and edge-triggered**:
set the instant the host deschedules that vCPU
(`kvm_steal_time_set_preempted()`, `arch/x86/kvm/x86.c:5079`, called from
`vcpu_put()`) and cleared the instant it's rescheduled back in
(`record_steal_time()`, `x86.c:3674`, called from `vcpu_enter_guest()`).
There is no polling, tick, or accumulation involved — it is the host's
own scheduler decision, mirrored into guest memory at the moment it
happens.

Caller cadence: `pv_wait_node()`'s hot loop runs `SPIN_THRESHOLD` (`1 <<
15` = 32768, `arch/x86/include/asm/spinlock.h:25`) iterations of
`cpu_relax()`, checking `pv_wait_early()` only when `loop & 0xff == 0` —
**128 check points per full spin**, roughly every 256 `PAUSE`s. At the
~50-cycle/PAUSE figure this project's own `tpause_cost.c` measured, that's
a check roughly every **2-4 µs** (at ~3 GHz). This number matters directly
in §2.2.

### 1.2 Consumer #3 — `get_steal_and_preemptions()`

```c
/* kernel/sched/core.c:191-199 */
void get_steal_and_preemptions(int cpunum, u64* preempt, u64* steals_time){
    struct rq *rq = cpu_rq(cpunum);
    *preempt = rq->preemptions;
#ifdef CONFIG_PARAVIRT
    *steals_time = paravirt_steal_clock(cpunum);
#else
    *steals_time = 0;
#endif
}
```

Two structurally different signals bundled into one call:

- `rq->preemptions` — a **local kernel counter**, incremented inside
  `steal_account_process_time()` (`kernel/sched/cputime.c:274`) any time
  `steal > 0` is observed on a tick. This is already downstream of the
  exact mechanism `ivh_tpause_ipi_and_tsc_next_steps_2026-07-22.md`
  Task 2 designed a TSC substitute for (`tsc_steal_heuristic()`,
  that doc §3.2). Nothing new to design here — see §3.2 below.
- `paravirt_steal_clock(cpunum)` — a **live, direct, host-authoritative
  read for an arbitrary remote CPU**, called fresh every time
  `get_steal_and_preemptions()` runs (i.e. every `vcap` poll interval).
  It returns a cumulative ns-since-boot counter; `vcap` diffs successive
  polls (`main.cpp:363`) to get an interval delta. This is the harder
  half — see §3.

Correction to the record: the scope note says this feeds
`/proc/vcap_info`/`/proc/vcap_preempted`. Checked directly
(`/home/nick/vsched_main/vsched_kernel/custom_modules/vsched_module.c:319-347`,
`proc_create("vcap_info", ...)`, line 386): there is **one** proc file,
`/proc/vcap_info`, formatted per-CPU as three lines (`preempt`,
`steal_time`, `max_latency`). `/proc/vcap_preempted` does not exist as a
separate file — "preempted" is the first of the three fields inside
`/proc/vcap_info`. Not a scope change, just a factual correction so the
design below targets the real interface.

---

## 2. Consumer #2 — feasibility verdict: **not viable as a like-for-like replacement**

### 2.1 Two candidate TSC/heartbeat substitutes, both checked against the real code

**(a) Reuse what already exists: `rq->clock_preempt` / `is_cpu_preempted(cpu)`.**
`kernel/sched/cputime.c:288-293` already implements a cross-CPU-readable
preemption detector with no `CONFIG_PARAVIRT` dependency at all:

```c
int is_cpu_preempted(int cpunum)
{
    s64 time_diff = sched_clock() - cpu_rq(cpunum)->clock_preempt;
    return time_diff > 1500000;   /* 1.5 ms */
}
```

`clock_preempt` is written unconditionally every scheduler tick
(`account_process_tick()`, `cputime.c:503`) — a genuine heartbeat, and
already documented in the `2026-07-23` doc as the tree's real
"preemption-staleness detector." It is trivially readable for an
arbitrary remote CPU (`cpu_rq(cpunum)`, same access pattern
`get_steal_and_preemptions()` already uses), so swapping
`vcpu_is_preempted(prev->cpu)` for `is_cpu_preempted(prev->cpu)` at
`qspinlock_paravirt.h:317` is a mechanically trivial one-line change.

**The problem is precision, and it is not close.** §1.1 established
`pv_wait_early()` is checked roughly every 2-4 µs during the hot spin.
`is_cpu_preempted()`'s staleness floor is 1.5 ms — **on the order of
400-750x coarser** than the decision cadence it would be plugged into.
Concretely: a host preemption lasting, say, 200 µs (a completely
ordinary LHP event, well within what this project's own memory notes
describe as the damaging range) would never register as "stale" under a
1.5 ms threshold — `clock_preempt` simply wouldn't have missed enough
ticks yet. `pv_wait_early()` would keep returning `false` throughout the
entire preemption, and the waiter would hot-spin straight through the
exact event the mechanism exists to detect. Lowering the threshold to
match (sub-microsecond) isn't available either: the write frequency is
capped by `HZ` (1000 on this build, i.e. one write per CPU per ms at
best) — you cannot detect staleness finer than the write cadence permits,
so the floor is structural, not just a tuning choice. Making it tighter
would require a **new, higher-frequency heartbeat write** (e.g. on every
`cpu_relax()`-adjacent boundary or a dedicated timer), which reintroduces
per-CPU write overhead this candidate's whole appeal (reuse an existing,
free, once-per-tick write) was meant to avoid.

**(b) The six-goals report's per-waiter elapsed-vs-expected-work heuristic**
(`ivh_six_goals_report_2026-07-22.md` §6.3): record `spin_start_tsc` at
spin entry, compare `rdtsc() - spin_start_tsc` against an
expected-work-cycles baseline (e.g. a decaying average of this lock's
recent hold time), and treat "elapsed exceeds baseline × slack" as "prev
is probably not making progress."

This is not just less precise than (a) — it asks a **different
question**, and that difference is structural, not a tuning gap. The
entire point of `vcpu_is_preempted(prev->cpu)` at this call site is to
**distinguish two cases that look identical from the waiter's own elapsed
time**: (i) `prev` is genuinely still running a longer-than-usual
critical section (correct behavior: keep spinning, it will finish), and
(ii) `prev` has been host-preempted mid-section (correct behavior: back
off/HLT, spinning burns a pCPU the host needs to reschedule `prev`). A
waiter measuring only its own elapsed wait time cannot tell these apart —
both present as "waited longer than expected." That is exactly why the
current code reads a **host-authoritative, `prev`-specific** bit instead
of inferring from local timing: the discriminating information (what is
`prev` actually doing right now) does not exist in the waiter's own TSC
delta at all, no matter how well-calibrated the slack multiplier is. A
TSC-elapsed-only heuristic here would misfire on ordinary lock-hold-time
variance as readily as (or more readily than) it catches real host
preemption — worse than merely noisy, because the false-positive and
true-positive cases are indistinguishable by construction.

### 2.2 Verdict

**Not recommended, for a structural reason, not just a measured
imprecision.** Candidate (a) has the right kind of signal (cross-CPU,
host-driven) but is 2-3 orders of magnitude too coarse at this call
site's actual check cadence, with no cheap way to sharpen it. Candidate
(b) asks a fundamentally different question than the one this check needs
answered and cannot recover the discriminating signal at any calibration.
There is also no performance motivation pulling the other way:
`vcpu_is_preempted()` here costs a single per-CPU memory read (`cmpb`),
not a vmexit or syscall — cheaper than a `rdtsc()`-based heuristic that
still needs its own baseline bookkeeping. The only remaining motivation
for a substitute would be portability (drop the `CONFIG_PARAVIRT`
dependency), and that would mean deliberately trading a working,
cheap, correctly-discriminating signal for one that cannot discriminate
at all. **Recommendation: leave `vcpu_is_preempted(prev->cpu)` exactly as
it is at this call site.** (This does not reopen consumer #1, which was
never a candidate for change either — it's a separate, independent
conclusion arrived at for a different reason: #1 was excluded to protect
an evaluation baseline, #2 is being kept because no substitute actually
works.)

---

## 3. Consumer #3 — feasibility verdict: **buildable, with a real evaluation-integrity cost to weigh**

### 3.1 The `rq->preemptions` half needs no new design

`rq->preemptions` is already exactly the output of
`steal_account_process_time()`'s tick-time observation of
`paravirt_steal_clock()` deltas — the same field the
`2026-07-22` dispatch's `tsc_steal_heuristic()` sketch
(`ivh_tpause_ipi_and_tsc_next_steps_2026-07-22.md` §3.2) already
proposed swapping in for. If that Task-2 substitution is ever made,
`rq->preemptions` continues to be written exactly the same way,
just sourced from the TSC/tick-gap inference instead of an exact
steal-clock read. Nothing new to design for this half; it is inherited
verbatim from that already-written sketch.

### 3.2 The `paravirt_steal_clock(cpunum)` half — the actually-new piece

This is a **live read for an arbitrary remote CPU**, not a locally
accumulated `rq` field, so it can't just inherit Task 2's per-tick
accumulator as-is — that accumulator needs to itself become a persistent,
monotonically non-decreasing, cross-CPU-readable counter that
`get_steal_and_preemptions()` can read at arbitrary poll intervals,
mirroring what `paravirt_steal_clock()` already provides.

Concrete sketch, extending Task 2's fields
(`kernel/sched/sched.h`, alongside `prev_tick_tsc`/`prev_tick_ns`):

```c
/* New, cumulative since boot — mirrors what paravirt_steal_clock()
 * already returns, just TSC/tick-gap-inferred instead of host-reported. */
u64 tsc_inferred_steal_total_ns;
```

`tsc_steal_heuristic()` (Task 2's sketch) already computes a per-call
`steal` delta and feeds it into `account_steal_time()`; the only addition
needed is one more line accumulating that delta:

```c
rq->tsc_inferred_steal_total_ns += steal;
```

`get_steal_and_preemptions()` then becomes:

```c
void get_steal_and_preemptions(int cpunum, u64* preempt, u64* steals_time){
    struct rq *rq = cpu_rq(cpunum);
    *preempt = rq->preemptions;
    *steals_time = rq->tsc_inferred_steal_total_ns;   /* was paravirt_steal_clock(cpunum) */
}
```

**This preserves the exported symbol's signature and `/proc/vcap_info`'s
on-wire format exactly** — `vcap` and `NHextend3.c` need zero code
changes; they keep reading three per-CPU numbers from the same proc file
in the same order. That interface stability is a genuine point in favor
of this design over a rewrite of the userspace side. It also drops the
`CONFIG_PARAVIRT` dependency for this path entirely (the `#ifdef
CONFIG_PARAVIRT` in the current function goes away), which is the one
honest motivation for doing this at all, per the same framing the prior
docs already settled on for Task 2/§6.4 — not simplification, portability.

### 3.3 The tradeoff already priced by prior docs, inherited here unchanged

Everything the `2026-07-22` docs already said about `tsc_steal_heuristic()`
applies verbatim to this use: it trades an exact, host-reported,
already-calibrated ground-truth number for a cheaper-to-read but noisier
inferred one, with a new, unmeasured slack-multiplier tunable
(`ivh_tsc_tick_slack_pct`) replacing a calibration problem
(`IVH_HOT_STEAL_FLOOR_NS`, the EWMA constants) this project has already
solved empirically. Sub-millisecond steal events remain invisible to a
tick-gap-based inference the same way they're invisible to
`steal_account_process_time()`'s own `>1 ms` significance filter today —
this is not a new loss specific to consumer #3, it's inherited from
Task 2's design as-is. Nothing new to add here beyond flagging that it
carries forward unchanged.

### 3.4 The evaluation-integrity tension this scope's own boundary implies — new finding, worth surfacing explicitly

`NHextend3.c` reads `/proc/vcap_info`'s `steal_time` field
(`read_vcap_steal()`, lines 44-70) and its own source comment
(lines 29-34) describes it in these exact words:

```c
/*
 * Host-level steal-time ground truth, read from /proc/vcap_info
 * ... Per-CPU raw cumulative steal ns since boot, driven
 * directly by the KVM steal-time MSR -- independent of guest scheduling
 * entirely, unlike cs_preempted_count below (which only catches
 * guest-internal off-CPU gaps, not real host-level vCPU steals).
 */
```

And its own printed report (`NHextend3.c:980-983`) labels the comparison
explicitly: `"HOST-level steal during hold (real /proc/vcap_info
steal_time delta, ground truth)"` — printed directly alongside a
guest-level proxy signal (`cs_preempted_count`) it exists specifically to
validate against that ground truth.

This is the **same evaluation role the user assigned to consumer #1**,
just showing up one layer removed, at a consumer this scope correction
explicitly places in-scope. If `get_steal_and_preemptions()`'s
`steals_time` output becomes TSC-inferred per §3.2, `/proc/vcap_info` stops
being ground truth, and `NHextend3.c`'s own in-source claim of comparing
its guest-level proxy against a host-authoritative reference becomes
false without any code change to the comment itself — the tool would
silently start comparing one proxy against another. `vcap`'s
`capacity_perc` ratio (`main.cpp:368-370`) inherits the identical
dependency.

This is not a reason to block the design — the user has explicitly scoped
#3 as in-scope for replacement, unlike #1 — but it is a real,
non-obvious cost worth deciding about explicitly rather than letting it
happen as a side effect. Three honest options, not a recommendation to
pick one:

1. Accept it: `NHextend3.c`'s "ground truth" framing becomes inaccurate
   and its comment/report label should be updated to say so.
2. Keep `/proc/vcap_info` on real `paravirt_steal_clock()` (i.e. don't do
   §3.2's swap) specifically so `NHextend3.c` retains a genuine
   host-side reference, even if `rq->preemptions` upstream (§3.1) is
   allowed to move to the TSC heuristic independently — the two halves
   of `get_steal_and_preemptions()` don't have to move together.
3. Add a second, clearly-labeled field/proc file for the TSC-inferred
   number alongside the real one (a debug comparator), which is also
   exactly what the `2026-07-22` doc already recommended doing *before*
   committing any of this to production behavior (§3.4 of that doc) —
   this option satisfies both that recommendation and preserves
   `NHextend3.c`'s ground truth in one move.

### 3.5 Verdict

**Buildable.** Mechanically small (one new per-rq field, one accumulation
line, one call-site swap), preserves the `/proc/vcap_info` wire format so
`vcap`/`NHextend3.c` need no changes to keep running, and its precision/
calibration tradeoffs are already fully priced by the existing Task-2
analysis — nothing novel or worse is introduced by extending that design
to a cross-CPU cumulative counter. The one new, real cost is §3.4's
evaluation-integrity tension, which deserves an explicit decision (one of
the three options above) rather than being absorbed silently. Recommend
option 3 (side-by-side debug comparator) as the first concrete step if
this is pursued, consistent with what the `2026-07-22` doc already
recommended before touching production decision logic, and because it is
the only option that doesn't cost `NHextend3.c` its reference signal
while still producing the portability data point.

---

## 4. Summary table

| Consumer | Verdict | Core reason | If pursued anyway |
|---|---|---|---|
| #2 `vcpu_is_preempted(prev->cpu)` in `pv_wait_early()` | **Not viable — recommend leaving as-is** | No TSC/heartbeat candidate can recover the discriminating signal this check needs (prev-preempted vs. prev's-CS-is-long look identical in elapsed time alone); the existing heartbeat (`is_cpu_preempted`) is 400-750x coarser than this call site's ~2-4 µs check cadence, with a structural (not tunable) floor at `HZ`. Current check is already cheaper (one percpu memory read) than any substitute would be. | N/A — no design sketch offered; this is a negative result. |
| #3 `get_steal_and_preemptions()` | **Buildable** | `rq->preemptions` half is already covered by Task 2's existing sketch; the live cross-CPU `paravirt_steal_clock()` half extends cleanly to a new cumulative per-rq counter, preserving `/proc/vcap_info`'s format exactly. | New field `rq->tsc_inferred_steal_total_ns` (§3.2); real cost is `NHextend3.c` losing its documented "ground truth" reference (§3.4) — decide explicitly among the three options there; recommend a side-by-side debug comparator first, not a direct production swap. |

## 5. Key references used

- `kernel/locking/qspinlock_paravirt.h:38,263-296` — `PV_PREV_CHECK_MASK`, `pv_wait_early()`.
- `arch/x86/include/asm/spinlock.h:25` — `SPIN_THRESHOLD`.
- `arch/x86/kernel/kvm.c:806-830` — `__kvm_vcpu_is_preempted()`.
- `arch/x86/kvm/x86.c:3674,5079` — `record_steal_time()`, `kvm_steal_time_set_preempted()`.
- `kernel/sched/core.c:191-199` — `get_steal_and_preemptions()`.
- `kernel/sched/cputime.c:256-294,503` — `steal_account_process_time()`, `is_cpu_preempted()`, `account_process_tick()`.
- `/home/nick/vsched_main/vsched_kernel/custom_modules/vsched_module.c:319-347,386` — `/proc/vcap_info` proc file, single-file confirmation.
- `/home/nick/vsched_main/vcapacity/main.cpp:211-258,349-428` — `get_cpu_information()`, `get_finalized_data()`, `capacity_perc`.
- `NHextend3.c:29-70,980-983` — `read_vcap_steal()`, "ground truth" framing in both comment and printed report.
- `tools/bpf/docs/ivh_tpause_ipi_and_tsc_next_steps_2026-07-22.md` §3 (Task 2, `tsc_steal_heuristic()` sketch — inherited by §3.1/§3.2 above).
- `tools/bpf/docs/ivh_six_goals_report_2026-07-22.md` §6 (first pass naming both consumers directly; §6.3's per-waiter heuristic evaluated in §2.1(b) above).
- `tools/bpf/docs/ivh_halt_ipi_and_tsc_next_steps_2026-07-23.md` Point 3 (`rq->clock_preempt`/`is_cpu_preempted()`, evaluated as candidate (a) in §2.1 above).
