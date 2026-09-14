# Lock skipping: Phase 0 results, two corrections, and the Phase 0b idle-time probe, 2026-09-14

Kernel `6.17.0-G-LOCK-26-rotprobe+` (measurement), `6.17.0-G-LOCK-27-idleprobe+`
(built this session). Source `/root/kernels/linux-6.17-vanilla`, branch
`ivh-rebuild-main`. Docs `/root/linux-6.17`, branch `kernel-43-clean`.

Continues `ivh_lock_skipping_step0_step1_plan_2026-09-14.md`.

## 1. Phase 0 headline, and why it does not mean what it looked like

Phase 0 counts, at each MCS handoff, whether the promotion target looks stale
and how far back the first live waiter is. Measured:

| workload | handoffs | stale successor | rate |
|---|---|---|---|
| qlockbench (one lock, deep queue) | 9,833,168 | 1,525 | 0.0155% |
| hackbench `-T -g1 -f8 -l400000`   | 4,028,229 | 8,547 | 0.2122% |
| hackbench, repeat run this session| 1,903,859 | 5,919 | 0.3109% |

**Correction 1 — "0.2122% of handoffs hit a preempted successor" is wrong.**
`ivh_rot_stale()` asks only whether the target's heartbeat is older than
`ivh_pv_beat_threshold`. It never consults `pn->state`. The heartbeat is
published only from the two qspinlock spin loops and the `pv_init_node()` seed,
so **a halted vCPU publishes nothing** — the tree says so itself at
`qspinlock_paravirt.h:572` — and `kvm.c:1985` documents a third case: a vCPU
that halted and *already woke* still reads stale until the next tick, up to 1ms
at HZ=1000.

The running threshold is **220,000 cycles = 100µs @ 2.2GHz**, not the
compile-time `IVH_BEAT_THRESHOLD_US` of 1500µs. So any waiter halted >100µs
reads as preempted. Cross-check via `ivh_node_halt_hist[cause][ilog2(cycles)]`,
buckets >= 18 (>= 119µs, guaranteed over the bar), same run:

| | count |
|---|---|
| `ivh_rot_preempted` | 5,919 |
| node halts >= 119µs (always read stale) | **16,917** |
| — of which TIER1 | 11,622 |
| — of which TIER2 | 2,305 |
| — of which EXHAUST | 2,990 |

The contaminating population is **2.86x the entire signal**. The probe is
substantially measuring IVH's own adaptive-spinning halts. Supporting: host
contention was *lower* in this run (hackbench 39.27s vs 53.3s) yet the rate went
*up*, 0.2122% -> 0.3109% — backwards for host preemption, consistent with halts.

Also invalid: the "174 events/s x 1ms = 17.4% of wall time" arithmetic that
motivated the cost question. hackbench spreads over thousands of distinct
pipe/socket locks; summing dead time across unrelated locks and dividing by wall
time corresponds to no throughput loss on any single lock.

**Correction 2 — the "86% had no live node to skip to" reading was wrong.**
All 5,100 no-live events were `tail_stop` (`n->next == NULL`), zero were "8 hops
all stale". So it is not a chain of halted waiters; the stale successor was
simply the last node in the queue. And `tail_stop` itself *over*-counts genuine
tails — a waiter that has done `xchg_tail()` but not yet stored `prev->next`
leaves the link transiently NULL (comment at `qspinlock_paravirt.h:1063`). So:

| | hackbench | qlockbench |
|---|---|---|
| stale events with a live node behind (skippable) | 819 = **13.8%** | 1,096 = **71.9%** |
| no live node found | 5,100 = 86.2% | 429 = 28.1% |

13.8% is a **floor**, not a ceiling. qlockbench, with a genuinely deep queue, is
where the mechanism has room to act.

## 2. Does the halted/preempted contamination matter for the decision?

**No, and this was settled by the user, correctly.** If the successor's TSC is
stale it will not take the lock promptly, whatever the reason, so it should be
skipped either way. The reason is stronger than it first appears:
`pv_kick_node()` does **not** kick — the only `pv_kick()` is at
`qspinlock_paravirt.h:1331`, in `__pv_queued_spin_unlock_slowpath()`. So a
halted successor is promoted, then stays asleep until the promoter finishes its
critical section and unlocks, and only then gets an IPI. **The lock is genuinely
free and unusable for that whole kick-plus-wake window.** Skipping to a spinning
waiter avoids the round trip entirely.

What the distinction still affects is *payoff*, not the decision: at IPI-wake
scale (~5-20µs) 819 events/39s is 0.01-0.04% of wall time; at preemption scale
(~1ms) it is ~2%. Hence Phase 0b.

## 3. Phase 0b: what to measure, and the metric that was rejected

**Rejected: promotion -> ack (MCS baton latency).** A promoted successor does not
acquire the lock; it becomes queue head and spins. If the promoter is still
inside its critical section for the successor's whole absence, *nothing* is
wasted. Worse, per the `pv_kick_node()` fact above, a halted successor's
promotion->ack interval is bounded below by the entire critical section, at zero
cost. Timing that would have produced a large, tight, confident and wrong
"excess", inflating exactly the population Phase 1 wants to act on.

**Built instead: lock idle time** — release -> the queue head claims it.

## 4. Design, and the three things review caught

- `u8 rot_flags` in `struct pv_node`'s 3-byte padding hole at offset 21.
  `sizeof` stays 32, `head_ctl` stays at 24, the `BUILD_BUG_ON` against
  `struct qnode` still holds, no cacheline layout changes. The promoter's
  deposit is free: `arch_mcs_spin_unlock_contended()` takes that line exclusive
  two instructions later anyway.
- **Stamp the TARGET, not the releaser.** First implementation keyed the release
  stamp by releasing CPU. That proves only "the last hashed release that CPU did
  was on a lock at this address" — every intervening fast-path release leaves
  the old stamp standing, so a match can span unbounded later tenures, and class
  0 is inflated arbitrarily. Fixed by stamping
  `per_cpu(ivh_rot_rel, node->cpu)` using the `node = pv_unhash(lock)` that
  `__pv_queued_spin_unlock_slowpath()` already holds. The reader then consumes
  its own local slot — no remote load on the acquisition path, and the `prev`
  plumbing added to `qspinlock.c` was reverted (that file is now pure additions).
- Stamp is **single-use** (consumed by clearing `->lock`), ordered
  `smp_wmb()`/`smp_rmb()`, and capped at 100x the staleness threshold to reject
  freed-and-reallocated locks matching a surviving stamp. Negative and capped
  intervals are *counted*, not silently dropped: the discards skew short, so
  dropping them quietly would shift every mean upward.

**Coverage limit, not papered over.** On x86-64 `__pv_queued_spin_unlock()` is
hand-written assembly (`PV_UNLOCK_ASM`), so only the hashed `_Q_SLOW_VAL`
release path is reachable from C. A release is hashed only when
`pv_kick_node()`'s `cmpxchg(&pn->state, VCPU_HALTED, VCPU_HASHED)` succeeded —
i.e. only when the successor had already halted. **Every sample is a halted
head.** A head the host descheduled mid-spin never halts, is never hashed, and
is invisible. Instrumenting it would mean an `rdtsc` on every unlock in the
kernel.

**Consequence for reading the result — this is not a baseline subtraction.**
Since class 0 is also an asleep head, `class_N - class_0` is meaningless: both
terms are dominated by the same kick + wake round trip. What is meaningful is
the **absolute** idle time of class `STALE|SKIPPABLE`, where the promoter saw a
stale successor *and* a live node behind it: rotation would have handed the lock
to a spinning waiter that takes it at once, so the whole interval is
recoverable with nothing to subtract.

## 5. Controls added

- `ivh_rot_steals` — `pv_hybrid_queued_unfair_trylock()` successes.
  `CONFIG_LOCK_EVENT_COUNTS` is off in this build so upstream's
  `pv_lock_stealing` compiles away. This is the control the whole question turns
  on: a steal is a dead head's cost *already* being recovered without rotation.
  Counted only when `val & _Q_TAIL_MASK` — otherwise it also counts the ordinary
  uncontended post-`pv_init_node()` retry, which is not a steal from anyone.
  Note the premise correction: that trylock is called from `qspinlock.c:321` and
  `:349`, the entry path of *newly arriving* waiters, **not** from
  `pv_wait_head_or_lock()`. Its steal loop runs precisely while pending is clear
  — i.e. exactly when the head is dead. A dead head costs tail latency and
  fairness, not throughput.
- `ivh_rot_idle_unknown` / `_backward` / `_capped`, and a reader summary
  printing `attributed / acks`, flagging <80% as not quotable.

## 6. Status

Built as `6.17.0-G-LOCK-27-idleprobe+`. Not yet run — no data in this doc for
Phase 0b itself. `ivh_pv_rot_probe` defaults to 0.
