# Post-Acquisition Reactive Migration (PARM) — design doc

**Status as of end of session 2026-07-02: design significantly revised after
correctness review. Kernel-side PARM is NOT safe to build as-is — read the
Correctness Hazard section before writing any code. Userspace-side PARM
remains viable and is the recommended near-term target.**

## One-line summary

Instead of guessing before a lock is taken (IVH, ruled out this session —
see `ivh_findings_2026-07-02.md`), migrate a thread *immediately after it
becomes the confirmed lock-holder*, gated on lock contention (not applied
uniformly) — entirely from inside the guest, using only vcap/vact-derived
signals, no hypervisor cooperation, no paravirt hypercalls.

## Why this is different from IVH (and doesn't inherit its failure)

IVH's failure mode: it fired *before* knowing whether this thread would
ever hold a lock, so it paid its ~3µs migration cost on roughly 1-in-15,000
attempts that were ever actually at risk. PARM removes that source of
waste by only acting once a lock is *confirmed* held.

**Important correction, discovered later this session: this alone is not
enough to make PARM positive-EV.** The original flat-cost model
(`C_LHP ≈ 1ms`) still gives `(1/15,000) × 1ms ≈ 67ns` expected benefit
against `~3000ns` migration cost — still clearly negative. **PARM only
becomes positive-EV when combined with a lock-hotness gate** (see
Professor Feedback section below) — the "confirmed holder" property alone
is necessary but not sufficient.

## The mechanism (revised)

Insert a bracket immediately after lock acquisition completes, before the
critical section body runs:

```c
// inside __raw_spin_lock() (include/linux/spinlock_api_smp.h), after
// queued_spin_lock() / do_raw_spin_lock() returns — this single point
// is reached by BOTH the fast path (uncontended cmpxchg) and the slow
// path (MCS queue + spin-wait), so one insertion point covers both,
// no separate slow-path-specific hook needed. Verified against the real
// struct definitions this session — see Structural Findings below.

preempt_enable();
if (current->flags & PF_IVH_ELIGIBLE &&
    lock_is_hot(lock) &&              // NEW — see Hotlock section; without
                                       // this gate the trade is negative-EV
    self_check_ok()) {                // OPEN QUESTION — see below, no
                                       // settled signal choice yet
    ivh_migrate_self(pick_target());
}
preempt_disable();
```

### Open question: what should `self_check_ok()` actually check?

**Not settled this session — flag explicitly for next time, don't assume
either answer below is correct without further work.**

The original design used `is_cpu_preempted(smp_processor_id())`. A later
Opus review (evaluating a separate but related question — whether IVH's
*source*-side trigger generally should use `is_cpu_preempted()` instead of
`cpu_capacity`/`ewma_act_ns`) identified a problem that applies equally
here: **to execute the check at all, your vCPU must currently have host
time — meaning it's being ticked, meaning `clock_preempt` is fresh, meaning
`is_cpu_preempted(self)` reads false almost by definition.** It only reads
true in the narrow <1.5ms window right after resuming from a steal — i.e.
it detects a steal that already *ended*, not one that will hit the CS about
to run. This is a general property of self-checking this specific signal,
not specific to the original pre-lock design — it applies to PARM's
post-acquisition self-check exactly as much.

The alternative, `cpu_capacity`/`ewma_act_ns`, avoids that specific paradox
but reintroduces the ~1-second staleness (`vcap`'s poll interval, verified
this session — see below) that this whole redesign was partly trying to
escape.

**No clean signal choice was found for this self-check by end of session.**
Options to explore next: (a) accept capacity/time-left's staleness as a
coarse pre-filter only, not a precise trigger, similar to how Gate 1
already works in the original pre-lock design; (b) look for a
higher-resolution self-observable signal that doesn't require "currently
running" to read (none identified yet); (c) accept PARM's self-check is
inherently weaker than hoped and rely on the Hotlock gate to do most of the
precision work instead.

## CORRECTNESS HAZARD — read before building kernel-side PARM

**This is the most important addition to this doc. It was not identified
until a later code-level review this session, after the mechanism above
was first drafted.**

`spin_lock()` disables preemption (`preempt_disable()`,
`include/linux/spinlock_api_smp.h:130-132`) for a reason beyond deadlock
avoidance: an enormous amount of kernel code takes a spinlock specifically
to protect **per-CPU** state, relying on the invariant "I hold this lock ⇒
I cannot migrate ⇒ `this_cpu_ptr()` stays valid for the whole critical
section." Verified this session: the kernel has a live, checked mechanism
specifically for this (`__this_cpu_preempt_check()`,
`include/linux/percpu-defs.h:315-454`) — this is a real, actively-enforced
invariant, not a theoretical concern.

PARM's bracket acquires CPU A's lock, then
`preempt_enable() → migrate → preempt_disable()` can land execution on CPU
B, and the critical section body then runs on B while still holding **A's**
lock and touching what the code assumes is still **A's** per-CPU data —
silent data corruption. **This will NOT trip the "scheduling while atomic"
check** (`kernel/sched/core.c:6133-6154`) — that check only catches
blocking-while-atomic; a genuine `preempt_enable()`/`preempt_disable()`
bracket is legal by construction, so it sails straight past that
protection. Legal (won't BUG) is not the same as correct (preserves the
per-CPU invariant).

One piece of good news: by the time `queued_spin_lock()` returns, the
calling thread's MCS node is already released back to the per-CPU pool —
so migrating post-return does **not** corrupt the MCS queue/lock internals
themselves. The hazard is entirely about what the critical section *body*
assumes about `this_cpu_ptr()` stability, not the lock's own bookkeeping.

**This hazard does not exist for userspace-held locks** (pthread mutex /
futex) — userspace code has no kernel per-CPU invariant in force, so
migrating a thread holding a userspace lock is unconditionally safe from
this specific angle. **This is the deciding factor for scoping the whole
project** — see Recommended Scope below.

**Also note:** a Hotlock gate reduces how *often* PARM fires (less
exposure), but does not remove this hazard — a rare corrupting migration
on a contended per-CPU lock is exactly as corrupting as a frequent one.
Frequency-gating fixes EV, not correctness.

## Structural findings: what qspinlock/MCS actually expose (verified against real code, 2026-07-02)

```c
// The lock word — include/asm-generic/qspinlock_types.h — 4 bytes total
struct qspinlock {
    u8  locked;   u8  pending;   u16 tail;  // tail encodes (cpu,idx) of
                                             // the LAST waiter only — an
                                             // identity, not a count
};

// Per-waiter node — include/asm-generic/mcs_spinlock.h
struct mcs_spinlock {
    struct mcs_spinlock *next;  // forward-only pointer to next waiter
    int locked;                 // 1 once signaled to proceed
    int count;                  // nesting depth (0-3, for IRQ-nested
                                 // acquisitions), NOT a waiter count
};

// Native wrapper — kernel/locking/qspinlock.c:40
struct qnode {
    struct mcs_spinlock mcs;
    // long reserved[2];  <- only exists under CONFIG_PARAVIRT_SPINLOCKS,
    //                       which is OFF in this build (verified:
    //                       `# CONFIG_PARAVIRT_SPINLOCKS is not set` in
    //                       both /boot/config-$(uname -r) and the tree's
    //                       .config, despite CONFIG_PARAVIRT=y and
    //                       CONFIG_KVM_GUEST=y both being on)
};
```

**There is no O(1) waiter-count or queue-position signal anywhere in
native qspinlock.** `tail` only identifies the last waiter; `next` only
lets you walk forward from a node you already hold, and there's no head
pointer or prev-pointer stored generically. Any Hotlock mechanism needs
genuinely new state — this is a real patch, not a "read a hint that's
already there."

Minimal-footprint way to add it: a separate side-table keyed by lock
address (same shape as the existing `pv_hash` structure in
`kernel/locking/qspinlock_paravirt.h`, usable as a template even though
this build has PV spinlocks disabled), holding a small atomic waiter
counter. Increment on entering `queued_spin_lock_slowpath()`
(`qspinlock.c:131`, right when the fast-path cmpxchg has already failed
and the thread commits to queueing), decrement before returning. Fully
uncontended locks — the overwhelming majority — never touch this table,
so they pay zero cost.

## Professor feedback and the corrected expected-value math

Original flat model used `C_LHP ≈ 1ms` (one holder's own stuck duration) —
against `~1/15,000` odds of any given acquisition being at risk, this is
clearly negative EV regardless of when you check (pre-lock or
post-acquisition).

**Professor's correction:** under real contention, when a lock-holder
stalls, *every other thread waiting for that same lock* also burns cycles
for the whole stall duration. True cost is closer to
`(number of waiters) × (stall duration)`, not just the holder's own
downtime. Worked with the professor's own numbers (10 waiters, ~60ms
aggregate stall cost):

```
expected benefit = P(steal hits this acquisition) × aggregate cost
                  ≈ (1/15,000) × 60,000,000ns
                  ≈ 4,000ns

expected cost = migration cost ≈ 3,000ns

net ≈ +1,000ns   (positive, but a thin margin — roughly 33%, meaning the
                   hotness gate needs to be reasonably accurate, not just
                   directionally correct, or this collapses back to
                   negative)
```

**This is why Hotlock is not a "nice to have" — it's the specific
condition under which PARM's math is favorable at all.** On an uncontended
lock, the aggregate cost collapses back to the flat single-holder estimate
and the math goes negative again — migrating the holder of a lock nobody's
waiting on is exactly the "many times for nothing" waste to avoid.

**PARM and adaptive spinning (separately designed by the user) solve
different halves of that 60ms:** adaptive spinning stops the *other 10
waiters* from burning CPU cycles while stuck (saves wasted compute, not
wall-clock delay); PARM tries to prevent the stall from happening at all
(saves the delay itself, when it works). Not redundant — complementary.

## Recommended scope, given the correctness hazard (Opus review, 2026-07-02)

**Build order: capacity/time-left signal fix (item 1) → pthread waiter
counter (item "3") → PARM scoped to userspace-held locks only (item 2,
gated by the counter). Do NOT attempt kernel-qspinlock PARM as a shipped
result — cut MCS-position patching (item "4") to future work.**

Reasoning: userspace-held locks are the *only* domain where post-
acquisition migration is both correctness-safe (no per-CPU kernel
invariant hazard) AND where the contention math has actually been worked
out (the "10 waiters" scenario). Kernel-qspinlock PARM is simultaneously
the hardest to implement correctly (thousands of call sites assume
per-CPU stability, no general way to audit them from the qspinlock layer)
and the domain where the math is thinnest. A mechanism that silently
corrupts per-CPU kernel state under contention is a worse paper outcome
than a clean, scoped userspace result with the kernel extension honestly
described as future work blocked on a named, specific, real problem.

**Practical note on the pthread waiter counter:** a standard glibc mutex
does not expose a usable O(1) waiter count — `__nusers` is not it (it's
process-shared/robustness bookkeeping, not a live waiter count); real
futex waiters live in the kernel's internal futex hash, not a readable
field. Cleanest path: wrap the benchmark's contended mutex with your own
atomic counter incremented/decremented around lock/unlock — a self-
contained shim, no glibc patching, no kernel rebuild coupling. Roughly a
day of work.

## Why this isn't PV spinlock

PV spinlock's `pv_wait`/`pv_kick` mechanism requires the hypervisor to
implement and expose new hypercalls (the guest traps into the hypervisor
to halt a vCPU and to wake one back up). Every hypercall is documented
attack surface — no single named CVE for pv-spinlock specifically, but the
general principle is well-supported: a malicious or compromised guest
exploiting a bug in a hypercall handler can potentially execute code with
hypervisor privilege (see Sources below). PARM never traps into the
hypervisor and requires no hypervisor-side implementation or opt-in.

## Why this isn't PLE

PLE is a host/hardware feature (VMX pause-loop exiting) — runs entirely
outside the guest, guest has zero visibility or control. PARM runs
entirely inside the guest kernel. No host-side configuration, no
per-workload PLE gap/window tuning.

## Honest caveats to state plainly in the paper

1. The `preempt_enable()` window is not hard-bounded — it makes
   preemption legal again, not guaranteed-short. In practice should be
   rare (nothing deliberately yields), but not a provable bound.
2. `is_cpu_preempted()` has ~1.5ms resolution (tick-driven) — if used
   anywhere in the final design, it cannot see anything shorter than
   roughly a scheduler tick.
3. This does not need to beat PLE or PV-spinlock on raw performance —
   the value proposition is deployment constraints (no hypervisor
   cooperation, no per-workload host tuning, smaller hypercall attack
   surface). But any claimed improvement must survive paired testing AND
   the outlier-removal robustness check (see `ivh_findings_2026-07-02.md`
   Methodology notes) before it goes in the paper — several promising-
   looking results this session did not survive that check.

## Composes with

- **OSMODE**: cheap, `vcap`-armed static key gating the whole mechanism
  off unless the VM is observed sustained-oversubscribed. Orthogonal,
  cheap, low risk — build alongside.
- **Adaptive spinning**: see Professor Feedback section above.

## Sources on hypercall attack surface

- Technical Information on Vulnerabilities of Hypercall Handlers:
  https://arxiv.org/pdf/1410.1158
- Paravirtualized ticket spinlocks for KVM host (LWN):
  https://lwn.net/Articles/564791/

## Key files

- `include/linux/spinlock_api_smp.h:130-132` — `preempt_disable()` call site
- `include/asm-generic/qspinlock.h:107` — `queued_spin_lock()`, unified
  fast/slow-path return point
- `include/asm-generic/qspinlock_types.h`, `include/asm-generic/mcs_spinlock.h`,
  `kernel/locking/qspinlock.c:40` — the structs in Structural Findings above
- `include/linux/percpu-defs.h:315-454` — `__this_cpu_preempt_check()`,
  live evidence the per-CPU-hazard concern is real, not theoretical
- `kernel/sched/cputime.c:288-308` — `is_cpu_preempted()`, `clock_preempt`
  write at line ~517 inside `account_process_tick()`
- `kernel/sched/core.c:5717` — `ivh_migrate_self()`, already built,
  reusable as-is
- `kernel/sched/fair.c:13257-13276` (Gates 1-4), `:13327` (target veto),
  `:13129` (disabled rescue path) — original pre-lock IVH, reference only
- `kernel/locking/qspinlock_paravirt.h` — `pv_hash` structure, template
  for the waiter-count side-table
