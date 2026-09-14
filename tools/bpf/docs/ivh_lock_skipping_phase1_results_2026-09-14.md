# Lock skipping, Phase 1: built, measured, ABANDONED (negative result), 2026-09-14

Kernel `6.17.0-G-LOCK-28-rotate+`. Continues
`ivh_lock_skipping_phase0_results_and_phase0b_2026-09-14.md`.

**Verdict: handoff-time lock skipping does not help and slightly hurts. Stop
work on it.** The rotation code is correct and is left in the tree behind
`ivh_pv_rot_enable`, default 0.

## 1. What was built

At MCS handoff, if the successor's vCPU looks stale, promote the first LIVE
waiter behind it instead. With queue `us -> A -> B -> C`, A stale and B live:

```
after = B->next;  A->next = after;  B->next = A;  *nextp = B;
```
giving `B -> A -> C`. A run of stale nodes moves as a block, no node lost, no
cycle. Starvation bounded by a skip counter in spare bits of `pv_node.rot_flags`
(cap `ivh_pv_rot_skip_max`, default 4).

**Safety rule: only ever write a `->next` that is already non-NULL.** `->next`
is written exactly twice in `qspinlock.c` -- `node->next = NULL` at :344 before
the node is published, and `WRITE_ONCE(prev->next, node)` at :380 by the single
waiter whose `xchg_tail()` returned that tail code. So a non-NULL `->next` has
taken its one and only write. A NULL one may have an enqueuer mid-link whose
plain `WRITE_ONCE` would clobber us and orphan a waiter -- a permanently stuck
queue. The two cases are indistinguishable, so both are refused.

**Why the splice is safe at all**: every node behind us is frozen. There is no
exit path -- no goto, return or break -- between `WRITE_ONCE(prev->next, node)`
(:380) and `arch_mcs_spin_lock_contended(&node->locked)` (:383), and only the
predecessor writes `->locked`. So no queued node can end its tenure, free its
qnode, or re-enter until we make the promotion store. This also resolves the
"private cached `next`" hazard that
`ivh_lock_skipping_design_discussion_2026-09-13.md` §3.2 called "the actual
killer": the private read `next = READ_ONCE(node->next)` happens only AFTER a
node is promoted, and we only write `->next` of nodes that are still waiting.

Verified: `sizeof(struct pv_node)` still 32, `rot_flags` at offset 21,
`qspinlock.c` 25 insertions / **0 deletions** against upstream.

## 2. It works

Smoke and all later runs: hundreds to thousands of live splices, no hang, no
kernel complaint, `ivh_rot_splice_blocked_starve` non-zero (the starvation bound
is live code, not decoration).

## 3. It does not help

qlockbench (16 threads, ONE shared eventfd -- the deepest queue this box can
make), balanced alternating order so drift cannot land on one arm:

| test | rotation OFF | rotation ON | delta | rounds ON won |
|---|---|---|---|---|
| 10 rounds, ordinary conditions | 3,176,899 | 3,137,790 | **-1.23%** | 5/10 |
| 8 rounds, BEST conditions (82% spliceable, 306 splices/s in every round) | 3,203,076 | 3,136,581 | **-2.08%** | **1/8** |

t = -1.13 and -1.55 respectively (need ~2.3). The t-tests are not significant,
but losing 7 of 8 under the most favourable conditions we could construct is
itself a signal (sign test p ~ 0.035).

**The experiment never had the power to detect the predicted win.** Upside was
bounded at 0.03-1%; the rig's run-to-run spread is 8-33%. So the neutral result
is not evidence rotation is harmless either -- it is evidence the question
cannot be answered this way. That is the most reusable finding here.

## 4. Why it cannot help -- the structural argument

**A dead successor never sets the pending bit.** `set_pending()` is called in
`pv_wait_head_or_lock()`, which only runs once the promoted node actually
executes. A preempted one never gets there. The pending bit is precisely what
blocks `pv_hybrid_queued_unfair_trylock()`. **So whenever rotation has something
to do, the steal valve is guaranteed OPEN**, and an arriving thread takes the
lock anyway.

Rotation is, by construction, attacking the one situation that is already
covered. Measured steal share: 41%-70% of acquisitions across runs; in the
deep-queue configuration, **24.93 steals per queued handoff -- the queue carries
3% of traffic.**

Two further costs, both real:
- The detection walk costs one `rdtsc` + one remote cacheline read
  (`ivh_tsc_beat[succ->cpu]`) on EVERY handoff, to catch ~10-300 events/s. It
  cannot be pre-filtered: the obvious cheap gate `succ->state != VCPU_RUNNING`
  is free but a host-preempted spinner still reads `VCPU_RUNNING`, so it drops
  exactly the population of interest.
- The splice leaves the node after the spliced run watching the WRONG
  predecessor -- its stack-local `prev` still points at the node we promoted,
  which is now live and publishing a fresh heartbeat. So it does NOT early-halt
  when its real predecessor is down, and burns the full spin threshold. That is
  a *missed* early halt: it suppresses tier1/tier2, the mechanism that actually
  wins.

## 5. The two unreachable buckets

Of stale successors, the non-spliceable remainder splits as:
- **"no live node at all"** -- and `ivh_rot_no_live == ivh_rot_tail_stop`
  (measured 381 vs 380, and 315 vs 315 in an independent run). This bucket is
  **not** a chain of dead waiters. It is *the queue ended at the sleeper*. No
  design can invent a waiter who is not there; a CNA-style secondary queue
  reaches **0%** of it, not 63%.
- **"live node is the tail"** -- splicing it means moving the tail code in
  `lock->val` against a concurrent `xchg_tail()`. CNA does not do this either;
  `cna_order_queue()` stops short of the tail for the same reason our
  `after == NULL` gate does. Our rule is not conservative, it is CNA's rule.

The two are linked: a live node behind a stale one is almost by definition a
recent arrival, and recent arrivals are near the tail.

**Deepening the queue does fix the first bucket** -- spliceable went 20.6% ->
82% at `-t 16` on one lock -- but it simultaneously raises the steal ratio to
24.93 per handoff, because every waiter is also a camper. The two effects trade
off, which is why the best-conditions A/B still lost 7/8.

## 6. What is worth keeping

1. **`ivh_pv_beat_threshold` is miscalibrated for this purpose.** 220,000 cycles
   (100us). Over 25.9M samples, 98.5% of ages are under 100us, and of the
   over-threshold ones **94.6% are 476us-4ms** -- unambiguous host preemption.
   Lowering it buys events worth ~10us instead of ~1.5ms; raising it to ~1ms
   loses ~18% of events while roughly doubling per-event value and dropping the
   halted-waiter contamination documented in the Phase 0 doc. One-line change,
   worth making regardless.

2. **The pending-blocked head window is a 65x larger opportunity, already
   instrumented.** `ivh_head_yield_ok_tier2_spinning` counts: my `prev` is the
   queue head, head is `HEAD_SPINNING` (pending SET), head reads stale, and
   `lock->locked == 0`. Measured **3,104/s** vs rotation's 47/s. This is the one
   window where stealing CANNOT rescue the lock, because the stale head left
   pending set. See `ivh_head_waiter_adaptive_spinning_design_2026-09-14.md`.
   Its duration must be measured before anything is built -- 3,104/s x 100us
   would be 27% of wall time, which is implausible, so either the windows are
   short or the counter over-counts.

3. **Phase 0b (`ivh_rot_idle_hist[]`) is dead and should not be quoted in either
   direction.** Attribution was 0.027%-0.5%. Only hashed releases stamp; hashing
   requires the head to have already halted; heads halt in ~0.013% of tenures.
   And the interval spans arbitrary stealer traffic, so it measures head-claim
   latency, not lock idle time. Additionally the probe costs ~16% when enabled,
   so every Phase 0b number was taken under heavy perturbation.

## 7. Methodological lessons

- **Fixed arm order fabricated a 14% effect.** Running A,B,C in sequence every
  round put all within-round drift on C. A balanced design measured the order
  effect directly at **+111,306 ops (~3.5%)** and the "13.65% walk cost"
  evaporated. Cycle arithmetic had already said the walk could only be ~0.3%.
  Always alternate.
- **Check the arithmetic against the mechanism before believing a number.**
  Twice today a large measured effect was an artifact that a back-of-envelope
  cycle count would have rejected immediately.
- **Phase 0's "skippable" was 5x too optimistic** because it only proved a live
  node existed, never that the splice was legal. Count the thing you will
  actually be able to do, not the thing that looks promising.
