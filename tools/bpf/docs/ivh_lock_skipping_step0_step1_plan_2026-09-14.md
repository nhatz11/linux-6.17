# Lock skipping, the safe path: an implementation plan for Step 0 and Step 1

2026-09-14. Follow-up to `ivh_lock_skipping_design_discussion_2026-09-13.md`, which found the
originally-proposed queue-reordering design structurally unsafe (a third party cannot rewrite a
node's `next` pointer after some other CPU has already cached that value privately — the failure
mode is a stale handoff corrupting an unrelated lock elsewhere in the kernel, not merely a
double-acquire of the same one) and recommended a two-step safe alternative instead. This is a
concrete plan for that alternative — not yet implemented. All line numbers/symbol names below were
re-verified directly against `/root/kernels/linux-6.17-vanilla` today; a couple of symbol names the
original scoping pass cited (`ivh_cs_head_check()`, `ivh_lock_steals`) do not actually exist in this
tree — the real counter is `lockevent_inc(pv_lock_stealing)` — corrected below.

## Step 0 — detect-only: is there even anything here worth building?

**Goal**: before writing a single line that changes behavior, measure the two numbers that decide
whether Step 1 is worth doing at all: (a) how often does a lock handoff target a successor that
looks preempted, and (b) when it does, how deep into the queue is the nearest non-preempted waiter.
Zero pointer writes, zero behavior change, purely additive counters — same posture as every other
instrumentation this project has shipped this session (`enum pv_bail_cause`, the tier-1-confirm
shadow mode, etc.).

### 3.1 Where it hooks

The natural site is `pv_kick_node()` (`kernel/locking/qspinlock_paravirt.h:888-931`) — this is
already the exact moment of handoff to a specific successor node, already has the
`struct pv_node *pn = (struct pv_node *)node;` cast in hand (so no new cast needed), and is already
PV-only code (native/non-PV qspinlock never calls it), matching the scope of everything else this
project has added to this file.

```c
static void pv_kick_node(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	u8 old = VCPU_HALTED;

	/* --- NEW: Step 0 probe, unconditional cheap check, no pointer writes --- */
	ivh_skip_probe(lock, pn);

	smp_mb__before_atomic();
	if (!try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED))
		return;
	...
```

Note this only fires on the *halted* successor path (the one `pv_kick_node()` actually handles —
see its own comment: a *running* successor observes `next->locked` itself and never reaches this
function at all). That's fine and arguably correct: a running successor was never a candidate for
"skip me, I'm preempted" in the first place, so restricting the probe to exactly the population
that could plausibly be preempted (already halted, waiting to be kicked) is the right scope, not an
accidental narrowing.

### 3.2 What it does

```c
/* New sysctl, default 0 -- same posture as ivh_adaptive_irqoff_bail_gate and
 * ivh_pv_tier1_confirm: a gate for something whose sign is not yet proven.
 * likely(!...) first, exactly like ivh_beat_publish_in_spin(). */
extern unsigned long ivh_pv_skip_probe;   /* arch/x86/include/asm/ivh_tsc_beat.h */

#define IVH_SKIP_HOP_CAP 8   /* hard bound -- a stale/cyclic next chain must not hang this */

static __always_inline void ivh_skip_probe(struct qspinlock *lock, struct pv_node *pn)
{
	struct mcs_spinlock *n;
	int hop;

	if (likely(!READ_ONCE(ivh_pv_skip_probe)))
		return;

	this_cpu_inc(ivh_skip_probe_calls);

	if (!is_wait_preempted(pn->cpu, /* tier2 */ true)) {
		this_cpu_inc(ivh_skip_target_healthy);
		return;
	}
	this_cpu_inc(ivh_skip_target_preempted);

	/* Read-only forward walk. Every ->next is either NULL or a pointer into
	 * a per-CPU qnodes[] slot that is never freed (only reused) -- this can
	 * loop on stale/cyclic data but can never fault. The hop cap is what
	 * makes that survivable: this is diagnostic code, it must never become
	 * an unbounded loop no matter what the queue looks like. */
	n = READ_ONCE(pn->mcs.next);
	for (hop = 0; n && hop < IVH_SKIP_HOP_CAP; hop++) {
		struct pv_node *cand = (struct pv_node *)n;

		if (!is_wait_preempted(cand->cpu, true)) {
			ivh_skip_depth_record(hop);   /* histogram, log2-bucketed like
			                               * the rest of this file's stats */
			return;
		}
		n = READ_ONCE(n->next);
	}
	ivh_skip_depth_record(-1);   /* "no alive candidate within the cap" bucket */
}
```

New counters, all `DEFINE_PER_CPU(u64, ...)` in `arch/x86/kernel/kvm.c` next to the existing
`ivh_beat_*` block, declared in `ivh_tsc_beat.h`:

- `ivh_skip_probe_calls` — how many halted-successor handoffs were checked at all.
- `ivh_skip_target_healthy` / `ivh_skip_target_preempted` — split of (a) above. The ratio
  `ivh_skip_target_preempted / ivh_skip_probe_calls` is the headline number.
- `ivh_skip_depth_hist[IVH_SKIP_HOP_CAP + 1]` — histogram of (b): index 0..7 = hop distance to the
  first alive candidate, index 8 = "none found within the cap." (Small enough to be a flat array,
  not log2-bucketed like the cycle-cost histograms — hop count is already a small bounded integer,
  not a wide-dynamic-range duration.)

### 3.3 What this answers, and the decision rule

Run under real contention (the same `NHextend-full`/hackbench workloads already validated this
session, at a few different thread counts and CS lengths to cover the "valley" region too — this
mechanism's payoff could plausibly itself depend on CS length or thread count, exactly like both of
this session's other two mechanisms did). Read `ivh_skip_target_preempted / ivh_skip_probe_calls`
and the depth histogram.

- **If the preempted-target ratio is small** (say, low single-digit percent) — Step 1 is very
  unlikely to matter; the number of handoffs it could possibly improve is small, and it should be
  documented as a negative result and closed, the same way the tier-1-cascade halt-cost investigation
  was closed earlier this session when its ceiling turned out too small to chase.
- **If it's non-trivial but the depth histogram is dominated by "none found within the cap"** — the
  underlying signal is real but this project's existing `is_wait_preempted()` predicate may be too
  trigger-happy (recall its own documented false-positive/false-negative rates from earlier
  investigations this session) to trust as the sole gate; worth re-examining the predicate before
  investing in Step 1, not the queue-walk depth.
- **If it's non-trivial and alive candidates are typically found at shallow depth (1-3 hops)** —
  Step 1 is justified and likely to pay off with an unfair-steal-window widening (see below).

## Step 1 — if justified: widen the existing unfair-steal window, don't touch the queue

**Core idea**: `pv_hybrid_queued_unfair_trylock()` (`qspinlock_paravirt.h:126-149`) already lets a
brand-new arrival steal the lock ahead of the entire MCS queue, racing the lock word directly
(`try_cmpxchg_acquire(&lock->locked, &old, _Q_LOCKED_VAL)`), as long as no queued waiter has yet set
the pending bit (`_Q_PENDING_MASK`). This is real "skipping" — a later arrival getting the lock
before an earlier, still-queued waiter — implemented the only way that's actually safe: by racing
a single atomic word, not by mutating any node's pointers. It's already instrumented
(`lockevent_inc(pv_lock_stealing)`).

**The lever, precisely**: the steal window closes the moment the queue head sets the pending bit
(`set_pending(lock)`, called from `pv_wait_head_or_lock()` — need to re-locate the exact call site
when this is actually built, since the file has moved code around this session already). The
proposal is: **don't close the window as eagerly when the current head looks preempted.** Concretely,
gate (or delay) the head's own `set_pending()` call on the SAME `is_wait_preempted()` check already
proven out by Step 0 — if the head appears preempted, hold off on claiming the pending bit for a
bounded extra interval, extending the window in which a fresh, running arrival can steal the lock
out from under the whole queue instead of joining it.

This delivers the original intent — "route around a preempted head" — while:

- Touching only a single shared atomic (`lock->val`'s pending bit), which already has well-defined,
  heavily-audited concurrent semantics (the pending bit is `qspinlock`'s own long-standing mechanism
  for exactly this class of tradeoff — see the file's own header comment: *"combines the best
  attributes of a queued lock (no lock starvation) and an unfair lock (good performance on not
  heavily contended locks)"*).
- Touching zero node pointers. Nothing here can produce the cross-lock corruption failure mode from
  §3.2 of the design-discussion doc, because no CPU is ever handed a decision made on its behalf by
  someone else's stale cached state — every steal attempt is a live, self-initiated race against the
  current, real value of `lock->val`, evaluated at the instant of the attempt.
- Being a straightforward, bounded knob (a gate + a delay duration) rather than a new protocol —
  small, reviewable, and default-off (mirroring `ivh_adaptive_irqoff_bail_gate`'s posture: *"the sign
  of this trade is not proven"*).

**The known, already-documented cost of this lever** (same file's own header comment, not new):
widening the unfair-steal window weakens the lock's starvation bound — a queued waiter can, in
principle, be repeatedly overtaken by a stream of fresh arrivals for as long as the window stays
open. This needs to be bounded explicitly (e.g. cap the extra delay, or cap consecutive steals per
queued epoch) before shipping even as a default-off experiment, and should be measured for tail-
latency/fairness regressions, not just throughput, when it's actually built.

### 4.1 Sketch of the sysctl surface (naming only, not final)

```
ivh_pv_skip_probe        0/1   — Step 0's detect-only instrumentation (this doc's §3)
ivh_pv_skip_steal_gate   0/1   — Step 1's master enable
ivh_pv_skip_steal_ns     N     — how long to hold the steal window open when the head looks
                                  preempted, in ns, converted to TSC cycles at read time
                                  (mirrors ivh_pv_beat_threshold's own ns-sysctl-to-cycles pattern)
```

### 4.2 What Step 1 would need to measure before being trusted

- Throughput on the same battery already used for the migration/AFL work: `NHextend-full` across
  the loop_spin sweep (especially the mid-CS "valley" region, since this is yet another
  fixed-cost-vs-contention-level tradeoff and could plausibly show the same non-monotonic shape),
  and hackbench, at minimum.
- Tail latency / fairness, explicitly — this is the one thing neither the migration engine nor the
  AFL work needed to check as carefully (their costs were throughput-shaped, not starvation-shaped).
  A max-wait-time metric per waiter, not just aggregate throughput, is required here specifically
  because of the starvation-bound tradeoff called out above.
- Re-derivation of `ivh_pv_skip_steal_ns`'s right order of magnitude from Step 0's own depth
  histogram (roughly: how long does it typically take for a fresh arrival to show up and race,
  versus how long the current head has already been stale) rather than guessing a value.

## Summary of what's not yet done

Nothing in this doc has been implemented. Step 0 is small (one new inline function, ~6 new per-CPU
counters, one sysctl) and safe to build next; Step 1 is contingent on Step 0's own measured numbers
and should not be started until they're in hand.
