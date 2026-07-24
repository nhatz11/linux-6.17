# IVH: reasoning pass on the "prev-OR-LH sleep + LH→waiter release IPI + heartbeat" proposal

Date: 2026-07-23
Branch: `kernel-43-clean`
Booted kernel at time of writing: `6.17.0-rseqport57-tpauseIPI+` (this is a
reasoning-only pass — no build, no reboot, no live load testing was done).

This doc is a point-by-point critique of a design proposal, grounded in the
actual source in this tree, not abstract reasoning. It deliberately disagrees
where the proposal doesn't hold up. It is a companion to, and does not
supersede, `ivh_tpause_ipi_and_tsc_next_steps_2026-07-22.md` (the "can we yield
without HLT" negative result) and `ivh_halt_ipi_mechanism2_implementation_2026-07-22.md`
(mechanism 2 = real HLT + plain IPI).

## The proposal, restated

> Use HLT for sleeping. Sleep when **prev OR the lock holder (LH) itself** is
> inactive, via a steal-time check. Replace the kick with an IPI sent **from the
> LH (about to release) to the next waiter**. Sending at lock release may be too
> late / add latency — is there an in-guest way to send it earlier? Idea: a
> per-tick "heartbeat" timestamp that eventually becomes `last_preempt`; send the
> IPI at release-time *or* at that heartbeat update. Or is there a better place
> that also needs no host/PV cooperation?

Verdict up front: the **spine of the proposal is right** — HLT to sleep, a plain
guest IPI to wake, and (crucially) the instinct that the remaining room to
optimize is on the *wake/when-to-sleep* side, not the yield side. Three of the
specific mechanisms proposed on top of that spine do **not** hold up against the
code: the "OR LH" half of the sleep trigger is not implementable and, on
inspection, not needed; "release is too late" rests on a premise that doesn't
survive scrutiny; and the "heartbeat" is a re-description of a field this tree
**already has** (`rq->clock_preempt`), conflated with a *different* field
(`rq->last_preemption`), and using it as a *wake* trigger would be worse, not
better. Details below.

---

## Point 1 — "Sleep when prev OR the LH itself is inactive"

**Verdict: REFINE. The prev/LH distinction is conceptually real, but the code
gives a deep-queue waiter no way to identify the true LH, and for that waiter
`prev` is not a weak proxy for the LH — it is the actual, most-proximate blocker
of its own progress. The "OR LH" half should be dropped.**

### The structures, read directly

`struct pv_node` (`kernel/locking/qspinlock_paravirt.h:50-54`):

```c
struct pv_node {
    struct mcs_spinlock mcs;   /* has ->next, ->locked, ->count */
    int                 cpu;
    u8                  state;  /* VCPU_RUNNING / VCPU_HALTED / VCPU_HASHED */
};
```

A queued waiter parked in `pv_wait_node()` is handed exactly one neighbor
pointer — `prev` (its MCS predecessor) — and can read `prev->cpu` / `prev->state`.
It has a `->next` (its successor) but **no pointer of any kind to the actual
current lock holder.**

`struct qspinlock` itself (`include/asm-generic/qspinlock_types.h:14-...`)
records **no owner CPU** — it is just `atomic_t val` decomposed into
`locked` / `pending` / `tail`. Unlike a mutex (`->owner`) or an OSQ node, a
qspinlock deliberately does not track *who* holds it. So there is no field,
anywhere reachable from a queued waiter, that names the LH's CPU to feed into
`vcpu_is_preempted()`.

The `pv_hash` table (`pv_hash()`/`pv_unhash()`, lines 208-257) does **not** close
this gap. It maps `lock → pv_node`, but the node it stores is **the next waiter
to be woken** (the queue head that hashed itself in `pv_wait_head_or_lock()`, or
was hashed on its behalf by `pv_kick_node()`), **not the holder**. It exists so
the *unlocker* can find whom to kick. A random deep-queue waiter cannot use it to
find the LH — and `pv_unhash()` `BUG()`s if there is no entry, so it isn't even a
safe lookup to attempt speculatively.

**So "check the LH's steal bit" is not implementable as stated for a queued
(role-C) waiter: there is no LH CPU to check.** `vcpu_is_preempted(prev->cpu)` —
which is exactly what `pv_wait_early()` already does (line 294) — is checking
`prev`, and that is the only vCPU a queued waiter can name.

### Why checking prev is not a compromise — it is the correct signal

The MCS queue is strict FIFO. A role-C waiter cannot get the lock until `prev`
has acquired it, released it, and handed `node->locked` forward. Its progress is
gated **transitively and entirely by `prev`**. Concretely:

- If `prev` is running, the chain ahead of you is draining and you are close.
- If `prev` is host-preempted, **you are stuck no matter what the ultimate holder
  is doing** — even if the true LH released this instant, `prev` still has to be
  rescheduled, take the lock, release it, and pass the baton to you.

So for a queued waiter, `prev`'s preemption is the *more* relevant signal than
the holder's, not a lossy stand-in for it. Adding an "OR LH is preempted" clause
would tell the waiter to sleep on a condition (holder preempted) that, when
`prev` is healthy, does **not** actually block its next step — a false trigger.

### Where a holder check *would* be meaningful, the code already handles it

The one waiter that races the holder directly is the **queue head**, parked in
`pv_wait_head_or_lock()` — and it waits on `lock->locked` itself (line 522,
`pv_wait(&lock->locked, _Q_SLOW_VAL)`). "Holder released" **is literally its wait
condition**, and its wake is the unlock-path IPI (`ivh_pv_kick()`). There is no
holder-CPU to steal-check there either (`lock->locked` doesn't encode one), and
none is needed — it is woken by the release event directly.

**Refinement to adopt:** keep the sleep trigger as
`vcpu_is_preempted(prev->cpu)` for queued waiters (which is `prev`, correctly),
and recognize the head waiter already keys off holder-release via `lock->locked`.
Drop "OR LH." The proposal's underlying instinct — *don't sleep unless something
is actually wrong* — is the sound part, and it is exactly the already-validated
reframing (make the preemption signal the sole sleep trigger instead of falling
through `SPIN_THRESHOLD` unconditionally). The "OR LH" addition is the part that
doesn't survive contact with the queue structure.

---

## Point 2 — "Is sending the IPI at lock release too late / does it add latency?"

**Verdict: CORRECT THE PREMISE. Release is the *earliest moment the information
"the lock is free" exists at all*, so there is no "too late" for the wake
*trigger*. The proposal conflates two different latencies. Also: the code already
sends a wake *earlier* than release, at MCS handoff — the wake is already
two-staged.**

The event that matters — "the lock is now free" — cannot be known by anyone,
guest or host, before the release actually happens. Release-time is therefore not
a *late* place to trigger the wake; it is the **earliest place it can possibly be
triggered.** Sending "wake up, the lock is free" before the lock is free would
mean sending it on a *guess*, and any waiter woken on a guess re-checks the
condition, finds the lock still held, and goes back to sleep — a spurious wake
(this is exactly the failure mode Point 3's heartbeat idea would cause).

Two distinct latencies are being blurred:

1. **Trigger latency** — "when can we *know* to send?" Answer: at release, not
   earlier, ever. Already optimal. Not improvable by any in-guest trick.
2. **Delivery/completion latency** — "after we send, how long until the woken
   vCPU is actually running with the lock?" This is real: IPI propagation, plus —
   if the woken vCPU was HLT'd and the host handed its pCPU away — the *host*
   having to reschedule that vCPU before it can run. That is the genuine cost,
   and it is a *wake-completion* cost, not a "sent too late" problem.

Crucially, latency (2) is the **same structural wall** the `2026-07-22` tpause
doc closed out: the guest cannot make the host reschedule a descheduled vCPU any
faster from inside the guest. It is also the same cost mechanism 0's
`KVM_HC_KICK_CPU` faces. So there is no in-guest lever to pull on (2) either.

**The code already does more than the proposal credits it with.** The wake is
already two-staged:

- `pv_kick_node()` (line 382, with the IVH `smp_send_reschedule(pn->cpu)` at
  line 444) fires **at MCS handoff** — when `prev` sets `next->locked` and
  advances the successor from role C to role B. This is *earlier than the final
  unlock*: it promotes the next-in-line so it is spinning on `lock->locked` and
  ready the instant the holder releases.
- `ivh_pv_kick()` (`arch/x86/kernel/kvm.c:1296`, called from
  `__pv_queued_spin_unlock_slowpath()` **after** `smp_store_release(&lock->locked, 0)`)
  fires **at the actual release** to wake the head waiter.

So "send the IPI earlier than release" is, for the queue-node transition,
**already happening** — the design already pushes the handoff-side wake as early
as the FIFO order allows, and reserves the release-side wake for the one waiter
whose condition genuinely is the release. There is nothing to move earlier.

---

## Point 3 — The "heartbeat" / `last_preempt` idea

**Verdict: CORRECT. The heartbeat already exists in this tree as
`rq->clock_preempt` (written every tick, unconditionally). It is conflated with a
*different* field, `rq->last_preemption` (written only on >1 ms steal). Using
either as a *wake* trigger would be redundant with mechanism 2's existing
timer-tick backstop at best, and harmful (spurious wakes) at worst. The tick
heartbeat's correct role is a *preemption detector* — which is a "when to sleep"
input, not a "when to wake" trigger.**

### There are two fields, not one — the proposal merges them

Grepped and read directly:

- **`rq->clock_preempt`** — written **every scheduler tick, unconditionally**, in
  `account_process_tick()`:
  ```c
  /* kernel/sched/cputime.c:503 */
  this_rq()->clock_preempt = sched_clock();
  ```
  This **is** the heartbeat the proposal describes ("every scheduler tick, write a
  timestamp"). It already exists. Its consumer is `is_cpu_preempted()`
  (`cputime.c:288-293`): a vCPU that the host has descheduled stops taking ticks,
  so its `clock_preempt` goes *stale*; staleness > 1.5 ms ⇒ "this vCPU was
  preempted." That is a heartbeat-staleness detector, verbatim. It feeds the
  rescue / migration-target-health logic (`fair.c:214-215, 13506, 13541-13542`),
  where it is even referred to in-comment as a "heartbeat."

- **`rq->last_preemption`** — written **only inside
  `steal_account_process_time()`**, and **only when the detected steal exceeds
  1 ms**:
  ```c
  /* kernel/sched/cputime.c:268-276 */
  if (steal > 0) {
      u64 now = sched_clock();
      if (steal > 1000000) {                 /* only >1ms steals */
          ...
          rq->last_preemption = now;
      }
      ...
  }
  ```
  This is the steal-derived signal that feeds `ivh_steal_imminent()` /
  `ivh_rq_capacity_and_timeleft_ok()` (Gate 1+2, `fair.c:13214-13301`). It is
  **not** the tick timestamp, and the tick timestamp does **not** "eventually
  become" it — they are separate fields, with separate write conditions, feeding
  separate consumers (`last_preemption` → the pre-lock migration gates;
  `clock_preempt` → the rescue/target-health path). The proposal's mental model
  ("the tick timestamp eventually becomes `last_preempt`") is the one factual
  error to correct cleanly here.

### Using the tick as a *wake* trigger would be worse, not better

The tick fires on a **fixed, lock-agnostic cadence** — and on *this* kernel that
cadence is **1 ms**, not 4 ms: `.config` in this tree is `CONFIG_HZ=1000`
(the proposal assumed HZ=250 → 4 ms; the real value here is 1000 → 1 ms/tick).
Either way, the tick **has no knowledge of "the lock just became free."** So
waking a sleeping waiter at the tick means waking it on *"some time has passed,"*
not *"the thing you were waiting for happened."* The waiter re-checks
`lock->locked` / `node->locked`, finds it **still held** (nothing about the tick
correlates with release), and goes back to HLT. That is a pure-cost spurious
wake — and for a HLT'd waiter it is especially bad: the host must reschedule the
vCPU *just to have it immediately re-yield.*

Moreover, "wake periodically as a fallback" **already exists**, twice:

- Mechanism 1's `IVH_PV_ADAPTIVE_TSC` (~1 ms) poll deadline is exactly a bounded
  periodic fallback re-check (`ivh_pv_wait()`, `kvm.c:1288-1293`).
- **Mechanism 2's HLT gets the timer-tick backstop for free**: a HLT'd vCPU
  un-halts on the next timer tick regardless of any IPI (documented in
  `ivh_halt_ipi_mechanism2_implementation_2026-07-22.md` §5, "worst-case wake
  bound is one timer tick"). So the "wake at the tick" fallback the proposal wants
  to *add* is, for mechanism 2, **already the mechanism's inherent safety net** —
  adding an explicit tick-driven wake-IPI would duplicate it.

### What the heartbeat *is* legitimately good for

Exactly what the tree already uses it for: a **preemption detector**, via
staleness (`is_cpu_preempted()`). That is a periodic, lock-agnostic signal used
to answer *"is this vCPU in trouble"* — a **"should I sleep / should I migrate"**
input — never a *"time to wake"* trigger. Note this loops straight back to
Point 1: the sound use of a preemption signal is to gate the *decision to sleep*
(is `prev`/self preempted), which is precisely the already-validated reframing.
The heartbeat feeds "should I sleep" well; it does not feed "time to wake" at all.

---

## Point 4 — "A better place that also needs no host/PV cooperation"

**Verdict: CONFIRM — already satisfied. The concern is already met by mechanism
2's existing release-time IPI.**

`smp_send_reschedule()` is already **fully guest-internal**. Traced in the
mechanism-2 doc (§3, Premise B) and re-confirmed here:
`smp_send_reschedule()` → `native_smp_send_reschedule()` →
`__apic_send_IPI(cpu, RESCHEDULE_VECTOR)` — a plain LAPIC IPI, **not** overridden
by KVM anywhere in this tree. It requires **no** PV feature bit, **no** hypercall,
and **no** host cooperation beyond the host *delivering an interrupt to a running
guest* — which the host must do for *any* interrupt in *any* guest, PV-aware or
not. This is no more "PV cooperation" than HLT's mandatory trap is (the framing
already settled with the user: HLT's trap and IPI delivery are both universal
hardware-virtualization behavior; the hypercall was the only PV-specific part,
and mechanism 2 already dropped it).

So the "doesn't require PV" property the proposal is reaching for is **already a
property of the current mechanism-2 release-time IPI.** There is no better place
to find on the *send* side — send-side cooperation is already zero.

One honest nuance (same as Point 2's latency (2)): while the *send* needs no
cooperation, the *effect* on a HLT'd-and-descheduled target does depend on the
host rescheduling that vCPU's pCPU. That is the inherent floor from the
`2026-07-22` tpause doc, not a PV dependency, and it is identical to what
mechanism 0's hypercall faces. It is not something a different send-site fixes.

---

## Summary table

| # | Proposal element | Verdict | One-line reason |
|---|---|---|---|
| 1 | Sleep when prev **OR LH** inactive | **Refine — drop "OR LH"** | No LH-CPU is recorded anywhere a queued waiter can reach (`struct qspinlock` has no owner; `pv_hash` stores the next *waiter*, not the holder). For a role-C waiter `prev` is the actual blocker, not a proxy; the head waiter already keys off holder-release via `lock->locked`. |
| 2 | Is release-time IPI "too late"? | **Correct the premise** | Release is the *earliest* moment "lock is free" exists — there is no "too late" for the trigger. The real latency (host rescheduling a HLT'd vCPU) is a *delivery* cost with no in-guest lever. And the code already wakes *earlier*, at MCS handoff (`pv_kick_node`), then again at release (`ivh_pv_kick`). |
| 3 | Per-tick "heartbeat" → `last_preempt`, wake at it | **Correct** | The heartbeat already exists as `rq->clock_preempt` (every tick), and is a *different* field from `rq->last_preemption` (>1 ms steal only) — they don't convert into each other. As a *wake* trigger it is redundant with mechanism 2's timer-tick un-halt backstop, or harmful (spurious wakes, since the tick knows nothing about release). Its correct role is a *preemption detector* → a "when to sleep" input. |
| 4 | A send-site needing no PV/host cooperation | **Confirm — already met** | `smp_send_reschedule()` is a plain LAPIC IPI, not KVM-overridden, needs no PV feature or hypercall. Mechanism 2's existing release IPI already satisfies "no PV cooperation." |

## Overall recommendation

**The one genuinely novel, buildable, source-grounded idea in the proposal is the
part it shares with the already-validated reframing: make the preemption signal
the *sole* trigger for going to sleep, instead of falling through
`SPIN_THRESHOLD` and sleeping unconditionally.** Concretely, that is a change to
`pv_wait_node()`'s loop (`qspinlock_paravirt.h:322-366`): today it spins up to
`SPIN_THRESHOLD` and then HLTs (mechanism 2) whether or not `prev` ever looked
preempted; a purer version sleeps *only when* `vcpu_is_preempted(prev->cpu)`
fires (with a bounded fallback so a lost handoff still makes progress — and note
mechanism 2's timer-tick un-halt already *is* that fallback). This is small,
honest, and grounded — but scope it precisely:

- It uses **`prev`**, not "LH" (Point 1). Do not add an LH check — it isn't
  implementable and isn't what blocks the waiter.
- It changes **when we decide to sleep**, keeping HLT as the sleep primitive and
  the existing two-stage IPI as the wake. It does **not** try to send the wake
  earlier than release (Point 2) or add a tick-driven wake (Point 3) — both of
  those are either already done or counterproductive.
- Be honest about the ceiling (carried from the `2026-07-22` docs): on *this*
  host mechanism 0 already yields via its own HLT, so this refinement will not
  *beat* mechanism 0 here. Its real, modest value is **avoiding HLT round-trips
  that weren't warranted** — i.e., not sleeping (and paying a vmexit + host
  reschedule) when `prev` was healthy and the lock was about to come free anyway.
  That is a legitimate efficiency claim; "we found a way to yield the host that
  mechanism 0 didn't have" is not, and the evidence still doesn't support it.

Everything else in the proposal (the LH steal-check, the earlier-than-release
IPI, the heartbeat wake) should be set aside for the reasons above, and — for the
heartbeat specifically — noted as *already implemented* (`rq->clock_preempt` /
`is_cpu_preempted()`) rather than something to build.

## Appendix — key references used

- `kernel/locking/qspinlock_paravirt.h:50-54` — `struct pv_node` (no LH pointer).
- `include/asm-generic/qspinlock_types.h:14-...` — `struct qspinlock` (no owner CPU).
- `kernel/locking/qspinlock_paravirt.h:208-257` — `pv_hash()`/`pv_unhash()` (stores next waiter, not holder).
- `kernel/locking/qspinlock_paravirt.h:263-295` — `pv_wait_early()` (`vcpu_is_preempted(prev->cpu)`, the prev-only check).
- `kernel/locking/qspinlock_paravirt.h:315-373` — `pv_wait_node()` (SPIN_THRESHOLD-then-sleep loop; the refinement target).
- `kernel/locking/qspinlock_paravirt.h:382-445` — `pv_kick_node()` (handoff-stage IPI, earlier than release).
- `kernel/locking/qspinlock_paravirt.h:454-538` — `pv_wait_head_or_lock()` (head waits on `lock->locked` directly).
- `arch/x86/kernel/kvm.c:806-812` — `__kvm_vcpu_is_preempted()` (reads host-written `steal_time.preempted` bit).
- `arch/x86/kernel/kvm.c:1256-1268` — mechanism-2 HLT branch.
- `arch/x86/kernel/kvm.c:1296-1334` — `ivh_pv_kick()` (release-stage IPI, guest-internal `smp_send_reschedule`).
- `kernel/sched/cputime.c:256-286` — `steal_account_process_time()` (writes `rq->last_preemption`, >1 ms steals only).
- `kernel/sched/cputime.c:288-293` — `is_cpu_preempted()` (consumes `clock_preempt`, 1.5 ms staleness).
- `kernel/sched/cputime.c:503` — `account_process_tick()` writes `rq->clock_preempt` every tick (the real "heartbeat").
- `kernel/sched/fair.c:13214-13301` — `ivh_steal_imminent()` / `ivh_rq_capacity_and_timeleft_ok()` (consume `last_preemption`).
- `.config`: `CONFIG_HZ=1000` (1 ms/tick on this build, not 250/4 ms).
