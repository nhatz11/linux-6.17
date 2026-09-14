# Two proposed enhancements to IVH adaptive spinning: design plans and hypotheses, 2026-09-04

Status: **design-stage only, nothing implemented, nothing tested.** Produced by two parallel Opus
research passes over this tree (`kernel/locking/qspinlock*.c`, `arch/x86/kernel/kvm.c`,
`arch/x86/include/asm/ivh_tsc_beat.h`, `kernel/sched/core.c`, `kernel/sched/fair.c`), each doing
read-only codebase research and asked to (a) map current behavior precisely with file:line
citations, (b) produce a concrete implementation plan, (c) do an honest correctness/risk pass, and
(d) give a falsifiable hypothesis tied to this session's own established root-cause findings for
IVH's persistent ~5-8% wall-clock regression vs stock PV spinlock (see
`ivh_adaptive_spinning_glock13_findings_2026-09-03.md`): mechanism 2's wake vehicle costs ~2.6-2.9x
more per real halt than stock's hypercall wake, and mechanism 2 halts 2.3-3.2x more often than
stock for equivalent work.

Both proposals originated from the user's own hypotheses, prompted specifically to have agents
verify/correct the reasoning against source, not just take it at face value.

---

## Idea 1: per-vCPU eager preemption for migration targets

**User's framing**: vCPUs currently hosting a task IVH proactively migrated there should run in
`voluntary` preemption mode for as long as they host it — believed to help because "the lock is
passed around quicker" — while the system stays on the modern `lazy` default otherwise.

### What actually differs between `lazy` and `voluntary`

Not what the hypothesis assumed. `PREEMPT_DYNAMIC` mode is a static-call/static-key table,
`kernel/sched/core.c:10819` (`__sched_dynamic_update()`). The two modes differ in:

| | `preempt_schedule` | `dynamic_preempt_lazy` |
|---|---|---|
| voluntary | **off** | false |
| lazy | **on** | true |

`dynamic_preempt_lazy` is consumed in exactly two places: `get_lazy_tif_bit()` (`core.c:4152-4158`)
and `sched_tick()`'s once-per-tick promotion of a lazy bit to a real one (`core.c:8731`). The
consequence, in `__resched_curr()` (`core.c:4093`): under `lazy`, a preemption request against a
*busy, remote* CPU sets a flag and sends **no IPI** (`core.c:4121-4123`) — the target only notices
at its next return-to-userspace or next tick (up to 1ms at this kernel's `CONFIG_HZ=1000`). Under
`voluntary`, the same request sets a real bit and fires `smp_send_reschedule()` immediately.
An **idle** target is always promoted to eager regardless of mode (`core.c:4105-4106`).

So voluntary isn't faster because the lock *holder* yields sooner — under voluntary
`preempt_schedule` is actually off, so the holder yields *less* readily. It's faster because the
**wakeup signal to the next thread is eager**, not deferred up to a tick. Corrected mechanism:
what gets faster is the *newly-runnable thread actually obtaining a CPU*, not lock release itself.

### Why this lands on IVH's migration path specifically

Both `bpf_sched_pre_lock_migrate()` dispatch paths (`kernel/sched/fair.c:13891`) converge on
`wakeup_preempt()` against the destination rq → `resched_curr_lazy()` (`fair.c:8964`). For
mechanism 0 (synchronous self-migration, the default), that wakeup latency sits **synchronously on
the lock-acquiring caller's own critical path** — it blocks inside `set_cpus_allowed_ptr()`'s
`wait_for_completion()`.

### Feasibility and minimal design

Mode selection is global (static-call patching under a mutex) — no per-CPU granularity exists to
build on, and making `cond_resched`/`preempt_schedule` per-CPU isn't feasible. But full voluntary
emulation isn't needed: only `get_lazy_tif_bit()` needs to be made conditionally per-CPU, since
`resched_curr_lazy()` already holds the destination rq's lock when it's called.

```c
/* kernel/sched/core.c, replacing 4152-4163 */
static __always_inline int get_lazy_tif_bit(struct rq *rq)
{
	if (!dynamic_preempt_lazy())
		return TIF_NEED_RESCHED;
	if (unlikely(time_before(jiffies, READ_ONCE(rq->ivh_eager_until))))
		return TIF_NEED_RESCHED;          /* eager window on this vCPU */
	return TIF_NEED_RESCHED_LAZY;
}
```

New field `unsigned long ivh_eager_until;` on `struct rq` (next to `ivh_uc_capacity` et al.,
`kernel/sched/sched.h:~1529/1725/1765`).

**Set**: a `jiffies` deadline (not a refcount — no natural residency-end event exists, and a
deadline can't leak), written in `bpf_sched_pre_lock_migrate()` immediately before each migration
commit (mechanism 0: before `fair.c:13833`; mechanism 1: before `fair.c:13749`):

```c
WRITE_ONCE(cpu_rq(target_cpu)->ivh_eager_until,
	   jiffies + READ_ONCE(ivh_eager_jiffies));   /* new sysctl, default ~2 */
```

**Clear**: none needed — self-expiring. Two migrated tasks on one CPU just take the later deadline.
Races between the remote unlocked write and the locked read are benign either direction (worst
case: one missed or one extra eager resched). A stricter variant worth A/B'ing: set immediately
before the migrate call, clear immediately after it returns, confining eagerness to exactly the
landing window.

### Risks

- **This produces `full`-like behavior on the flagged CPU, not `voluntary`-like.** `preempt_schedule`
  stays globally on under `lazy`, so a flagged CPU now allows arbitrary-point kernel preemption on
  a real `TIF_NEED_RESCHED` — i.e., it's not established that "eagerness" alone (vs. voluntary's
  *removal* of involuntary preemption) is the actual winning ingredient. **Recommended control
  before building anything**: boot with global `preempt=full` and compare against `voluntary` and
  `lazy`. If `full` behaves like `lazy` and only `voluntary` wins, this design targets the wrong
  mechanism entirely.
- **Lock-holder preemption amplification**: eager resched can preempt a mutex/sleeping-lock holder
  on exactly the CPU IVH just declared healthy — precisely the pathology IVH exists to mitigate,
  now made more likely there. (It cannot preempt a spinlock holder — `preempt_count > 0` already
  blocks that regardless of mode.)
- **Not orthogonal to the wake-vehicle finding**: every eager remote resched is the same
  `smp_send_reschedule()` IPI already implicated in the regression — but at migration-rate (gated
  by `ivh_steal_imminent`/`ivh_max_concurrent`), not halt-rate, so likely much rarer in absolute
  terms. Track `/proc/interrupts`'s `RES` line as a first-class metric if this is built.
- `CONFIG_NO_HZ_FULL=y` in this build means on a `nohz_full` CPU with one runnable task, the tick
  is off, so the once-per-tick lazy→eager promotion never fires — lazy deferral there is bounded
  only by return-to-user, making this flag *more* valuable specifically there.

### Hypothesis

**Probably does not close the main 5-8% regression** — that regression is rooted in pv-qspinlock's
halt/wake path, which never dequeues the task, so `wakeup_preempt()`/`resched_curr_lazy()` isn't on
that path at all. This is a plausible **complement to the separate migration-effectiveness win**
(migration's value proposition — "get this thread running on a healthy vCPU now" — is partially
undercut every time landing on that vCPU is itself deferred up to a tick), not a fix for adaptive
spinning's own gap.

**Free, already-instrumented test to run before building anything**: `ivh_timeout_count` already
counts migrations whose `wait_elapsed_ns > 1000000` (`fair.c:13755`/`:13849`) — and 1ms is exactly
one tick at this kernel's `CONFIG_HZ=1000`. Compare that counter's distribution under `lazy` vs.
`voluntary` with zero new code. If the two distributions are identical, the whole premise is wrong.

---

## Idea 2: TSC-based proactive lock stealing at the queue head

**User's framing**: currently lock stealing only happens when a new thread joins the queue and
finds the lock free. Wants an enhanced mode where the *waiter already on the queue* closest to
acquiring can grab the lock if it looks free — either via a small grace-period gap (let the true
predecessor act first, steal if it doesn't within the gap), or by checking TSC staleness (reuse the
existing heartbeat mechanism: if it's been a while since last update, grab the lock instead of
sleeping). Rationale given: qspinlock's queue order isn't a correctness guarantee, just a
starvation-avoidance one, so jumping the nominal order when staleness is detected shouldn't break
anything fundamental.

### What stealing already exists today — read this before designing anything new

This kernel has `pv_hybrid_queued_unfair_trylock()` (`kernel/locking/qspinlock_paravirt.h:90-142`),
not `pv_queued_spin_steal_lock()` (naming differs from some other trees). Two steal paths already
exist:

- **Path A — new arrival, pre-queue**: steals whenever `!(val & _Q_LOCKED_PENDING_MASK)`
  (`:100-101`). Only reachable from `qspinlock.c:363`/`:391` — **not** reachable from
  `pv_wait_head_or_lock()`.
- **Path B — the queue head, every spin iteration**: `pv_wait_head_or_lock()` calls
  `trylock_clear_pending()` on *every one* of `SPIN_THRESHOLD` iterations (`:1005`,
  `try_cmpxchg_acquire` on `locked_pending`), not just once.
- **Path C — head, at hash time**: `:1060`, `xchg(&lock->locked, _Q_SLOW_VAL) == 0` → head found it
  free while hashing.

**The head-of-queue "grab it the instant it looks free" behavior the user described already exists**,
unconditionally, at full spin cadence. There is no gap to close there.

**What's genuinely missing**: the head halts *unconditionally* on `SPIN_THRESHOLD` exhaustion.
`pv_wait_node()` (mid-queue waiters) has mechanism 2's scoped-halt gate for tier-2
(`kvm.c` around `:826-827`: `if (!wait_early && mech == 2) continue;`). `pv_wait_head_or_lock()` has
**no equivalent** — it always clears pending, hashes, and calls `pv_wait()` on exhaustion. IVH's own
source comment above `kvm.c:2413` calls this asymmetry out explicitly as intentional (until now).

### Mid-queue stealing (the user's literal, broadest framing) is unsafe — do not build it

MCS nodes are statically per-CPU with an index refcount (`qspinlock.c:84`,`:347`,`:539`) that gets
recycled the instant an acquisition completes. If a mid-queue node bows out of the queue early, its
predecessor still holds a pointer to that node and will unconditionally write to it
(`arch_mcs_spin_unlock_contended`, `pv_kick_node()`, `:527-528`) on its own eventual acquire — but
by then that node's array slot may already belong to a *different, later* acquisition on that CPU.
That's a direct mutual-exclusion break, not a bookkeeping wart, and its successor is left orphaned
(no one left to ever set its `->locked`). Fixing this properly needs abortable/generation-tagged MCS
nodes plus concurrent lock-free list-splice — invasive to generic `qspinlock.c`, disproportionate to
the expected gain. **Scope must be narrowed to the head only**, where none of this applies (the head
already legally re-enters its own poll state via `continue`; nothing is hashed, no kick obligation
is created).

### Detection strategy: use CS-age, not heartbeat reuse — already measured in this tree

This surprised both the user's framing and my own prior assumption: **this codebase already has
`ivh_cs_head_check()`** (`kvm.c:509-653`, consulted at `:1031`) with multiple predicate forms,
already A/B measured on a 27-million-sample run (`ivh_tsc_beat.h:336-386`):

| form | signal | sensitivity | false-positive rate | precision |
|---|---|---|---|---|
| 0 | CS-age > threshold (time-gap) | 34.15% | 0.203% | 18.36% |
| 1 | in-CS AND beat stale | 10.22% | 0.166% | 7.68% |
| 2 | beat stale only (heartbeat reuse) | 6.89% | 0.183% | 3.34% |

**The user's specific "reuse the TSC heartbeat" idea (form 2) has already been tried here, and it's
the worst of the three.** Structural reason: tier-2's heartbeat is republished by a *spinning* CPU
every ~90µs (`ivh_beat_publish_in_spin()`, mask `0xfff`) — but a lock *holder* isn't spinning, so its
only refresh is the 1kHz tick, making heartbeat staleness a low-resolution, low-precision signal for
this specific position in the protocol. CS-age (form 0, already built, backed by
`ivh_cs_beat_threshold` = 220000 cycles, `kvm.c:1407`) beats it on every axis. **Recommendation:
build the head-scoped-halt gate on the existing form-0 predicate, not on heartbeat reuse.**

### Concrete patch shape

```c
/* qspinlock_paravirt.h, in pv_wait_head_or_lock()'s loop, before falling through to hash+halt */
        clear_pending(lock);

+       if (READ_ONCE(ivh_pv_wait_mechanism) == 2 &&
+           READ_ONCE(ivh_head_scoped_halt) &&
+           !ivh_head_holder_stale(lock))     /* ivh_cs_head_check() sans loop-mask */
+               continue;                     /* re-arm SPIN_THRESHOLD, stay hot */

        if (!lp) { /* ONCE */
```

`continue` re-enters cleanly at the loop's own top (`pn->state = VCPU_RUNNING`, `set_pending`) —
exactly the state the head must hold regardless.

### Correctness

The steal `cmpxchg` itself (`trylock_clear_pending()`, `try_cmpxchg_acquire` on `locked_pending`,
expected `_Q_PENDING_VAL`) is safe by construction under any timing: it only succeeds if `locked==0`
*and* `pending==1` (ours) atomically — either it precedes the real holder's release (fails, no
steal, no harm beyond one extra cmpxchg) or follows it (succeeds, and the holder has genuinely
already left). Mutual exclusion holds unconditionally. The only live hazard is the pre-existing,
documented `clear_pending()` starvation window (`kvm.c` around `:489-507`) — and the new `continue`
path re-sets pending within a few instructions, making that window *shorter* than today's (which
currently stays open across a full `pv_wait()`), not longer.

### A live wrinkle worth checking before building

`ivh_cs_head_check()` at `ivh_cs_preempt_src==2` currently does the *opposite* of what this proposal
wants — it triggers an *earlier* bail into halt+IPI, not a later one (`:1031-1032`). Given the
established wake-cost asymmetry, that existing path is plausibly already making the regression
*worse* today, and what this proposal describes is close to that mechanism's inverse. Worth an
explicit, isolated A/B of the current `src==2` behavior before assuming a clean baseline.

### Hypothesis

This is the more directly relevant of the two ideas to the measured root cause: it removes a real
halt+IPI round trip from the critical path specifically in the case where it's provably wasted
(holder still healthy) — attacking excess halt *frequency* at exactly the one site
(`pv_wait_head_or_lock`) that's currently ungated, unlike `pv_wait_node()`.

The ceiling is bounded by two numbers, both already measurable with zero new code:

1. **What fraction of the measured 2.3-3.2x excess halts even happen at the head** vs mid-queue
   (`pv_wait_head_or_lock()` vs `pv_wait_node()` are distinct `pv_wait()` call sites — split
   `ivh_pv_wait_calls`, `kvm.c:1106`, by caller). This single number bounds the entire possible gain.
2. **How rare a genuinely-preempted holder actually is**: existing data says only **0.134%** of
   head checks on hackbench hit one (`ivh_tsc_beat.h:404-410`), capping precision near 18% even with
   the best predicate form. If head halts are a small minority of the excess, most of the 5-8% gap
   lives in `pv_wait_node()`'s halt policy instead, and the lever to pull is there, not here.

---

## Summary: recommended order of operations if either is pursued

Both proposals have a **free, already-instrumented empirical check** that should run before any
code is written, since both could falsify or bound the idea with existing counters:

1. Idea 1: diff `ivh_timeout_count`/`wait_elapsed_ns` distribution under `lazy` vs `voluntary`
   (zero new code) — tests whether tick-granularity deferral is actually costing migrations.
2. Idea 1 control: boot `preempt=full` and compare against `voluntary`/`lazy` — isolates "eager
   wake" from "no involuntary preemption" as the actual winning mechanism before building either.
3. Idea 2: split existing halt/wait counters by call site (head vs. mid-queue) — bounds the maximum
   possible benefit before writing the scoped-halt gate.
4. Idea 2 pre-check: isolate current `ivh_cs_preempt_src==2` behavior in an A/B, since it may
   already be actively counterproductive under the existing (non-scoped) head-check logic.

None of these four require a kernel rebuild.
