# G-LOCK-32 design: choose the next waiter at unlock ("deferred promotion"), 2026-09-15

Status: design for review. Nothing built. Builds on G-LOCK-31 (kernel commit
`e04549b10222`, branch `ivh-rebuild-main`, tree `/root/kernels/linux-6.17-vanilla`).

## 1. Why

G-LOCK-31 skips preempted waiters at promotion (`pv_handoff_rotate()` at
`kernel/locking/qspinlock.c:530`), i.e. BEFORE the holder's critical section.
If the CS is long or preempted, the chosen head can itself become preempted
before release, and nobody re-checks. Choosing at unlock uses current state.

Holding the holder's MCS node through the CS is not an option: qnodes are a
per-CPU stack of 4 (`qspinlock.c:139`, `idx = node->count++` at `:318`,
`__this_cpu_dec` at `:545`), and holds do not nest the way waits do.
Instead (advisor's suggestion): the holder gives its node back as today and
keeps only a pointer to the first waiter. That pointer stays valid through the
CS because an unpromoted waiter cannot leave the queue while the lock is held.

## 2. Knobs (all default to G-LOCK-31 behaviour)

- `ivh_pv_skip_point`: 0 = choose next before CS (today); 1 = defer to unlock.
  Skipping itself is still `ivh_pv_rot_enable`. `skip_point=1, rot_enable=0`
  = deferred promotion with no skipping (isolates the cost of deferral).
- `ivh_pv_unlock_reserve` (only meaningful with `skip_point=1`):
  - 0 (default, stock-like): holder sets pending at acquire; the first waiter
    drops it if it halts during the CS; the holder clears it at release.
  - 1: holder sets pending at acquire and clears it at release (newcomers queue
    and halt during the CS; only arrivals at the release instant can steal).
  - 2: holder sets pending at acquire and leaves it set through the handoff; the
    chosen waiter clears it when it grabs the lock (no stealing at all).

## 3. Acquire path, `skip_point=1`

After `set_locked()` and the owner stamp (`qspinlock.c:482`, `:507`), and after
`next` is known (`:512` wait for `node->next`), instead of rotate + promote:

1. `pv_hash(lock, next)` — reuse the PV wake table (`qspinlock_paravirt.h:283`)
   to save the first-waiter pointer per lock. At this point no entry for `lock`
   exists (a self-hashed head unhashes on the `xchg==0` race; a kicked head was
   unhashed by the unlocker). NEED a non-BUG insert: today `pv_hash()` `BUG()`s
   when full (`:308`). On failure, fall back to today's promote-before-CS path.
2. `WRITE_ONCE(lock->locked, _Q_SLOW_VAL)` after hashing (same ordering as
   `pv_kick_node()` / the head path), so the unlock takes the slowpath.
3. Set pending (`set_pending()`), so arrivals stop camping in
   `pv_hybrid_queued_unfair_trylock()` (`:136-170` loops while tail != 0 and
   pending == 0) and enqueue instead — mirrors today's head spinning with
   pending set during the CS.
4. Do NOT set `next->locked`, do NOT call `pv_kick_node()`. Release the qnode
   (`:545`) as usual.

Uncontended branch (`:462`, tail == ours) and fast-path acquisitions are
unchanged: nothing to defer.

## 4. Unlock path

`queued_spin_unlock()` → owner clear (before release, unchanged) →
`pv_queued_spin_unlock()` asm cmpxchg 1→0 fails on `_Q_SLOW_VAL` →
`__pv_queued_spin_unlock_slowpath()` (`qspinlock_paravirt.h`):

- `node = pv_unhash(lock)`.
- Dispatch on the NODE, not the knob (so live toggling is safe):
  `node->locked == 1` → today's halted-head path (store 0, kick).
  `node->locked == 0` → deferred first waiter:
  1. Walk from `node` with G-LOCK-31's `ivh_rot_class()` rules if
     `ivh_pv_rot_enable`: skip PREEMPTED (VCPU_RUNNING + stale beat), stop at
     HALTED and pick the original first waiter, pick first LIVE; same splice
     (never splice a node whose `->next` is NULL), same starvation cap.
  2. Pending per `ivh_pv_unlock_reserve` (0/1: clear; 2: keep).
  3. `smp_store_release(&lock->locked, 0)`.
  4. Promote: `WRITE_ONCE(pick->locked, 1)`; `smp_mb()`; if `pick` state is
     VCPU_HALTED → `pv_kick(pick->cpu)`. Race with the halter in
     `pv_wait_node()` (`smp_store_mb(state, HALTED)` then reads `node->locked`
     before `pv_wait()`): store-then-read on both sides, so either the kicker
     sees HALTED or the halter sees locked == 1. Do not kick live picks.

## 5. First waiter during the CS

The first waiter B is still in `pv_wait_node()`, not the head loop. Its local
`prev` is the holder's qnode, which the holder has already returned.

- Rule 1 (`is_cs_preempted`, criterion from `ivh_cs_criterion`) must run in the
  waiter loop for B: `prev->cpu` is the holder's CPU (per-CPU constant), the
  owner-slot tag names `lock` only for the actual holder. No scan in waiter
  context. Needs a new halt cause in `ivh_node_halt_hist` (currently 6 causes)
  and reader updates.
- Tier 1 on B reads `prev->state` of the returned qnode. If the holder's CPU
  reuses that slot for another contended wait and halts, B halts early. Claimed
  harmless (B is woken at unlock) — count it.
- Reserve option 0: when B halts (any cause) and it is the first waiter (owner
  tag via `prev->cpu` names `lock`), drop the reservation with
  `cmpxchg(lock->locked_pending, _Q_SLOW_VAL | _Q_PENDING_VAL, _Q_SLOW_VAL)`
  rather than a plain store, so it cannot clear a pending bit that a later head
  set (locked is 0 or LOCKED then, not SLOW_VAL). Reviewer: check the ABA where
  the next holder also defers and re-sets SLOW_VAL|PENDING.

## 6. Unchanged

Migration engine, `ivh_pre_lock`, heartbeat publish sites, `spin_mode 1/2/6`
with `skip_point=0`. `spin_mode` gains presets for the unlock variants.

## 7. Known costs

- Every contended acquire does a hash insert; every contended unlock does the
  slowpath (unhash, optional walk, handoff).
- Rule 4 still stops at a halted first waiter; with reserve 2 the lock idles
  while it wakes.
- Nanosecond window between promotion and the pick grabbing the lock.
- Fast-path holders: the first arrival becomes head immediately (no pointer to
  save), same as today.

## 8. Review verdict (independent pass, 2026-09-15): SOUND WITH FIXES

Checked read-only against `e04549b10222`. Mutual exclusion, one-hash-entry-per-lock,
the frozen-node argument and the unlock-time splice hold. Required changes:

- **F1 (mandatory, liveness).** Promotion at unlock must CHANGE the pick's state,
  not just read it: `pick->locked = 1; smp_mb__before_atomic();
  if (try_cmpxchg_relaxed(&pick->state, VCPU_HALTED, VCPU_RUNNING)) pv_kick(cpu);`
  A halted node waiter sleeps on `pv_wait(&pn->state, VCPU_HALTED)`; only a state
  change clears that condition. Kicks do not latch in the no-PV-unhalt spin path,
  the IRQ-off spin path, or IPI mode, so section 4 as written can hang a waiter.
  Use RUNNING, not HASHED; never call `pv_kick_node()`/`pv_hash()` for the pick at
  unlock (orphan hash entry -> later deferral hangs the queue).
- **F2.** Unlock order: unhash -> walk/splice -> pending -> release -> promote.
- **F3.** Gate the waiter-side logic (rule 1 in `pv_wait_node`, reserve-0 drop) on
  `skip_point`, read once; refuse `skip_point=1` unless `ivh_cs_owner_enable=1`
  and `ivh_cs_owner_clear=1`.
- **F4.** Drop the "hash full -> promote before CS" fallback (it would BUG in
  `pv_kick_node`'s own `pv_hash`). Entries <= qnodes in use <= 4 x possible CPUs
  <= table size, so the insert cannot fail.
- **F5.** Reserve-0 `cmpxchg` on `locked_pending` under `#if _Q_PENDING_BITS == 8`.
- Reserve-0 policy gap: if B was already halted before the holder acquired, the
  holder should `smp_mb()`, re-read B's state after `set_pending`, and drop pending
  if HALTED.
- Append any new halt cause at the END of the exported halt arrays.

Notes: reserve 2 is not absolutely steal-free (a stealer that read the word before
the acquire can still win the byte cmpxchg after release). Every queue-path release
now goes through the asm slowpath and touches the global hash table twice; the pick
needs a cross-CPU handoff before its trylock, so the post-release gap is larger than
"nanoseconds" (to be measured).
