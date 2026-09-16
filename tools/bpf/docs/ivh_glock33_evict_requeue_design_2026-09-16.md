# G-LOCK-33 design: evict-and-requeue instead of rotation repair, 2026-09-16

Status: **BUILT** 2026-09-16 as `6.17.0-G-LOCK-33-evict+`. See §11 for what
shipped and where it differs from this design. Supersedes the rotation-repair splice in
G-LOCK-31 (`pv_handoff_rotate`) and G-LOCK-32 (`pv_deferred_handoff`'s splice
block), and **obsoletes §7 of `ivh_lockskipping_kill_or_embrace_2026-09-15.md`**
(the `cmpxchg_tail` trick), which existed only to unblock the tail case that
this design does not have.

Origin: user, 2026-09-16. "Don't skip halted waiters, and do a special retry for
skipped preempted waiters. Instead of skipping then repairing the queue, send
them to the back of the queue so they can either lock-steal or join the back of
the line. We sacrifice fairness for threads we cannot trust due to the host
being away."

## 1. The idea

A preempted waiter is not repositioned. It is **removed from the queue**, tagged
`VCPU_SKIPPED`, and left to re-enter on its own when its vCPU is scheduled
again. Fairness is preserved for waiters we control (halted ones are never
skipped and are woken normally) and abandoned for waiters we do not (host-
preempted ones, which could not have used the lock anyway).

## 2. Why this is structurally simpler than rotation repair

Rotation repair writes two pointers because it KEEPS the skipped node in the
queue one position back. Eviction does not keep it, and the unlocker is leaving
the queue anyway, so there is nothing to repair:

```
before:  H -> S(preempted) -> L(live) -> ...
action:  mark S SKIPPED; promote L
after:   L -> ...            (S is orphaned, holding its own node)
```

**Zero pointer writes for the unlink.** The lock word's tail is untouched. The
existing promotion protocol at `kernel/locking/qspinlock.c:541`

```c
arch_mcs_spin_unlock_contended(&next->locked);
pv_kick_node(lock, next);
```

runs unchanged; only the choice of `next` changes. `pv_handoff_evict()` drops
into the slot `pv_handoff_rotate()` occupies, same signature.

## 3. The tail restriction disappears (the biggest consequence)

Both current paths refuse to act when the node they want to move has a NULL
`->next` (`qspinlock_paravirt.h:2437`, "never splice through a NULL ->next"),
because the lock word still names it as the tail and a concurrent enqueue would
clobber the splice. Eviction never touches the pick's `->next`, so the pick
being the tail is fine.

The only surviving rule is **do not skip a node whose `->next` is NULL**, and
that is self-enforcing: a NULL `next` means there is nobody to skip *to*, so the
walk stops and promotes that node anyway. It is no longer a restriction.

Two consequences for the existing plan:

1. **§0 of the kill-or-embrace plan is wrong under eviction.** It argued
   hackbench cannot test skipping because two waiters per lock leaves nothing to
   skip to. With `H -> S -> L`, `S->next == L` is non-NULL, so depth-2 queues
   ARE actionable. Every campaign winner reopens as a test workload.
2. **The Phase 1 opportunity count must be re-measured.** 306 spliceable
   events/s (0.0075% of acquisitions) was computed under the tail rule; a large
   share of what it counted as `splice_blocked_tail` becomes real opportunity.
   Step 1's STOP RULE A must be re-run before it can kill anything.

Safety argument for the tail: a non-NULL `->next` proves the node is not the
tail, permanently. The tail only moves forward, and the only way `lock->val`
can name this node's `encode_tail(cpu, idx)` code again is if the node itself
re-publishes it -- which happens only on requeue, after the reset in §6.

## 4. Commit point: cmpxchg RUNNING -> SKIPPED

"Never skip a halted waiter" and the halt race are the same problem. A candidate
can halt between classification and decision. Make the state transition the
commit:

```c
u8 old = VCPU_RUNNING;
if (!try_cmpxchg(&pn->state, &old, VCPU_SKIPPED))
        break;          /* it halted under us: promote it, which is fair */
```

- **Succeeds**: the candidate was `VCPU_RUNNING`. A host-preempted vCPU reads
  RUNNING (that is tier 2's entire premise), so it is spinning or off-CPU and
  will observe `SKIPPED` when it resumes. **No kick, no hypercall, no IPI** --
  the eviction is free on the unlock path.
- **Fails**: the candidate is `VCPU_HALTED`. Do not skip it; promote it and let
  the existing `pv_kick_node()` wake it, exactly as today.

This is why the tag belongs in `pn->state` and not in a spare `rot_flags` bit:
only a CAS on `state` atomically excludes the halt transition. A separate flag
would need a second handshake.

## 5. The walk: always promote the node you are looking at

**Never backtrack.** Because a skip is a removal, the queue is well formed after
every single step, so the walk can commit incrementally:

| candidate class | action |
|---|---|
| LIVE | promote |
| HALTED | promote (and kick, as today) |
| PREEMPTED, `next == NULL` | promote |
| PREEMPTED, hops == `IVH_ROT_HOP_CAP` | promote |
| PREEMPTED, cmpxchg failed | promote |
| PREEMPTED, otherwise | mark SKIPPED, advance |

Invariant this buys: **the node whose predecessor was skipped is always the node
that gets promoted**, so no waiter is ever left spinning behind an orphan.

```c
static void pv_handoff_evict(struct qspinlock *lock, struct mcs_spinlock *node,
                             struct mcs_spinlock **pnext)
{
        struct mcs_spinlock *cand = *pnext;
        unsigned long src = READ_ONCE(ivh_pv_preempt_src);
        u64 thr = READ_ONCE(ivh_pv_beat_threshold), now = rdtsc();
        int hops;

        if (!READ_ONCE(ivh_pv_evict_enable))
                return;

        for (hops = 0; hops < IVH_ROT_HOP_CAP; hops++) {
                struct pv_node *pn = (struct pv_node *)cand;
                struct mcs_spinlock *after;
                u8 old = VCPU_RUNNING;

                if (ivh_rot_class(cand, src, thr, now) != IVH_ROT_PREEMPTED)
                        break;                          /* LIVE or HALTED: take it */
                after = READ_ONCE(cand->next);
                if (!after)
                        break;                          /* nobody to skip to */
                if (pn->requeues >= READ_ONCE(ivh_pv_requeue_max))
                        break;                          /* starvation bound, sec 7 */
                if (!try_cmpxchg(&pn->state, &old, VCPU_SKIPPED))
                        break;                          /* halted under us */
                this_cpu_inc(ivh_evict_marked);
                cand = after;
        }
        *pnext = cand;
}
```

## 6. The retry, waiter side

**Correction to the original framing.** `ivh_tsc_beat_publish()` runs from
`account_process_tick()` (`kernel/sched/cputime.c:547`) -- interrupt context,
per-CPU, for whatever task is on that CPU. It can observe but cannot break the
waiter out of its spin. The check belongs in the waiter's own loop in
`pv_wait_node()`, next to the existing `node->locked` test:

```c
if (READ_ONCE(node->locked))
        return 0;
if (READ_ONCE(pn->state) == VCPU_SKIPPED)
        return -EAGAIN;
```

`pn->state` is at offset 20 of `struct pv_node`, which embeds `mcs` first, so it
shares a cacheline with `node->locked`: the extra load is free.

The intended effect survives exactly: because the check sits in the spin loop, a
preempted vCPU discovers its eviction precisely when the host gives it a CPU
back. Nobody needs to know when that is.

`queued_spin_lock_slowpath()` gains a `requeue:` label immediately above
`old = xchg_tail(lock, tail)`:

```c
        if (pv_wait_node(node, prev, lock) == -EAGAIN) {
                this_cpu_inc(ivh_evict_requeued);
                pn->requeues++;                 /* NOT reset here; see sec 7 */
                pv_init_node(node);             /* state = VCPU_RUNNING */
                node->locked = 0;
                node->next = NULL;
                if (queued_spin_trylock(lock)) {        /* "steal" */
                        this_cpu_inc(ivh_evict_steal_ok);
                        goto release;
                }
                goto requeue;                           /* "back of the line" */
        }
        arch_mcs_spin_lock_contended(&node->locked);
```

Same `node`, same `idx`, same `tail` code -- do **not** re-run
`idx = node->count++` (`qspinlock.c:323`); the per-CPU 4-deep qnode stack must
see exactly one push and one pop per acquisition.

**Ordering requirement:** `state = VCPU_RUNNING` and `next = NULL` must both be
visible before `xchg_tail` publishes this node's tail code, or a promoter can
reach a node that still reads `SKIPPED`.

Node lifetime is safe throughout: an evicted waiter has not left the acquire
path, so it still owns its qnode index and nobody else on that CPU can reuse it.

## 7. Known costs and hazards

**7.1 The starvation bound is gone and must be rebuilt.** Rotation cost a
skipped waiter one position, capped at `ivh_pv_rot_skip_max` (default 4).
Eviction costs it the entire queue, uncapped: a chronically preempted vCPU can
be evicted, requeue, and be evicted again without limit. Fix: a `u8 requeues` in
`struct pv_node` (there is room -- `rot_flags` occupies one byte of a three-byte
hole after `->state`, so `sizeof(struct pv_node)` stays 32 and the
`BUILD_BUG_ON` at `:1113` still holds), zeroed on genuine queue entry but NOT on
requeue reset, with the walk refusing to evict at `ivh_pv_requeue_max`. **No
fairness bound can be claimed for this design until that exists**, and it is the
first thing a reviewer will attack.

**7.2 `VCPU_SKIPPED` perturbs every `state != VCPU_RUNNING` test** (~12 sites).
The two that matter:
- tier 1 in `pv_wait_early()` (`:1034`): the node whose predecessor was just
  evicted reads `SKIPPED`, concludes "prev is down", and halts early. It is
  the promoted node, so `pv_kick_node()` rescues it -- a performance blip, not
  a hang. Count it.
- `ivh_rot_class()` (`:1484`) reads state first and would class a `SKIPPED`
  node as HALTED. Unreachable (a SKIPPED node is not in the queue), but make it
  explicit rather than accidental.
Also: append the new state at the END of the exported halt-cause arrays.

**7.3 Thrash.** A vCPU that returns quickly is sent to the back, works forward,
and is preempted again -- potentially worse than having waited. This is why the
closest prior art (He, Scherer & Scott, "Preemption adaptivity in time-published
queue-based spin locks", HiPC 2005) uses a timeout rather than eager eviction.
The §7.1 requeue cap is the guard. **Cite this paper; it is the nearest
neighbour and a reviewer will know it.**

**7.4 Honest per-skip cost.** Rotation: 2 plain stores in the unlocker.
Eviction: 1 cmpxchg in the unlocker, plus later 1 `xchg_tail` on the hottest
cacheline in the system, paid by the evicted thread. Eviction uses MORE atomics
per skip. The real argument is different and stronger:

> Rotation leaves the preempted waiter near the front, so **every subsequent
> handoff during the same preemption episode re-walks it** -- O(handoffs) per
> episode. Eviction removes it once -- O(1) per episode. That, not "repair is
> expensive", is the case for this design; repair is two stores.

**7.5 The evicted thread's `xchg_tail`** lands on the contended lock word. Skips
were rare enough to ignore under the tail rule; §3 may make them much more
common. Measure `ivh_evict_marked` rate before assuming the cost is noise.

## 8. Counters

`ivh_evict_marked`, `ivh_evict_requeued`, `ivh_evict_steal_ok`,
`ivh_evict_halt_race` (cmpxchg failed -> promoted instead),
`ivh_evict_cap_refused` (requeue cap hit), plus a per-node requeue histogram --
the requeue distribution IS the fairness evidence for §7.1.

## 9. Interaction with G-LOCK-32 (unlock-time choice)

Eviction is orthogonal to `ivh_pv_skip_point` and works at either site; it is
strictly better at unlock, where the classification is current. Under
`skip_point=1` the deferred first waiter B sits in `pv_wait_node()` with pending
set as a reservation -- if B is evicted it must requeue into a lock whose
pending bit is set on its behalf. Resolve before building the combination;
build `skip_point=0` first.

## 10. Build order

1. `VCPU_SKIPPED` + `requeues` in `pv_node`; audit the `!= VCPU_RUNNING` sites.
2. `pv_handoff_evict()` behind `ivh_pv_evict_enable` (default 0), replacing
   nothing -- `pv_handoff_rotate()` stays so the two are A/B-able in one kernel.
3. Waiter-side `-EAGAIN` + the `requeue:` label + trylock.
4. Counters, then re-run Step 1 of the kill-or-embrace plan with the tail rule
   lifted (§3.2) before spending anything on throughput A/Bs.

## 11. As built (2026-09-16)

Kernel `6.17.0-G-LOCK-33-evict+`, branch `ivh-rebuild-main`. Everything below
defaults OFF, so the boot reproduces G-LOCK-32 exactly until a knob is set.

### 11.1 One walk, both promotion points

The user's requirement was that the promotion can happen either pre-CS or after
unlock with **the same logic**. That is a single shared function,
`pv_evict_walk(succ)` in `qspinlock_paravirt.h`, returning the node to promote:

| `ivh_pv_skip_point` | caller | classification uses |
|---|---|---|
| 0 | `pv_handoff_rotate()` -- the thread that just acquired, pre-CS | pre-CS state |
| 1 | `pv_deferred_handoff()` -- the unlock slowpath, after `pv_unhash()` | current state |

Both callers test `ivh_pv_evict_enable` **first** and fall through to the
rotation splice only when it is off, so the two policies cannot both run on one
handoff. The walk writes no `->next` pointer at either site, so each caller's
existing promotion protocol is untouched (`arch_mcs_spin_unlock_contended()` +
`pv_kick_node()` at skip_point 0; the F1 `locked=1` / cmpxchg-state / kick at
skip_point 1).

### 11.2 Three handoff policies selectable live in one boot

```
ivh_pv_rot_enable=0  ivh_pv_evict_enable=0   upstream FIFO handoff
ivh_pv_rot_enable=1  ivh_pv_evict_enable=0   rotation repair (G-LOCK-31)
                     ivh_pv_evict_enable=1   eviction (G-LOCK-33)
```

crossed with `ivh_pv_skip_point` 0/1. The sysctl handlers refuse the
rotation+eviction combination **in both directions** rather than letting
eviction silently win -- a run recorded as "rotation on" that actually measured
eviction would be worse than a failed write. `ivh_pv_evict_enable=1` also
requires `ivh_pv_preempt_src=2`, for the same reason rotation does: at any
other value the liveness test degrades to `vcpu_is_preempted()`, hardwired
false on this host, so nothing would ever be classified preempted and the A/B
would compare two identical arms.

### 11.3 Changes from the design above

- **`pv_wait_node()` now returns `int`** (`PV_WAIT_OK` / `PV_WAIT_REQUEUE`,
  defined in `kernel/locking/qspinlock.h` so the native stub can share them).
  The `VCPU_SKIPPED` check sits in the hot spin loop immediately after the
  `node->locked` load -- same 32-byte `pv_node`, so same cacheline, an L1 hit
  against a `cpu_relax()` measured at ~26 cycles.
- **The requeue label reuses the existing post-init trylock**, not a new one.
  `requeue:` sits above `if (queued_spin_trylock(lock))` and above the
  `smp_wmb()`, which means the retry gets the "steal it outright" attempt and
  the barrier its reset needs, in the right order, for free.
- **`pv_requeue_node()` is separate from `pv_init_node()`** precisely so it does
  NOT clear `->requeues`. It re-seeds the heartbeat for the same reason
  `pv_init_node()` does: the node is about to be somebody's predecessor again.
- **The fairness histogram moved to eviction time.** Recording
  `ivh_evict_requeue_hist[min(count, 7)]` on every eviction makes the array a
  **survival curve** -- bucket i counts tenures that reached i evictions, so the
  highest non-empty bucket IS the observed starvation bound. Sampling
  `->requeues` at acquisition instead would have been incomplete: a tenure can
  acquire by three routes (the `requeue:` trylock, promotion out of
  `pv_wait_node()`, or an empty queue after `xchg_tail()`), and any single
  per-acquisition site misses at least one.
- **`ivh_evict_stop_halted` counts only HALTED stops**, not every non-PREEMPTED
  stop. A LIVE stop is the ordinary outcome on nearly every handoff; folding
  the two together would make the fairness rule look like it fires constantly.

### 11.4 Freeze argument, as implemented

A queued waiter cannot leave the queue except by having `->locked` set, and only
the holder does that -- so every node the walk touches is frozen and its `->next`
is stable. Eviction does not weaken this, because eviction is the holder's own
action. An evicted node clears `->next` and republishes its tail code only from
`pv_requeue_node()`, i.e. strictly after the walker is done with it, and behind
the caller's `smp_wmb()`.

Two consequences checked during the build:
- A node that is evicted and requeues can, in a short queue, land back inside
  the same walk's reach. Re-classifying and even re-evicting it is harmless:
  it is a genuine queue member at its new position, bounded by the hop cap and
  the requeue cap.
- The node whose predecessor was evicted reads a stale `prev->state`. It is
  always the promoted node (§5 invariant), its tier-1 check may fire once, and
  `pv_kick_node()` rescues it. Performance blip, not a hang.

### 11.5 Knobs and counters

Knobs: `ivh_pv_evict_enable` (0), `ivh_pv_requeue_max` (4, clamped 255, 0
disables eviction entirely).

Counters: `ivh_evict_walks` (denominator), `ivh_evict_walks_acted`,
`ivh_evict_marked`, `ivh_evict_requeued`, `ivh_evict_steal_ok`,
`ivh_evict_halt_race`, `ivh_evict_cap_refused`, `ivh_evict_tail_stop`,
`ivh_evict_stop_halted`, `ivh_evict_hop_cap`,
`ivh_evict_requeue_hist[8]`.

**Reading `ivh_evict_tail_stop` correctly:** under rotation the equivalent
(`ivh_rot_splice_blocked_tail`) was a REFUSED opportunity. Under eviction it
means "the queue ended here, so there was genuinely nobody to promote instead".
The two are not comparable and must not be differenced.

### 11.6 First run

Step 1 of the kill-or-embrace plan, with the tail rule lifted:
`ivh_pv_evict_enable=1`, `ivh_pv_skip_point=0`, and read
`ivh_evict_walks_acted / ivh_evict_walks`. That ratio is the opportunity rate
the old 0.0075% figure was measuring under a restriction that no longer
applies, and it now includes depth-2 queues -- so run it on hackbench and the
campaign winners, not only on qlockbench.
