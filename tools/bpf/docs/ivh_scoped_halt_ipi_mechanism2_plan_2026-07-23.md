# IVH `ivh_pv_wait_mechanism=2` REPLACEMENT: "scoped halt / IPI kick"

Date: 2026-07-23
Branch: `kernel-43-clean`
Booted kernel at time of writing: `6.17.0-rseqport57-tpauseIPI+`
Status: **implementation plan** — no build, no reboot, no live test performed.
This plan is written to be handed to a different agent (Sonnet) to implement
directly. It supersedes the *sleep-trigger semantics* of
`ivh_halt_ipi_mechanism2_implementation_2026-07-22.md` (mechanism 2 as originally
built). It is grounded in the code as it exists right now in this tree.

---

## 0. One-paragraph summary of the change

Mechanism 2's **wake side stays exactly as already built** (two-stage plain-IPI:
`pv_kick_node()`'s `smp_send_reschedule(pn->cpu)` at MCS handoff, and
`ivh_pv_kick()`'s `smp_send_reschedule(cpu)` at release). Its **sleep side changes
in exactly one place**: a queued MCS waiter in `pv_wait_node()` must go to sleep
(real HLT) **only when `pv_wait_early()` has fired** (i.e. the predecessor `prev`
looks unhealthy — host-preempted per the steal bit, or MCS-halted), and must
**never** sleep merely because the fixed `SPIN_THRESHOLD` inner loop ran out while
`prev` still looked healthy. When the threshold runs out with `prev` healthy,
mechanism 2 loops back and keeps spinning. This is achieved by **adding one
guarded `continue`** in `pv_wait_node()`, gated on `ivh_pv_wait_mechanism == 2`,
so mechanisms 0 and 1 keep their current "spin the threshold, then sleep
regardless" behavior byte-for-byte. **No logic changes to `arch/x86/kernel/kvm.c`
at all** — the mechanism-2 HLT body there is reused verbatim; it is simply reached
from `pv_wait_node()` on a narrower trigger. Comments are updated in three places.

**Do NOT delete the mechanism-2 HLT branch in `kvm.c` (`ivh_pv_wait()`,
lines 1256–1268).** "Remove the already-built version 2" means remove its
*unconditional-sleep-trigger semantics*, which live in the **absence of a gate in
`pv_wait_node()`** — so the fix is to **add** the gate in
`qspinlock_paravirt.h`, not to remove code in `kvm.c`. The halt sequence, its
lost-wakeup guard, and the IPI wake are all kept.

---

## 1. Current control flow, verbatim (the thing being changed)

`kernel/locking/qspinlock_paravirt.h`, `pv_wait_node()` (lines 315–373). Reduced
to the load-bearing skeleton with real line numbers:

```c
static void pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct pv_node *pp = (struct pv_node *)prev;
	bool wait_early;
	int loop;

	for (;;) {                                                       /* 322 OUTER */
		for (wait_early = false, loop = SPIN_THRESHOLD; loop; loop--) { /* 323 INNER */
			if (READ_ONCE(node->locked))
				return;                                  /* 325 got the baton */
			if (pv_wait_early(pp, loop)) {                   /* 326 prev unhealthy? */
				wait_early = true;
				break;                                   /* 328 exit inner EARLY */
			}
			cpu_relax();                                     /* 330 */
		}
		/* reach here two ways:
		 *   (a) inner EARLY break  -> wait_early == true   (prev looked unhealthy)
		 *   (b) inner EXHAUSTED    -> wait_early == false  (loop hit 0, prev healthy) */

		smp_store_mb(pn->state, VCPU_HALTED);                    /* 342 publish HALTED */

		if (!READ_ONCE(node->locked)) {                          /* 344 re-check */
			lockevent_inc(pv_wait_node);
			lockevent_cond_inc(pv_wait_early, wait_early);
			pv_wait(&pn->state, VCPU_HALTED);                /* 347 SLEEP (-> ivh_pv_wait) */
		}

		cmpxchg(&pn->state, VCPU_HALTED, VCPU_RUNNING);          /* 355 */
		lockevent_cond_inc(pv_spurious_wakeup,
				  !READ_ONCE(node->locked));             /* 364 */
	}                                                                /* loop OUTER */
}
```

The defect for mechanism 2: path **(b)** (threshold exhausted, `prev` healthy)
still falls through to `smp_store_mb(VCPU_HALTED)` + `pv_wait()` — an
unconditional HLT that the "scoped halt" design must eliminate. `pv_wait_early()`
already returns the exact signal we want (see §3); `wait_early` already records
whether it fired. We only need to stop path (b) from sleeping **for mechanism 2**.

Confirmed handoff ordering (why the spinning path is safe), from
`kernel/locking/qspinlock.c`:

```c
405	arch_mcs_spin_unlock_contended(&next->locked);   /* store-release next->locked = 1 */
406	pv_kick_node(lock, next);                        /* only THEN try to advance/kick */
```

So `node->locked` is set to 1 **before** `pv_kick_node()` runs. A mechanism-2
waiter that is still spinning (never halted, `pn->state == VCPU_RUNNING`) will
observe `node->locked == 1` at line 324 on its very next inner iteration and
`return` — with **no dependence on any kick**. This is exactly the
"vCPU was running and will observe its next->locked value and advance itself"
case upstream already documents in `pv_kick_node()` (line 388–390).

---

## 2. THE CHANGE — `pv_wait_node()`, add one guarded `continue`

**File:** `kernel/locking/qspinlock_paravirt.h`
**Function:** `pv_wait_node()`
**Insert point:** immediately after the inner `for` loop closes (current line 331,
the `}` of the inner loop) and **before** `smp_store_mb(pn->state, VCPU_HALTED)`
(current line 342). i.e. into the comment gap at lines 332–341.

Add exactly this block:

```c
		/*
		 * IVH mechanism 2 ("scoped halt"): only ever go to sleep
		 * (real HLT via pv_wait()/ivh_pv_wait()) when pv_wait_early()
		 * actually fired -- i.e. wait_early == true, meaning the
		 * predecessor `prev` we are queued behind looks unhealthy
		 * (host-preempted per vcpu_is_preempted(prev->cpu), or itself
		 * MCS-halted per prev->state != VCPU_RUNNING; pv_wait_early()
		 * folds both, and both are genuine "prev is not making progress
		 * for us" signals). If instead the inner SPIN_THRESHOLD loop
		 * merely EXHAUSTED while `prev` still looked healthy
		 * (wait_early == false), do NOT fall through to HLT: loop back
		 * to the outer for(;;), re-arm SPIN_THRESHOLD, and keep
		 * spinning. This degrades to a plain MCS busy-poll on
		 * node->locked -- which is bounded by prev's FIFO progress
		 * exactly like native qspinlock -- rather than paying an
		 * unwarranted HLT vmexit + host reschedule when nothing is
		 * actually wrong. pn->state stays VCPU_RUNNING throughout the
		 * spin, so a concurrent pv_kick_node() cmpxchg(HALTED->HASHED)
		 * fails and correctly leaves us to self-observe node->locked.
		 *
		 * Scoped to mechanism 2 ONLY. Mechanisms 0 and 1 keep their
		 * existing behavior unchanged: fall through and sleep once the
		 * threshold is spent regardless of wait_early (mechanism 0's
		 * host-cooperative HLT/kick, mechanism 1's bounded TPAUSE poll).
		 * The `== 2` conjunct is what preserves that.
		 */
		if (!wait_early && READ_ONCE(ivh_pv_wait_mechanism) == 2)
			continue;
```

That is the entire functional change. `continue` re-enters the outer `for (;;)`
(no loop-increment expression), which re-runs the inner loop with a fresh
`loop = SPIN_THRESHOLD` and `wait_early = false`. No new variable, no new
function, no change to the sleep body, the state machine, or the wake side.

Why `wait_early` is the correct predicate (not a fresh
`vcpu_is_preempted(prev->cpu)` re-read): `wait_early` is initialized `false` at
the top of every inner loop (line 323) and set `true` **only** on the early
`break` at line 327–328, which is taken **only** when `pv_wait_early(pp, loop)`
returned true. So after the inner loop, `wait_early == true` iff `pv_wait_early()`
fired, and `wait_early == false` iff the loop exhausted. It is a precise,
already-present record of "did the prev-unhealthy check trip." Re-reading the
steal bit here instead would duplicate `pv_wait_early()`'s internals for no gain.

---

## 3. Why `pv_wait_early()` is the right gate, and stays UNCHANGED

`pv_wait_early()` (lines 263–295) already computes exactly the signal the design
calls for. Its body, in effect:

```c
	if ((loop & PV_PREV_CHECK_MASK) != 0)   /* 266 only check every 256 iters */
		return false;
	if (READ_ONCE(prev->state) != VCPU_RUNNING)  /* 269 prev MCS-halted */
		return true;
	if (!READ_ONCE(ivh_pv_wait_mechanism))       /* 291 mech 0 = stock upstream */
		return false;
	return vcpu_is_preempted(prev->cpu);         /* 294 mech !=0: host steal bit */
```

For mechanism 2 this returns true when **either** `prev` is MCS-halted (stock
adaptive-spin signal) **or** `prev`'s vCPU is host-preempted (the IVH steal-bit
signal — `vcpu_is_preempted(prev->cpu)`, reading the same `struct kvm_steal_time`
the IVH migration core already depends on, per the user's explicit sign-off).
Both are legitimate "prev is not going to hand me the lock soon" conditions; the
"scoped halt" trigger is their disjunction, which is exactly `wait_early`.

**No change to `pv_wait_early()`'s body.** Its existing mechanism-gate
(`if (!READ_ONCE(ivh_pv_wait_mechanism)) return false;`) keeps mechanism 0 at
stock upstream behavior and gives mechanisms 1 and 2 the steal-bit check. The
only edit here is to **tighten the comment** (lines 273–289) to state that, under
the new design, for mechanism 2 this early-bail is now the *sole* path into HLT
(cross-reference the `continue`-gate in `pv_wait_node()`), whereas previously
mechanism 2 also reached HLT via threshold exhaustion.

---

## 4. `arch/x86/kernel/kvm.c` — NO logic change (item 2 answered)

### `ivh_pv_wait()` mechanism-2 branch (lines 1256–1268): keep verbatim

The real-HLT body — the `irqs_disabled()` / `safe_halt()` split with the
`READ_ONCE(*ptr) == val` lost-wakeup re-check — is **reused unchanged**. Under the
new design this branch is simply *reached less often from `pv_wait_node()`*: only
after `wait_early`, never after bare threshold exhaustion. Nothing about *how* it
halts, or *when it is called given that it is called*, needs to differ. The
caller (`pv_wait_node()`) already does the check-then-publish
(`smp_store_mb(VCPU_HALTED)` then `if (!READ_ONCE(node->locked))`) before
invoking `pv_wait()`, and that sequence is untouched — it still runs on the
`wait_early` path. So the missed-wakeup interlock is preserved exactly (see §6).

**Comment-only update** at the mechanism-2 branch (lines 1212–1255): the current
comment describes mechanism 2 as "the yield like mechanism 0 but hypercall-less"
and implies it halts on the same trigger as mechanism 0. Update it to note that,
as called from `pv_wait_node()` (role C, queued node), the halt is now **scoped**:
it is reached only when `pv_wait_early()` fired for the predecessor — see the
`continue`-gate in `qspinlock_paravirt.h:pv_wait_node()`. Keep the entire
correctness discussion (HLT traps without PV_UNHALT; IPI un-halts; timer-tick
backstop; lost-wakeup guard) — all of it still holds.

### `pv_wait_head_or_lock()` path (role A, queue head): intentionally UNSCOPED

`pv_wait_head_or_lock()` (lines 454–538) also calls
`pv_wait(&lock->locked, _Q_SLOW_VAL)` (line 522), which routes to `ivh_pv_wait()`.
The queue head has **no `prev`** to steal-check — it races the holder directly and
waits on `lock->locked` itself ("holder released" *is* its wait condition), woken
by `ivh_pv_kick()`'s release IPI. There is no `vcpu_is_preempted()` signal
available or meaningful there. So the head waiter keeps the **unconditional
"spin SPIN_THRESHOLD then real-HLT"** structure (identical to mechanisms 0/1
structurally) for mechanism 2. **Leave `pv_wait_head_or_lock()` unchanged.**

**This asymmetry is intentional and known, not an oversight:** role C (queued MCS
node, has `prev`) gets scoped/adaptive halting; roles A/B (queue head / pending,
no `prev`) do not, because they have no predecessor signal to scope against and
their wake is the release event itself. State this explicitly in the comment
update so a future reader does not "fix" the head path to match. (This matches
the reasoning already settled in
`ivh_halt_ipi_and_tsc_next_steps_2026-07-23.md`, Point 1.)

### `ivh_pv_kick()` (lines 1296–1343): NO change

Wake side is unchanged by design. The mechanism-0 branch (PV_UNHALT hypercall)
and the nonzero fall-through (`smp_send_reschedule(cpu)`) both stay. A comment
refresh is optional; the existing comment is still accurate.

---

## 5. Every `ivh_pv_wait_mechanism` / `mechanism==2` reference, accounted for (item 4)

Grepped across `arch/x86/kernel/kvm.c`, `kernel/locking/qspinlock_paravirt.h`,
`kernel/locking/qspinlock.c`:

| Location | What it is | Action |
|---|---|---|
| `kvm.c:1118` | `unsigned long ivh_pv_wait_mechanism = 0UL;` decl | none |
| `kvm.c:1123-1128` | sysctl table entry, `proc_doulongvec_minmax`, no extra1/extra2 | none — value `2` already accepted, no clamp (verified in doc #1 §2) |
| `kvm.c:1180-1210` | `mechanism==0` block in `ivh_pv_wait()` | none |
| `kvm.c:1212-1255` | mechanism-2 explanatory comment | **comment update** — note scoped trigger from role C, unscoped from role A |
| `kvm.c:1256-1268` | mechanism-2 real-HLT branch body | **none** — reused verbatim |
| `kvm.c:1270-1293` | `mechanism==1` TPAUSE deadline poll | none |
| `kvm.c:1306-1310` | `ivh_pv_kick()` `mechanism==0` branch | none |
| `kvm.c:1312-1342` | `ivh_pv_kick()` nonzero `smp_send_reschedule` + comment | none (optional comment refresh) |
| `kvm.c:1357,1361,1391` | `kvm_spinlock_init()` comments / `pr_info` | none — generic, still accurate |
| `qspinlock_paravirt.h:263-295` | `pv_wait_early()` body + comment | body **none**; **comment tighten** (mech 2: this bail is now the sole HLT path) |
| `qspinlock_paravirt.h:322-366` | `pv_wait_node()` | **THE CHANGE — add guarded `continue`** (§2) |
| `qspinlock_paravirt.h:382-445` | `pv_kick_node()` body + comment | body **none** (cmpxchg-fail early-return already handles a spinning role-C successor); optional one-line comment note |

No stale mechanism-2 semantics remain in logic. The only comments describing the
old "unconditional halt, same trigger as mechanism 0" semantics are the
`kvm.c:1212-1255` block and the `pv_wait_early` comment — both flagged for update
above.

---

## 6. Correctness invariants — re-derived against the NEW control flow (item 5)

### 6.1 No permanent hang / progress is bounded

Two mutually exclusive cases after the inner loop, for mechanism 2:

- **`wait_early == true` (prev unhealthy):** we fall through and HLT exactly as
  the already-built mechanism 2 does. Bound: the halted vCPU un-halts on the
  **next timer tick** at the latest (HLT participates in the normal interrupt/tick
  path; `CONFIG_HZ=1000` ⇒ ≤1 ms), independent of any IPI — the existing,
  unchanged backstop. On wake the outer `for(;;)` re-checks `node->locked` and
  retries. **Bounded.**
- **`wait_early == false` (prev healthy):** we `continue` and spin. Bound: the MCS
  queue is strict FIFO; `prev` (running, not preempted) will acquire, release, and
  `arch_mcs_spin_unlock_contended(&node->locked)` set our `node->locked = 1`
  (`qspinlock.c:405`), which our inner-loop poll at line 324 observes and
  `return`s on. **No IPI and no halt are required for this progress.** This is
  precisely native qspinlock's own MCS spin, which is the accepted baseline. If
  `prev` transitions from healthy to preempted mid-spin, the next
  `pv_wait_early()` sample (every 256 iters) flips `wait_early` true and we halt —
  self-correcting. **Bounded.**

The removed fixed-threshold fallback bounded only the *decision to sleep*; it was
never the bound on *spin progress*. Removing it as a sleep-trigger removes no
useful liveness bound: when we don't halt, we are polling `node->locked` every
iteration, which is bounded by prev's FIFO completion. The only unbounded scenario
— `prev` runs forever without releasing — is a lock-holder-holds-forever bug that
hangs *every* spinlock waiter (native included); it is not new to mechanism 2 and
is out of scope.

### 6.2 No missed wakeup / lost-wakeup race

- **On the halt path (`wait_early`):** the interlock is unchanged.
  `smp_store_mb(pn->state, VCPU_HALTED)` (full barrier) publishes HALTED, then
  `if (!READ_ONCE(node->locked))` re-checks before `pv_wait()`; inside
  `ivh_pv_wait()` the mechanism-2 branch re-checks `READ_ONCE(*ptr) == val` under
  IRQs-off and uses `safe_halt()`'s atomic `sti;hlt` shadow (or bare `halt()` with
  a pending IPI un-halting despite IF=0). The waker side
  (`pv_kick_node` cmpxchg-then-IPI, `unlock` store-release-then-`ivh_pv_kick`)
  publishes the condition word before the IPI. All of this is byte-for-byte the
  already-audited mechanism-2/upstream sequence. **Unchanged ⇒ preserved.**
- **On the spin path (`continue`, `pn->state == VCPU_RUNNING`):** there is *no
  halt*, hence no check-then-halt window to lose a wake in. Concurrency with the
  waker: `qspinlock.c` sets `node->locked = 1` (store-release, line 405) **before**
  `pv_kick_node()` (line 406). `pv_kick_node()`'s
  `try_cmpxchg_relaxed(&pn->state, &old=VCPU_HALTED, VCPU_HASHED)` **fails** (state
  is RUNNING), so it early-returns at line 403 — no hash, no `_Q_SLOW_VAL`, no IPI.
  Our spinning waiter independently observes the already-store-released
  `node->locked == 1` and returns. No wake needed, none lost. This is the exact
  "running successor self-advances" contract upstream already relies on.

### 6.3 No double-wake / double-halt

The `continue` only *skips* the halt for the not-`wait_early` mechanism-2 case; it
introduces no additional halt and no additional wake. When mechanism 2 does halt
(`wait_early`), it is a single halt matched by the single existing two-stage wake.
A mechanism-2 waiter is HALTED for at most one `pv_wait()` call per outer-loop
iteration, exactly as before. No new state transition is added.

### 6.4 Live sysctl-toggle safety

`ivh_pv_wait_mechanism` is `READ_ONCE`-sampled at each site; the new `== 2` read
in `pv_wait_node()` is one more such sample. A toggle between a waiter's spin
decision and a later loop iteration only changes which branch the *next* iteration
takes — every combination self-corrects on the outer `for(;;)`:
a waiter that was spinning (mech 2) and finds the sysctl now 0/1 on its next pass
simply falls through and sleeps as 0/1 dictate; a waiter that halted under mech 2
and is later kicked under any mechanism still wakes (nonzero fall-through IPI, or
mech-0 hypercall/tick backstop). No combination strands a vCPU. The boot-only,
genuinely dangerous toggle (`virt_spin_lock_key` / `pv_ops` registration) is
untouched. **Preserved.**

---

## 7. Honest caveats (brutal-honesty section — do not gloss)

1. **Mechanism 2 now never yields under pure in-guest contention.** When the lock
   is simply held a while by a *running* holder ahead of us (no host preemption
   anywhere in the chain), `wait_early` stays false and mechanism 2 **spins
   indefinitely** where mechanism 0 would HLT after `SPIN_THRESHOLD`. This is the
   *designed* "scoped halt" semantics — yield only when there is actual preemption
   to get out of the way of — but it is a real behavioral divergence from
   mechanism 0: under heavy non-preemption contention, mechanism 2 will burn more
   busy-spin CPU (and never cede the pCPU for other guest tasks / power) than
   mechanism 0. It degrades to *native qspinlock* spin behavior, not to mechanism
   0. For the IVH LHP thesis this is defensible and arguably correct; as a raw
   CPU-utilization number under contention it may look *worse* than mechanism 0.
   Measure and report this honestly; do not expect a universal win.

2. **The sleep trigger folds in the stock `prev->state != VCPU_RUNNING` check,
   not only `vcpu_is_preempted()`.** We gate on `wait_early`, which is true for
   *either* sub-condition of `pv_wait_early()`. A strict "only
   `vcpu_is_preempted(prev->cpu)`" trigger would require duplicating
   `pv_wait_early()`'s internals into `pv_wait_node()`. That is not recommended
   (code duplication of an audited helper, and the stock "prev is MCS-halted"
   signal is itself a valid "prev won't hand me the lock soon" condition). If the
   user specifically wants the narrower trigger, that is a deliberate follow-up,
   not this plan. Flagged so the semantics are explicit.

3. **Expected performance on THIS host is parity-to-slightly-worse vs mechanism 0**
   (carried from the prior docs): mechanism 0 already yields via its own HLT here,
   so the only edge the scoped design can claim is *avoiding HLT round-trips that
   were not warranted* (not halting when `prev` was healthy and the lock was about
   to come free) — a modest efficiency claim, not a throughput win. The genuine
   win case (host without PV_UNHALT, where mechanism 0 busy-spins) is unchanged
   from doc #1 §6 and still requires a PV_UNHALT-disabled test config to show.

---

## 8. Confidence & difficulty assessment

**Confidence the plan is clean and buildable: high.** The functional change is a
single guarded `continue` at a well-defined insertion point, using a predicate
(`wait_early`) that already exists and already means exactly what we need, gated so
mechanisms 0/1 are provably untouched (the `&& == 2` conjunct). No new variables,
no `kvm.c` logic change, no wake-side change, no new locking or ordering. It will
compile with an incremental `kvm.o` + `qspinlock.o` build (mechanism-2 body and
all symbols already exist).

**Trickiest part:** convincing oneself that a *perpetually spinning, never-HALTED*
role-C waiter (the new steady state when `prev` is healthy) is race-free against
`pv_kick_node()`. It is, and non-obviously so: the safety rests on the
`qspinlock.c:405` store-release of `node->locked` happening *before* the
`qspinlock.c:406` `pv_kick_node()` call, and on `pv_kick_node()`'s
`cmpxchg(VCPU_HALTED→VCPU_HASHED)` *failing* for a RUNNING waiter (so it sends no
IPI and does no hashing). The spinning waiter then self-observes the already-set
`node->locked` — the exact "running successor advances itself" contract upstream
documents. This is verified against the real call ordering, not assumed.

**No part of this design turned out harder or riskier than the task framing
implied.** The one thing worth the reader's attention is the §7.1 behavioral
divergence (never yields under pure contention), which is a *semantics* fact to
report, not an implementation difficulty.

---

## 9. Implementer checklist (hand to Sonnet)

1. `kernel/locking/qspinlock_paravirt.h`, `pv_wait_node()`: insert the guarded
   `continue` block from §2 between the inner-loop close (line 331) and
   `smp_store_mb(pn->state, VCPU_HALTED)` (line 342). This is the only functional
   edit.
2. Same file, `pv_wait_early()` comment (lines 273–289): tighten to state that for
   mechanism 2 this early bail is now the *sole* path into HLT (xref the new
   `continue`-gate). No body change.
3. Same file, `pv_kick_node()` comment (optional, lines ~416–442): add one line
   noting a mechanism-2 role-C successor is now often still spinning
   (`VCPU_RUNNING`), whereupon the `cmpxchg` fails and it self-advances — already
   correct, just make it explicit. No body change.
4. `arch/x86/kernel/kvm.c`, `ivh_pv_wait()` mechanism-2 comment (lines 1212–1255):
   update to describe the scoped trigger (role C) vs the still-unconditional head
   path (role A), and mark the asymmetry intentional. **Do not touch the HLT body
   (lines 1256–1268).** No other `kvm.c` change.
5. Incremental build check only:
   `sudo make -j$(nproc) arch/x86/kernel/kvm.o kernel/locking/qspinlock.o`,
   then chown artifacts back to `nick:nick`
   (`find arch/x86/kernel kernel/locking -user root`). Full rebuild + reboot is
   the user's step.
6. Sanity after the user reboots: `sysctl kernel.ivh_pv_wait_mechanism=2` must read
   back `2`; then A/B `hackbench -g4 -l20000` under a real host co-runner
   (oversubscription), comparing mech 0 vs 2, capturing `ivh_pv_wait_calls` and
   `/proc/interrupts` RES deltas — and per §7.1, also watch guest CPU-util under
   contention, since mechanism 2 may busy-spin more than mechanism 0.
