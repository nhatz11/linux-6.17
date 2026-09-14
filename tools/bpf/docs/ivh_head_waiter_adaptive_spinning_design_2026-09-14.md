# Adaptive spinning for the identity-free waiters (pending bit / queue head), 2026-09-14

Design draft. Nothing here is built. Builds on
`ivh_six_goals_report_2026-07-22.md` §4.2 and
`ivh_adaptive_tpause_ipi_plan_2026-07-22.md` §3, which established the
constraint; and on the measurements in
`ivh_lock_skipping_phase1_results_2026-09-14.md`, which turned out to size the
opportunity.

## 1. The constraint, restated exactly

Tier 1 and tier 2 both answer one question: *is my PREDECESSOR down?* Both need
a predecessor **identity** -- `prev->state` for tier 1, `ivh_tsc_beat[prev->cpu]`
for tier 2. Three waiter roles exist, and only one of them has that identity:

| role | has MCS node? | has usable `prev`? | can do tier1/tier2 today? |
|---|---|---|---|
| A. pending-bit waiter (`queued_fetch_set_pending_acquire`, waits on `smp_cond_load_acquire(&lock->locked, !VAL)`) | no | **no** | no |
| B. first thread at `queue:` (`xchg_tail()` returned no prior tail) | yes | **no** -- skips the `prev` branch, waits on `lock->val` | no |
| C. second and later queued waiters | yes | yes | **yes** |

A's conceptual predecessor is the lock **owner**. `struct qspinlock` is 32 bits
-- `locked` byte, `pending` byte, 16-bit tail -- and **contains no owner field**.
`locked` is a flag, not an identity. So there is no in-band way to ask "which
CPU holds this lock". B is in the same position for the same reason: an MCS node
does not help when nothing is in front of you in the queue.

This is a data-structure fact, not a tuning gap.

## 2. Why the obvious fix is out

Adding an owner-CPU field to `struct qspinlock` widens **every spinlock in the
kernel**. `spinlock_t` is embedded in `struct inode`, `struct task_struct`,
every wait queue, every `struct file`... Going 4 -> 8 bytes changes structure
layout and cacheline occupancy tree-wide. That is not a patch anyone would take,
and it would perturb every measurement this project has made. Rejected.

## 3. Ranked options

### Option 1 (RECOMMENDED) -- don't identify the holder; act on the case already detected

**Do this first. It is the only one with a measured opportunity, and the
detection is already built and running.**

`ivh_head_yield_ok_tier2_spinning` (in `pv_wait_node()`'s HEAD_SPINNING arm)
fires when: my `prev` IS the queue head, that head is in `HEAD_SPINNING` (it has
set the pending bit and not yet halted), tier 2 says the head is stale, **and
`lock->locked == 0`**. That is exactly "the lock is free and its designated
owner is not running".

Measured 2026-09-14 on qlockbench: **3,104/s**, against 47/s of spliceable
handoff-rotation events -- a **65x** larger opportunity, in the one window where
`pv_hybrid_queued_unfair_trylock()` CANNOT rescue the lock, because the stale
head left the pending bit set and stealing refuses to proceed while pending is
set.

Note what this sidesteps: **role B does not have to identify anyone.** Its
SUCCESSOR (a role-C waiter, which does have identity) detects the stall and acts
on its behalf. The identity problem is solved by delegation.

Proposed action, in the observing waiter:
```c
/* We are role C. prev is the queue head, it looks stale, lock is free. */
u32 old = _Q_PENDING_VAL;
if (try_cmpxchg_acquire(&lock->locked_pending, &old, _Q_LOCKED_VAL))
        /* we took the lock the dead head could not */
```
This is the identical atomic `trylock_clear_pending()` already performs -- one
cmpxchg on a field with long-audited semantics, no `->next` rewriting, no
starvation bound, no new per-node state. `HEAD_YIELDED` already exists in
`head_ctl` and is never set; the Stage-1 scaffolding is in the tree.

**Open question that must be answered first**: 3,104/s x even 100us would be 27%
of wall time idle on a lock sustaining millions of acquisitions/s -- implausible.
So either the windows are far shorter than the staleness threshold implies, or
the counter over-counts (it samples every `PV_PREV_CHECK_MASK` iteration, so one
stall can be observed repeatedly by the same waiter). **Measure the duration
before building the action.** Reuse the `ivh_rot_rel` stamp scheme keyed on the
head's `pn->cpu`: stamp at observation, consume when `lock->locked` next goes
nonzero. Two hours of work, and it decides whether this is worth anything.

### Option 2 -- contention-only owner breadcrumb

Publish the holder's identity out of band, but only when someone is waiting.
On slowpath acquisition, if the pending bit is set (i.e. contention exists),
the acquirer writes `{lock, smp_processor_id()}` into a small direct-mapped
table hashed by lock address; it clears on release. Roles A and B hash the same
way and validate the lock pointer before trusting the CPU.

- **Cost is paid only under contention**, never on the uncontended fastpath.
- Precedent exists: `pv_hash()` in `qspinlock_paravirt.h` already maintains
  exactly this shape of lock-keyed table for `_Q_SLOW_VAL`, so a hashed
  lock-indexed side table is not a novel cost to this subsystem.
- **Collisions give a wrong identity**, so the lock pointer must be stored and
  compared, and a mismatch must mean "unknown", never "not preempted".
- Conflicts with `pv_hash()` if the same table is reused -- it would need its
  own.

Worth building only if Option 1's duration measurement says the pending-blocked
window is real AND we then want roles A/B to self-assess rather than be rescued
by a successor.

### Option 3 -- identity-free staleness: judge the LOCK, not the holder

Roles A and B cannot ask "is the holder preempted", but they can ask "has this
lock been held longer than a critical section plausibly takes, at a moment when
the host is known to be oversubscribed". Two identity-free inputs:

1. time since I started waiting (local, free), and
2. the global capacity/steal signal IVH already maintains
   (`vcap_probe`, `ivh_capacity_threshold`, `ivh_steal_source`).

Halt early only when both fire. Cheap and requires no new per-lock state.

**Weakness, stated plainly:** it is a coarse inference. It cannot distinguish
"my holder is preempted" from "my holder is doing a long critical section while
some unrelated vCPU is preempted". Expect false positives proportional to host
oversubscription -- exactly the regime where being wrong is most costly. Treat
as a fallback for role A only, where nothing better is possible.

### Option 4 -- rejected: widen `struct qspinlock`. See §2.

## 4. Value ordering, and an honest note on role A

Role A's wait is **structurally the shortest in the system** -- it is the front
of the line, waiting only for the current holder to release, with no queue ahead
of it. `ivh_adaptive_tpause_ipi_plan_2026-07-22.md` §3 called role A "out of
scope structurally, not deferred" for the IPI-wake mechanism, and that judgement
still holds for the same reason: it never sleeps in a kickable primitive, and it
has the least to gain.

The value is concentrated in **role B**, and specifically in the case where B
sets the pending bit and is then descheduled by the host -- because that is the
one state in which the pending bit actively BLOCKS the unfair-steal path that
otherwise rescues every dead-owner situation. That is what Option 1 attacks, and
it is the only one of these with a measured event rate.

## 5. Recommended order

1. Measure the duration of the pending-blocked window (Option 1's open
   question). If the windows are short, the whole area closes cheaply.
2. If long: implement Option 1's cmpxchg in the observing role-C waiter.
   Smallest possible change, no new state, no starvation concern.
3. Only then consider Option 2, and only if roles A/B self-assessment is
   independently motivated.
4. Option 3 only for role A, and only if role A ever shows a measurable cost --
   it has not so far.
