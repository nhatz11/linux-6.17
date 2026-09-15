# Lock skipping: kill it or embrace it -- the plan, 2026-09-15

One decision, made on evidence, in a fixed order. Each step has a **stop rule**,
so the cheapest step that can kill the idea runs first.

## 0. Why hackbench is the wrong workload

hackbench spreads contention across many pipe/socket locks with roughly **two
waiters each**. With two waiters there is no queue to walk: the successor is the
only candidate, so skipping has nothing to skip to and nothing to measure. Every
hackbench number in this project is therefore silent on lock skipping, in both
directions.

The same applies to most of the campaign's winners (dbench, fs_mark, ebizzy):
they win because of *halting* policy, not queue depth.

**What is needed: one lock, many waiters, deep MCS queue.**

| tool | why | status |
|---|---|---|
| **qlockbench** (`/root/linux-6.17/qlockbench.c`) | written for exactly this: drives a DEEP MCS queue on a SINGLE kernel spinlock (eventfd `ctx->wqh.lock`) | **built, ready** |
| **will-it-scale lock1 / lock2** | 16 threads on one file-lock path | ready (it regressed -10% under IVH, so it has depth) |
| **stress-ng dentry / flock** | many workers, shared dcache / `blocked_lock_lock` | ready |
| **locktorture** | the only tool with a **tunable hold time**, so it can sweep CS length on a kernel lock | needs `CONFIG_LOCK_TORTURE_TEST` -- fold into the next kernel build |

**spinbench and libslock cannot test this.** They are userspace
`pthread_spinlock_t` / userspace MCS; they never enter the kernel qspinlock, so
skipping is not in their path at all. They would show a flat line regardless of
the mechanism working or not.

## 1. Step 1 -- opportunity, detect-only (cheap, and can kill it outright)

Boot **G-LOCK-31**, set `ivh_pv_rot_probe=1` (count, do not act), run qlockbench
and the deep-queue kernel workloads. Read:

| counter | meaning |
|---|---|
| `ivh_rot_handoffs` | handoffs seen |
| `ivh_rot_preempted` | handoffs where the successor was preempted |
| `ivh_rot_splice_ok` | ... and a live waiter existed behind it with a non-NULL `->next` (**the real opportunity**) |
| `ivh_rot_stop_halted` | walk met a halted waiter and stopped (G-LOCK-31 fairness rule) |
| `ivh_rot_tail_stop` / `ivh_rot_no_live` | nothing to skip to |

**STOP RULE A:** if `splice_ok` is below ~100/s on the *deepest-queue* workload
available, the mechanism has no opportunity to act on. **Kill it** and write the
negative result. No further runs needed.

Prior evidence pointing this way: the 2026-09-14 Phase 1 measurement found 306
spliceable events/s on qlockbench = 0.0075% of acquisitions, and concluded that
for skipping to be worth 1% of throughput each splice would have to recover ~33 us
of *fully idle* lock, which is implausible by two orders of magnitude.

## 2. Step 2 -- does acting on it pay? (only if Step 1 passes)

A/B on the deep-queue workloads only:

- arm **IVH** = `spin_mode 2` (today's winner)
- arm **IVH_SKIP** = `spin_mode 6` (G-LOCK-31: tier 1 + exhaustion, no tier 2,
  `is_cs_preempted` on the head, skipping of preempted-not-halted waiters)

ABBA blocks, capacity-settled waits, 8 blocks -- the campaign harness runs this
unchanged by adding the two arms.

**STOP RULE B:** if IVH_SKIP is not above IVH by >= 5% on any deep-queue
workload, **kill it**. G-LOCK-31's corrected skip rule (never skip a halted
waiter) was the last plausible bug in the promotion-time design; if it still does
not pay, promotion-time skipping is finished.

## 3. Step 3 -- unlock-time skipping (only if Step 2 is ambiguous or positive)

**G-LOCK-32** (built 2026-09-15) defers the choice of the next waiter to unlock,
so it is made with current state instead of pre-CS state:

| knob | values |
|---|---|
| `ivh_pv_skip_point` | 0 = choose at promotion (G-LOCK-31), 1 = choose at unlock |
| `ivh_pv_unlock_reserve` | 0 = drop the pending reservation when the first waiter is already halted; 1 = clear at release; 2 = keep through handoff (no stealing) |

Requires `ivh_cs_owner_enable=1` and `ivh_cs_owner_clear=1` (enforced by the
sysctl handler).

Three arms, same workloads: **IVH**, **skip@promotion**, **skip@unlock**.
Also run `skip_point=1, rot_enable=0` -- deferred promotion with **no** skipping
-- to price the deferral itself (hash insert per contended acquire, slow path on
every contended unlock).

**STOP RULE C:** if the cost of deferral alone exceeds whatever skipping
recovers, kill it; the mechanism cannot pay for its own delivery.

## 4. Step 4 -- the sweep figure (only for a paper section that survives)

The one figure worth publishing is not a bar chart, it is a **line**: skipping's
benefit against **critical-section length** (or preemption intensity). That needs
a kernel lock with a tunable hold time, which means `locktorture`
(`CONFIG_LOCK_TORTURE_TEST`, currently off). Sweep `torture_type=spin_lock` hold
times across roughly 1 us .. 1 ms and plot IVH vs IVH_SKIP.

A second, cheaper sweep exists today: **queue depth** via qlockbench thread count
(2, 4, 8, 16), which directly tests the hypothesis that skipping only matters
once queues are deep.

## 5. What "embrace" would require

If Steps 1-3 pass, before it can be claimed:
- the four G-LOCK-32 review fixes are in the build (F1 mandatory: change the
  pick's state, do not merely kick it, or a waiter can hang);
- a starvation bound demonstrated (`ivh_pv_rot_skip_max`, default 4);
- fairness stated: halted waiters are never skipped (that is the G-LOCK-31 rule);
- the regression set re-run, since skipping changes the queue for everyone.

## 6. Honest prior

The 2026-09-14 Phase 1 result was **negative** (-1.23% over 10 rounds, -2.08%
winning 1 of 8 under the best conditions), and an independent review judged the
implementation correct and the negative result sound, while noting the throughput
A/B was underpowered and the magnitude argument was the stronger evidence.
G-LOCK-31 fixed a real bug in the skip rule (halted waiters were being skipped,
which both wasted the mechanism and hurt fairness) and G-LOCK-32 removes the
stale-information objection. **Those are the two remaining reasons to look
again. If Step 1 or Step 2 fails, the answer is kill, and the negative result is
itself publishable as a design lesson: stealing already rescues the case that
skipping targets.**

## 7. Possible improvement: splice the tail by moving the tail pointer

**Idea (user, 2026-09-15).** Today both paths refuse to splice a pick whose
`->next` reads NULL, because the lock word still names that node as the tail and
a concurrent enqueuer would clobber the splice (or be orphaned by it). Instead of
refusing, **update the tail first**:

```c
after = READ_ONCE(pick->next);
if (!after) {                                   /* pick looks like the tail */
    if (cmpxchg_tail(lock, pick_code, prev_code) == pick_code) {
        WRITE_ONCE(prev->next, NULL);           /* prev is the tail now */
        WRITE_ONCE(pick->next, succ);           /* pick moves to the front */
    } else {
        /* an enqueue is in flight: abort the skip, promote succ */
    }
}
```

**Why the race closes.** The tail lives in `lock->val`, and `xchg_tail()` and this
cmpxchg are both compare-and-swaps on that word, so they serialise:
- arrival wins -> tail is D, our cmpxchg fails, we abort (safe);
- we win -> tail is prev, the arrival's `xchg_tail()` returns prev and it links
  behind prev, giving `pick -> succ -> ... -> prev -> D`.

It also distinguishes the two cases a NULL `->next` cannot: a waiter mid-enqueue
has already changed the tail, so the cmpxchg fails.

**Why it matters.** With shallow queues the live waiter worth jumping to is very
often the tail, so `ivh_rot_splice_blocked_tail` is a large share of the refused
opportunities. In the 2026-09-14 Phase 1 data every "nobody to skip to" case was
a queue that simply ended there.

**Costs and open hazards (needs adversarial review before building):**
1. Adds an atomic RMW to a handoff path that today uses only plain stores, on the
   cacheline every arriving thread already contends.
2. The cmpxchg must preserve `locked` and `pending`, and it interacts with the
   steal path and the `(val & _Q_TAIL_MASK) == tail` uncontended exit
   (`qspinlock.c:462`).
3. Needs each node's own tail code `encode_tail(cpu, idx)`; `struct mcs_spinlock`
   does not store `idx`. Cleanest: stash the 16-bit tail code in `pv_node` at
   `pv_init_node()` time -- there are 2 free bytes after `rot_flags`, so sizeof
   stays 32.
4. Reviewer should attack: interaction with `pending` and the steal path; whether
   `prev` can itself be mid-splice; what happens if `prev` is the node that just
   acquired the lock.

**Sequencing:** queue this behind Steps 1-2. If the opportunity measurement says
there is nothing to act on even counting the tail-blocked cases, this cannot pay
for itself.

## 8. Known gap: no starvation cap in the unlock-time path

G-LOCK-31's promotion-time splice counts skips in the skipped node's `rot_flags`
(bits 2-7) and promotes it unconditionally at `ivh_pv_rot_skip_max` (default 4).
**G-LOCK-32's `pv_deferred_handoff()` does not carry that cap.** Exposure is
small in practice -- each skip costs one position, and a waiter can only be
skipped again while it is still preempted, during which it could not have used
the lock anyway -- but the cap must be added before any fairness bound is
claimed for unlock-time skipping.
