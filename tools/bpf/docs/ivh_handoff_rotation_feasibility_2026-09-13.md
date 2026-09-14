# Handoff-time queue rotation in PV qspinlock: promote the first live waiter

**Date:** 2026-09-13
**Tree read:** `/root/kernels/linux-6.17-vanilla` (6.17 + IVH G-LOCK paravirt
modifications). All line numbers below are from that tree and were verified by
direct `grep -n` / `sed -n`, not inherited from any earlier document.

**Question:**

> Can the existing MCS handoff be changed from "wake `next`" to "find the first
> live waiter, rotate that waiter into `next` position, then wake it," while
> preserving qspinlock's one-head invariant and qnode lifetime guarantees?

---

## VERDICT

**YES. Structurally sound, and it is not even novel — it is ShflLock's
`shuffle_waiters()` splice, performed by the acquirer instead of by a separate
shuffler, with "vCPU currently runnable" substituted for "same socket".**

The rotation `H -> A -> B -> C -> D` ==> `H -> C -> A -> B -> D` is byte-for-byte
the pointer sequence at ShflLock Fig. 4 lines 95-98, evaluated with
`qlast == qnode == H`. It preserves the one-head invariant trivially, because it
changes **which** node is promoted, never **how many**: exactly one
`smp_store_release(&X->locked, 1)` executes, exactly as today.

Two load-bearing corrections to the previous investigation
(`ivh_intelligent_handoff_feasibility_2026-09-13.md`) are established in §1 and
§2 below:

1. Its claim that CNA and ShflLock "never change *who gets the lock next*" is
   **false, for both algorithms.** CNA's whole mechanism is the primary-queue
   head rewriting its own `node->next` before handoff; `cna_lock_handoff()`
   contains an explicit re-load with the comment *"reload @next in case it was
   changed by `cna_order_queue()`"*. ShflLock moves a same-socket waiter into
   `qlast.next` where `qlast` starts as the shuffler's own node.
2. Its claim that the actor would be "**the releaser**, which is not a queue
   member at all and therefore has zero lifetime guarantee" does not apply here.
   `qspinlock.c:455` is executed by the **acquirer**, inside
   `queued_spin_lock_slowpath()`, *before* the critical section runs and *before*
   `__this_cpu_dec(qnodes[0].mcs.count)` at `:468`. At that instruction the actor
   is still a queue member holding its node slot, and it has promoted nobody.
   The CNA structural lifetime argument therefore holds at `:455` **verbatim and
   unweakened**, exactly as it holds at `:410` where CNA uses it.

The previous document's negative verdict is correct for the question it asked
(demote a node that is *already* head, `locked == 1`). That scenario is out of
scope here and is not rescued by anything below.

**Single riskiest part of the implementation:** stale `prev` pointers. After a
rotation, node A's `prev` (captured on its stack at `qspinlock.c:370`) no longer
names its real predecessor. Verified safe in this tree — `grep -n -- '->next'
kernel/locking/qspinlock_paravirt.h` returns **zero hits**, and every use of
`prev`/`pp` (`:668`, `:714`, `:744`, `:746`, `:795`) is a *read* of `pp->state`
or `pp->head_ctl` feeding a halt heuristic, never a write and never a
correctness decision — but it is the property most likely to be broken by a
future PV change, so it must be nailed down by a comment and a debug assertion.

---

## 1. Does CNA change who gets the lock next? (The previous doc's central claim)

**No, it does not preserve the successor. The previous document is wrong here.**

Source: `qspinlock_cna.h` from the LKML v15 series
(*[PATCH v15 3/6] locking/qspinlock: Introduce CNA into the slow path of
qspinlock*, Alex Kogan).

```c
static __always_inline u32 cna_wait_head_or_lock(struct qspinlock *lock,
                                             struct mcs_spinlock *node)
{
        /*
         * Try and put the time otherwise spent spin waiting on
         * _Q_LOCKED_PENDING_MASK to use by sorting our lists.
         */
        while (LOCK_IS_BUSY(lock) && !cna_order_queue(node))
                cpu_relax();

        return 0; /* we lied; we didn't wait, go do so now */
}

static int cna_order_queue(struct mcs_spinlock *node)
{
        struct mcs_spinlock *next = READ_ONCE(node->next);
        struct cna_node *cn = (struct cna_node *)node;
        int numa_node, next_numa_node;

        if (!next)
                return 0;

        numa_node = cn->numa_node;
        next_numa_node = ((struct cna_node *)next)->numa_node;

        if (next_numa_node != numa_node) {
                struct mcs_spinlock *nnext = READ_ONCE(next->next);

                if (nnext)
                        cna_splice_next(node, next, nnext);

                return 0;
        }
        return 1;
}

static void cna_splice_next(struct mcs_spinlock *node,
                            struct mcs_spinlock *next,
                            struct mcs_spinlock *nnext)
{
        /* remove 'next' from the main queue */
        node->next = nnext;
        ...
}
```

`node` here is **the head's own node**. `node->next = nnext` is the head
overwriting its own successor pointer. The loop repeats until a same-node waiter
sits at `node->next`. Then:

```c
static inline void cna_lock_handoff(struct mcs_spinlock *node,
                                 struct mcs_spinlock *next)
{
        u32 val = 1;

        if (node->locked > 1) {
                ...
                /*
                 * We have a local waiter, either real or fake one;
                 * reload @next in case it was changed by cna_order_queue().
                 */
                next = node->next;
                ...
        }
        arch_mcs_lock_handoff(&next->locked, val);
}
```

That comment — *"reload @next in case it was changed by `cna_order_queue()`"* —
is the direct refutation. CNA deliberately discards the successor the generic
code had cached and hands the lock to a different node.

**So "the head changes who gets the lock next, before the handoff store" is
published, reviewed, working practice.** It went fifteen LKML revisions with
Peter Zijlstra and Waiman Long reviewing the pointer protocol; the objections
that stopped it were about NUMA fairness policy and merge appetite, never about
this being unsound.

The only substantive difference between CNA's splice and ours is *where the
displaced waiter goes*: CNA parks it on a secondary circular queue rooted in
`node->locked`, which forces CNA to overload `mcs.locked`, add `encoded_tail` to
the node, replace the tail-clear path, and CAS the lock word in
`cna_splice_head()`. **Our rotation keeps every waiter in the single primary
queue, so none of that machinery is needed.** We are strictly simpler than CNA,
not more complex.

### 1.1 ShflLock also changes who gets the lock next

ShflLock Fig. 4 (verified against the SOSP'19 PDF, text-extracted):

```
59   def shuffle_waiters(lock, qnode, vnext_waiter):
60     qlast = qnode # Keeps track of shuffled nodes
...
77      qcurr = qprev.next
78      if qcurr is None:
79        break
80      if qcurr == lock.tail: # Do not shuffle if at the end
81        break
...
84           if qcurr.skt == qnode.skt:
85             if qprev.skt == qnode.skt: # No shuffling required
86               qcurr.batch = ++batch
87               qlast = qprev = qcurr
89             else: # Other socket waiters exist between qcurr and qlast
90               qnext = qcurr.next
91               if qnext is None:
92                 break
93               # Move qcurr after qlast and point qprev.next to qnext
94               qcurr.batch = ++batch
95               qprev.next = qnext
96               qcurr.next = qlast.next
97               qlast.next = qcurr
98               qlast = qcurr # Update qlast to point to qcurr now
99           else: # Move on to the next qnode
100            qprev = qcurr
```

`qlast = qnode` at line 60: **the shuffler's own node is the initial insertion
anchor.** On the first splice, line 97 is `qnode.next = qcurr` — the shuffler
overwrites its own successor pointer.

Evaluate lines 95-98 on our exact target, with `qnode = qlast = H`,
`qprev = B`, `qcurr = C`, `qnext = D`:

| line | op | result |
|---|---|---|
| 90 | `qnext = qcurr.next` | `D` |
| 95 | `qprev.next = qnext` | `B -> D` |
| 96 | `qcurr.next = qlast.next` | `C -> A` |
| 97 | `qlast.next = qcurr` | `H -> C` |

Final queue: **`H -> C -> A -> B -> D`.** That is the task's target rotation,
character for character, including the preservation of A-before-B.

### 1.2 What ShflLock invariant 1 actually protects

> 1) The successor of the lock holder, if it exists, always keeps its position
> intact in the queue. 2) Only one waiter can be an active shuffler, as
> shuffling is single threaded. 3) Only the head of the queue can start the
> shuffling process. 4) A shuffler may pass the shuffling role to one of its
> successors.

The previous document read invariant 1 as "the head's successor may not move".
It says no such thing. In ShflLock the **lock holder** is the thread inside the
critical section (t0 in Fig. 5); its successor is the **queue head / shuffler**
(t1). Invariant 1 therefore says: *a shuffler may not relocate the node that is
about to be notified by the current holder's unlock.* It protects the shuffler,
not the shuffler's successor.

Fig. 5's own caption settles it: *"(b) t2 becomes active, while t1 continues
shuffling and reaches t4, t1 first moves t4 after t2"* — the shuffler t1
rearranges the nodes directly behind it, changing who acquires after it.

Invariant 1 exists because in ShflLock the shuffler and the unlocking holder are
**different threads racing on the same `next` pointer**: the holder reads
`qnext = qnode.next` (line 33/40) and then writes `qnext.status = S_READY`
(line 42), while a shuffler could concurrently relocate that node. Invariant 1 is
a **mutual-exclusion rule between two concurrent actors**, not a structural
prohibition on reordering.

**Our design satisfies invariant 1 by construction and by a stronger route:
there is only one actor.** The node that performs the rotation and the node that
performs the notification are the same CPU executing straight-line code
(`qspinlock.c:453` -> `:455`). No concurrent shuffler exists because we are not
introducing one. Invariant 2 (single shuffler) and invariant 3 (only the head
originates) are likewise satisfied for free.

---

## 2. The exact handoff point in this tree (Q1)

Verified by `grep -n` against `/root/kernels/linux-6.17-vanilla/kernel/locking/qspinlock.c`:

```
301:	idx = node->count++;
336:	node->locked = 0;
337:	node->next = NULL;
353:	smp_wmb();
362:	old = xchg_tail(lock, tail);
370:		prev = decode_tail(old, qnodes);
373:		WRITE_ONCE(prev->next, node);
376:		arch_mcs_spin_lock_contended(&node->locked);
384:		next = READ_ONCE(node->next);
386:			prefetchw(next);
410:	if ((val = pv_wait_head_or_lock(lock, node)))
413:	val = atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));
438:		if (atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL))
447:	set_locked(lock);
453:		next = smp_cond_load_relaxed(&node->next, (VAL));
455:	arch_mcs_spin_unlock_contended(&next->locked);
456:	pv_kick_node(lock, next);
468:	__this_cpu_dec(qnodes[0].mcs.count);
```

The previous work's identification is confirmed: the one and only promotion
store is `qspinlock.c:455`,
`arch_mcs_spin_unlock_contended(&next->locked)` == `smp_store_release((l), 1)`
(`kernel/locking/mcs_spinlock.h:30-37`; `include/asm-generic/mcs_spinlock.h`
defines `struct mcs_spinlock { next; locked; count; }` and nothing else).

**The single most important structural fact, and the one the previous
investigation missed:**

> `qspinlock.c:455` is **not** on the release path. It is inside
> `queued_spin_lock_slowpath()`, executed by the CPU that has *just acquired*
> the spinlock at `:447` and has *not yet run its critical section*.

This is the standard MCS-inside-qspinlock structure: the acquirer immediately
passes the MCS baton downstream so the next waiter can start spinning on
`lock->locked`, and only then returns to its caller to run the critical section.
The actual unlock path is `__pv_queued_spin_unlock()` /
`__pv_queued_spin_unlock_slowpath()` in `qspinlock_paravirt.h:1123-1195`, which
touches `lock->locked` and the PV hash and **never walks `next` pointers at
all**.

Consequences, all of which the previous doc's §3.3 objection depended on being
false:

* At `:455` the actor still owns its node slot: the release
  `__this_cpu_dec(qnodes[0].mcs.count)` is at `:468`, thirteen lines later.
* At `:455` the actor has promoted nobody. `next->locked` is still 0 for every
  node behind it.
* Therefore the actor at `:455` has **exactly the same queue-position
  entitlement** that CNA's splicer has at `:410`. The only thing that changed
  between `:410` and `:455` is that it now also holds the spinlock word — which
  strictly *adds* exclusion, it does not remove any.

**Can the skip decision be inserted immediately before `:455`?** Yes, and `:455`
is a better site than `:410` for a liveness policy, because the runnability
signal is read at the last possible instant instead of potentially thousands of
cycles before the handoff. (CNA sits at `:410` because NUMA node id is stable
and because it wants to use otherwise-idle spin time; neither reason transfers.)

### 2.1 The lifetime guarantee, proved from this tree

**Claim:** at `qspinlock.c:453-455`, every node reachable from `node` via `next`
is still queued; none can have returned from `queued_spin_lock_slowpath()`; none
can have had its per-CPU slot recycled.

**Proof.** There are exactly three `goto release` sites (`grep -n "goto release"`):

* `:319` — the `idx >= _Q_MAX_NODES` fallback, *before* `xchg_tail()` at `:362`.
  Such a node was never linked into any queue.
* `:346` — the post-init opportunistic `queued_spin_trylock()`, also before
  `:362`. Same.
* `:439` — the uncontended tail-clear, reached only after the node has already
  passed `arch_mcs_spin_lock_contended(&node->locked)` at `:376` (or was the
  first queuer).

Once `WRITE_ONCE(prev->next, node)` at `:373` has executed, the node is blocked
in `arch_mcs_spin_lock_contended(&node->locked)` at `:376`, which is
`smp_cond_load_acquire(l, VAL)` — an unconditional spin until `locked != 0`.
There is no timeout, no signal, no bail-out. **The sole exit from the queue is
`locked` becoming non-zero, and the sole writer of a queued node's `locked` is
its unique predecessor at `:455`.**

Since we are at `:455` and have not written anything yet, no node behind us has
been promoted, so no node behind us can have exited, so no node behind us can
have reached `:468` and freed its slot. QED.

This is CNA's argument verbatim. It did not weaken between `:410` and `:455`.

### 2.2 A second stability property that makes the traversal lock-free

In unmodified qspinlock, a queued node's `next` pointer is **monotonic**: it
starts `NULL` (`:337`, before publication), is written exactly once to a non-NULL
value by the arriving successor (`:373`), and is never written again. Nothing in
`qspinlock_paravirt.h` writes it — confirmed:

```
$ grep -n -- '->next\|mcs\.next' kernel/locking/qspinlock_paravirt.h
(no output)
```

So a scan `H->next`, `A->next`, `B->next`, ... reads values that cannot change
under it, and each non-NULL pointer it obtains is an address-dependent load that
orders the subsequent reads of that node's fields (the enqueuer's `smp_wmb()` at
`:353` orders `pv_init_node()`'s `pn->cpu` / `pn->state` stores before both
`xchg_tail()` at `:362` and `WRITE_ONCE(prev->next, node)` at `:373`).

The rotation is the first thing that would make a `next` pointer mutable after
publication. That mutation is performed by a single actor (the head), under the
exclusion of §2.1, on nodes that provably cannot be reading their own `next` —
see §4.

---

## 3. The rotation itself (Q2, Q6)

### 3.1 The pointer sequence — the brief's version is incomplete

The task sketched:

```c
after_c = C->next;   /* D */
B->next  = after_c;  /* B -> D */
C->next  = A;        /* C -> A */
```

**This is missing the store that reinserts C, and as written it is a
use-after-free-class bug.** After those three stores the queue reads
`H -> A -> B -> D` with `C` pointing into it but reachable from nothing. C is
then permanently orphaned: nobody will ever write `C->locked`, and C's CPU spins
forever at `qspinlock.c:376` with preemption disabled. The required fourth store
is `H->next = C`, and it is the *last* one:

```c
	/* invariants checked before any store: see 3.2 */
	a = READ_ONCE(node->next);        /* A  (== the cached `next`)        */
	/* ... scan established b, c with b->next == c, c->next == d != NULL   */
	d = READ_ONCE(c->next);           /* D, known non-NULL                */

	WRITE_ONCE(b->next, d);           /* 1. unlink C from between B and D */
	WRITE_ONCE(c->next, a);           /* 2. C now heads the skipped run   */
	WRITE_ONCE(node->next, c);        /* 3. splice C in behind us         */
	next = c;                         /* 4. hand off to C, not to A       */
```

Stores 1-3 are ShflLock lines 95-97 in order; store 4 is CNA's
`next = node->next` reload in `cna_lock_handoff()`. Both halves of the
implementation already exist in published, reviewed code.

Degenerate cases fold in cleanly:

* `c == a` (the immediate successor is already live): no stores at all, normal
  handoff. This must be the fast path; it is the common case.
* `b == a` (skip exactly one, `H -> A -> B -> C` becomes `H -> B -> A -> C`):
  store 1 becomes `a->next = d`, store 2 `b->next = a`, store 3 `node->next = b`.
  Self-consistent; no special-casing needed.

### 3.2 Preconditions, and why each is needed

Before any store, the scan must have established, all by `READ_ONCE`:

| # | precondition | why |
|---|---|---|
| P1 | `a = node->next` is non-NULL | guaranteed by `:453`'s `smp_cond_load_relaxed` |
| P2 | every scanned node's `next` was non-NULL when read | otherwise we walked off the end |
| P3 | **`c->next != NULL`** (i.e. `d` exists) | **the tail rule** — see §5 |
| P4 | `b->next == c` and `c` was reached from `a` by `next` hops | the chain is what we think it is |
| P5 | scan length `<= IVH_ROT_MAX_HOPS` | bounded work while holding the lock |

P3 is the only non-obvious one and it is the same guard as CNA's `if (nnext)`
and ShflLock's lines 80 and 91-92.

`b` never needs a separate tail check: `b->next == c != NULL`, so `b` is
provably not the tail. `node` (== H) never needs one either: `node->next == a
!= NULL`.

### 3.3 Ordering of the four stores

The three pointer stores are plain `WRITE_ONCE`; no barriers between them are
required, because **no other agent can observe the intermediate states** (§4).
The single barrier that matters is the one already present: store 4's
`smp_store_release(&c->locked, 1)` at `:455` orders all three pointer stores
before the promotion, and C's `smp_cond_load_acquire(&node->locked, VAL)` at
`:376` pairs with it, so C is guaranteed to see `c->next == a` by the time it
reads its own `next` at `:384`/`:453`.

This is the same release/acquire pairing qspinlock already relies on; the
rotation adds no new ordering requirement whatsoever.

### 3.4 Order after the promoted waiter completes (Q6)

Final structure `H -> C -> A -> B -> D`:

* C is promoted, becomes head, acquires, and at its own `:455` hands to
  `C->next == A`.
* A then hands to `A->next == B` (`A->next` was never touched by the rotation).
* B hands to `B->next == D`.

So the post-rotation grant order is **C, A, B, D** — A and B retain their
original relative order and are *ahead of D*, not moved to the tail. This is the
requested semantics and it falls out of the pointer sequence without extra work.
Note the contrast with CNA, which really does move the displaced waiter to a
tail (of the secondary queue) and therefore needs three extra patches
(v15 4/6..6/6) just for starvation control. Our worst case is that a skipped
waiter loses **exactly one position per handoff event**, which is a far weaker
starvation pressure — but not zero; see §9.6.

---

## 4. Why this does not recreate the double-head problem (Q3)

The previous investigation's killer argument was:

> Setting a non-head waiter's `locked` flag creates a second node in the head
> state, and both then execute `set_locked(lock)` at `qspinlock.c:447`.

That argument is correct **and it does not apply**, for a reason that is
arithmetic rather than subtle:

> **In the previous proposal, a promotion store was *added*. Here, a promotion
> store is *redirected*. The count of `X->locked = 1` stores per handoff is
> unchanged: exactly one.**

Side by side:

| | previous proposal (out of scope) | this proposal |
|---|---|---|
| actor | the **releaser** (`__pv_queued_spin_unlock_slowpath`), not a queue member | the **acquirer** at `qspinlock.c:455`, still a queue member (slot freed at `:468`) |
| state of the head when the actor acts | head already promoted, `H->locked == 1`, spinning at `:413` | nobody promoted; `A->locked == B->locked == C->locked == 0` |
| stores to `X->locked` | `H->locked = 1` (already happened) **+** `N->locked = 1` = **two** | `C->locked = 1` = **one** |
| resulting heads | 2 | 1 |
| lifetime guarantee over walked nodes | none | full (§2.1) |

Stated as the invariant the previous document named:

> *At most one node is in the head state at any instant, and a node enters the
> head state only via the one-shot handoff its unique predecessor performs at
> `qspinlock.c:455`.*

**This proposal satisfies both clauses.** Exactly one node (C) enters the head
state. It enters via the one-shot handoff at `:455`. And the clause "its unique
predecessor" is satisfied too, because the rotation *makes* H be C's unique
predecessor (store 3, `node->next = c`) **before** the handoff store — the queue
is well-formed at the moment of promotion, not merely afterwards. H then leaves;
A's predecessor is C, B's is A, D's is B, each unique.

### 4.1 Nobody can observe the intermediate states

Between store 1 and store 3 the queue is transiently malformed. The set of
agents that could in principle observe it:

1. **A queued node reading its own `next`.** Happens only at `:384` and `:453`,
   both of which are reached only *after* `arch_mcs_spin_lock_contended()` at
   `:376` returns, i.e. only after promotion. None of A/B/C/D is promoted during
   the rotation. **Cannot observe.**
2. **An arriving node writing `tail->next` at `:373`.** Writes only to the node
   that `xchg_tail()` named, i.e. the real tail. P3 guarantees we never write
   `c->next` if `c` could be the tail, and `b`/`node` are provably non-tail. The
   arrival's target node is untouched by us. **Disjoint.**
3. **`pv_wait_node()` / `pv_kick_node()` / `pv_wait_head_or_lock()` /
   `__pv_queued_spin_unlock_slowpath()`.** `grep -n -- '->next'
   kernel/locking/qspinlock_paravirt.h` returns **nothing**. The entire PV layer
   is `next`-blind. **Cannot observe.**
4. **A nested context on our own CPU (hardirq/softirq/NMI taking a different
   lock).** It uses `qnodes[idx]` for a *higher* `idx` on *this* CPU and joins a
   *different* lock's queue. A qnode is claimed by `idx = node->count++`
   (`:301`) and belongs to at most one queue at a time, so its node set is
   disjoint from {H, A, B, C, D}. If the IVH knob is enabled it may perform its
   own rotation — on its own queue, on disjoint nodes. **Disjoint.**
5. **`vcpu_is_preempted()` / the TSC heartbeat.** Read-only observers of
   per-CPU stamp data; they never touch qnodes. **N/A.**

The rotation is therefore atomic *by non-observability*, not by any barrier or
lock. That is precisely the property CNA and ShflLock rely on.

### 4.2 `lock->val` and the PV hash are untouched

* **`lock->val`'s tail field:** the rotation never changes the set of queued
  nodes and never changes the last node, so the encoded tail is unchanged. **No
  atomic on the lock word is required.** This is strictly simpler than CNA,
  which must `atomic_try_cmpxchg_release()` in `cna_splice_head()` precisely
  because its re-merge *does* change the tail.
* **Tail-clear at `:437-440`:** unreachable on the rotation path. That branch is
  taken only when `(val & _Q_TAIL_MASK) == tail`, i.e. we are the only queue
  member, in which case `goto release` at `:439` fires and `:453-455` never run.
  The two paths are mutually exclusive.
* **PV hash one-entry-per-lock (`qspinlock_paravirt.h:279`, `:302` `BUG()`s):**
  preserved. `pv_kick_node(lock, next)` at `:456` is called exactly once, on
  exactly one node (C instead of A). Its
  `try_cmpxchg_relaxed(&pn->state, &old /*VCPU_HALTED*/, VCPU_HASHED)` at `:908`
  fires at most once, so `pv_hash()` at `:919` inserts at most one entry. The
  head hashing itself at `:1021` is likewise done by exactly one node, because
  there is exactly one head. Since our policy *prefers* a running C, the
  `VCPU_HALTED` cmpxchg will usually fail and no hash entry is created at all —
  strictly less hash pressure than today.

### 4.3 What happens to A and B (the skipped waiters)

They are **untouched**. `A->locked` and `B->locked` remain 0; `A->next` remains
B. Each is still blocked at `:376` and each will be promoted in turn by its new
predecessor. Nothing is stranded, nothing is duplicated.

The one thing that does change for them is the value of their stack-local `prev`
pointer, captured at `:370`:

* A's `prev` names H; A's real predecessor is now C.
* D's `prev` names C; D's real predecessor is now B.
* B's `prev` names A, which is still correct.

`prev` is passed to `pv_wait_node(node, prev, lock)` and is used at exactly five
places in this tree, all reads, all heuristic:

```
668:	struct pv_node *pp = (struct pv_node *)prev;
714:			cause = pv_wait_early(pp, loop);
744:				switch (READ_ONCE(pp->head_ctl) & 0xffff) {
746:					bool tier1 = READ_ONCE(pp->state) != VCPU_RUNNING;
795:					if (READ_ONCE(pp->state) != VCPU_RUNNING) {
```

All of them feed one decision: *should I stop spinning early and halt?* A wrong
answer costs a wasted `pv_wait()`/kick round trip. It cannot affect who gets the
lock, cannot corrupt the queue, and cannot cause a lost wakeup — the wakeup
comes from A's *real* predecessor calling `pv_kick_node(lock, A)` at `:456`, and
from `__pv_queued_spin_unlock_slowpath()` via the hash, neither of which uses
`prev`.

Memory safety of a stale `prev` is also unconditional: `qnodes` is
`DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[_Q_MAX_NODES])` (`:138`), a static
array. A stale `prev` always points at valid, mapped memory; the worst case is
reading a `state`/`head_ctl` belonging to a later, unrelated acquisition — which
this tree **already tolerates today** (the `pv_init_node()` comment at `:638-654`
documents exactly this "slot still holds a value from a previous and entirely
unrelated spin" hazard and seeds the heartbeat to blunt it).

**This is the riskiest single property in the whole design**, not because it is
wrong today — it is demonstrably fine — but because it is an invariant living in
a `grep` result rather than in the code. See §9.7.

---

## 5. Traversal direction, distance, and the tail (Q5)

**Direction:** strictly forward (behind us). We may read and write the `next`
field of any node we reach, subject to the tail rule, because §2.1 guarantees
those nodes are queued and §4.1 guarantees nobody else is looking at them. We
may *not* touch anything ahead of us, and there is nothing ahead of us — the
head is anchored by no pointer (`lock->val` carries only the tail;
`encode_tail`/`decode_tail`, `qspinlock.h:52-69`). That asymmetry, which killed
the previous proposal, is irrelevant here because we never need to unlink
ourselves.

**How far:** the scan may advance while each hop's `next` is non-NULL, and must
stop at the first node whose `next` is NULL. That node is the *possible tail*
and is the last node we may **read** but must never **write**, and it may never
be chosen as the promotion candidate `c`.

**Why the tail rule is mandatory here, exactly as in CNA and ShflLock.** A
concurrent arrival E executes:

```
362:	old = xchg_tail(lock, tail);     /* E publishes itself as the new tail */
373:	WRITE_ONCE(prev->next, node);    /* E stores into the OLD tail's ->next */
```

If we wrote `c->next = a` while `c` was the old tail, E's `:373` store races
ours. Either order loses: if E wins, `a` (and everything behind it) is dropped
from the queue and A/B spin at `:376` forever; if we win, E is dropped and E
spins forever while `lock->val` still advertises E as the tail, so the eventual
`:453` `smp_cond_load_relaxed(&node->next, VAL)` on B never completes. Both are
unbounded hangs with preemption disabled. This is the failure mode CNA's
`if (nnext)` and ShflLock's line 80/91 exist to prevent, and it applies to us
identically.

**How to detect the tail.** Two mechanisms; use the first, optionally assert the
second.

1. **`READ_ONCE(c->next) != NULL` (primary, sufficient).** If `c` has a
   successor, some node already `xchg_tail()`-ed past `c`, so `lock->val`'s tail
   is at or beyond that successor. The tail can only advance while the queue is
   non-empty, and it can never return to `c`'s encoded value: doing so would
   require `c`'s (cpu, idx) slot to be re-enqueued, which requires `c` to have
   completed, which requires `c` to have been promoted, which cannot have
   happened (§2.1). The tail can also be *cleared* to 0 only by the tail-clear
   cmpxchg at `:438`, which is unreachable while our queue has members (§4.2).
   So `c->next != NULL` is a **permanent** certificate that `c` is not the tail
   — it cannot be invalidated between the check and the store.

2. **`(atomic_read(&lock->val) & _Q_TAIL_MASK) != encode_tail(pn->cpu, idx)`
   (debug assertion only).** This is ShflLock's line-80 form. It requires the
   candidate's `idx`, which `struct pv_node` does not currently store — see
   §9.3. It is strictly redundant given (1) and costs an atomic read of a
   contended cacheline on the lock-held path, so it belongs under
   `CONFIG_DEBUG_SPINLOCK` / a `WARN_ON_ONCE`, not in production.

**Bound on scan length.** The scan runs with the spinlock *held* and (usually)
preemption disabled, so it must be capped. `IVH_ROT_MAX_HOPS` of 4-8 is the
right order: it covers the realistic "one or two preempted vCPUs in front of a
live one" case, keeps the walk inside a couple of cachelines' worth of remote
node fetches, and bounds the worst-case added latency to the critical section.
CNA takes the same posture by re-entering `cna_order_queue()` once per spin
iteration rather than walking far. Hitting the cap simply means "no rotation,
hand off to A" — see Q4.

---

## 6. The liveness predicate (Q4)

Use the existing detector. `is_wait_preempted(cpu, tier2)`
(`qspinlock_paravirt.h:330`) already routes between `vcpu_is_preempted(cpu)` and
the IVH TSC heartbeat according to `ivh_pv_preempt_src`, and already has the
tier-1/tier-2 counter split that this project's measurements depend on. No new
detector is needed and none should be invented.

**`first_live` selection.** Walk `a, a->next, ...`; the candidate `c` is the
first node satisfying *both*:

* `!is_wait_preempted(((struct pv_node *)c)->cpu, /*tier2=*/...)`, and
* `READ_ONCE(c->next) != NULL` (P3, the tail rule).

If `c == a`, there is nothing to do: normal handoff. If no such `c` exists
within `IVH_ROT_MAX_HOPS`, **fall back to the normal handoff to A**. That is a
pure no-op fallback — we simply do not execute stores 1-3 — so there is no
pathological search and no deadlock path. The fallback is also the correct
answer when every remaining waiter looks preempted: promoting a preempted
successor is exactly what unmodified qspinlock does today, so the worst case is
status quo.

**The predicate is off the correctness path here, and that is the decisive
difference from the previous proposal.** The previous document's §3.4 argued
correctly that "vCPU preempted" is an unlatched observation that can go stale
between the test and the store, and that a wrong answer there put two CPUs in
one critical section. In this design a wrong answer means *we promoted a waiter
that had just been descheduled*, or *we failed to skip one that had just been
descheduled* — i.e. we made the same class of mistake unmodified qspinlock makes
100% of the time. The queue is still well-formed, exactly one node is promoted,
and the cost is throughput, not exclusion. The predicate has moved from the
correctness path to the performance path, which is where CNA's `numa_node` and
ShflLock's `S_PARKED` live.

Per the project's standing note on this host, do **not** use in-guest steal-time
readings as ground truth when evaluating whether the policy is firing correctly;
the `ivh_pv_preempt_src` selection and host-side data are the reference.

---

## 7. Where this belongs architecturally (Q7)

Three options were posed. The recommendation is **(b), a PV-specific handoff
helper**, implemented through the existing `pv_*` hook pattern so that the
generic/native path is *bit-identical* when the feature is compiled out or
disabled.

**(a) Generic MCS handoff in `qspinlock.c`.** This is what the CNA series did
(v15 2/6 refactors `arch_mcs_spin_unlock_contended` into
`mcs_lock_handoff()`/`arch_mcs_lock_handoff()` so CNA can override it). It is
the most upstream-shaped option and would be required if the mechanism ever
needed to work for native qspinlock. It is not required for us — the mechanism
is meaningless without a hypervisor — and it perturbs a file used by every
architecture. **Rejected for now**, but note that if this ever goes upstream it
should be re-expressed in that refactor's terms.

**(c) A CNA/ShflLock-style helper attached to the head, at `:410`.** Would reuse
`pv_wait_head_or_lock()` and burn otherwise-idle spin time. **Rejected**: the
liveness signal must be as fresh as possible, `:410` can precede the handoff by
the entire `ivh_pv_spin_threshold` (default `1UL << 15`,
`arch/x86/kernel/kvm.c:1130`), and the rotation would then have to be re-checked
at `:455` anyway. A pre-scan at `:410` is a possible *optimisation* later
(compute the candidate list cheaply while spinning, validate at `:455`), not the
initial design.

**(b) PV-specific hook — recommended.** `qspinlock.c` is compiled twice — the
`#define _GEN_PV_LOCK_SLOWPATH` at `:476` and the `#include "qspinlock.c"` at
`:490` — and the file already carries four no-op native stubs for exactly this purpose at `:149-164`
(`__pv_init_node`, `__pv_wait_node`, `__pv_kick_node`, `__pv_wait_head_or_lock`)
with the matching `#undef`s at `:481-484`. Adding a fifth follows a pattern that
is already there four times, and the native build gets an empty
`static __always_inline` function — zero generated code, zero behaviour change.

Total footprint in the generic file: **one stub + one `#define` + one `#undef` +
one call line.**

---

## 8. Reconciliation with CNA and ShflLock (Q8)

**What invariant requires each of them to preserve the successor?**

*Neither of them does preserve it* — that was the previous document's error
(§1). What each actually enforces is narrower:

| | CNA | ShflLock | this proposal |
|---|---|---|---|
| who may reorder | the primary-queue head, in `cna_wait_head_or_lock()` | the token-holding shuffler, originated by the queue head | the acquirer, at its own `:455` |
| how single-actor-ness is obtained | there is only one head | explicit `is_shuffler` token, invariants 2+3 | there is only one head, **and** the reorder and the handoff are the same instruction stream |
| may it change who gets the lock next | **yes** (`node->next = nnext`, then `cna_lock_handoff()` reloads `node->next`) | **yes** (`qlast.next = qcurr` with `qlast == qnode` initially) | yes |
| may it move the node that the current holder is about to notify | n/a (it *is* that node) | **no** — invariant 1 | n/a (it *is* the holder, and it is the notifier) |
| tail rule | `if (nnext)` | lines 80, 91-92 | P3, `c->next != NULL` |
| lifetime guarantee source | queue position | queue position | queue position (§2.1) |
| does it touch `lock->val` | yes, `atomic_try_cmpxchg_release()` in `cna_splice_head()` (it changes the tail) | yes, on tail reset | **no** |
| extra per-node state | 8 B (`numa_node`, `real_numa_node`, `encoded_tail`) + `mcs.locked` overloaded | `skt`, `batch`, `is_shuffler`, `status`, `task` | 1 B optional (`skips`) |

**Which of those invariants would our pre-handoff rotation violate?** **None.**

* CNA's and ShflLock's tail rule: honoured (P3).
* ShflLock invariant 1 (don't move the node the holder is about to notify):
  honoured trivially — we *are* the holder and we *are* the notifier, executing
  in program order on one CPU. There is no second actor to race.
* ShflLock invariants 2 and 3 (single shuffler, originated by the head):
  honoured by construction; we introduce no shuffler role at all.
* ShflLock invariant 4 (a shuffler may pass the role on): not used.
* CNA's "everything I touch is provably still queued": honoured (§2.1).
* qspinlock's one-head invariant: honoured (§4) — one promotion store, one head.
* PV's one-hash-entry-per-lock: honoured (§4.2).

**Which safety guarantees survive if the current head performs the rotation
before promoting?** All of them, and one is *strengthened*: because the reorder
and the notify are performed by the same CPU in straight-line code, we do not
need ShflLock's `is_shuffler` token, and we do not need invariant 1 as a
separate rule — it degenerates into "a single thread cannot race itself". That
is why this design needs less machinery than either published algorithm, not
more.

**Is this a new CNA-like policy with "currently runnable" replacing NUMA
locality?** Yes, with two structural simplifications:

1. **Single queue.** CNA needs a secondary queue because it wants to *deprioritise
   a whole class* of waiters (remote node) indefinitely. We want to *reorder
   locally* by one or two positions. A rotation inside the primary queue
   expresses that directly, which removes the `mcs.locked` overload, the
   `encoded_tail` field, `cna_try_clear_tail()`, `cna_splice_head()` and its CAS
   on the lock word.
2. **Later action point.** CNA acts at `:410` because `numa_node` is stable.
   Runnability is not, so we act at `:455`.

**SAFE TARGET vs. UNSUPPORTED TARGET, stated plainly:**

* **SAFE TARGET (this document).** H is the current head; it has just acquired
  the lock at `:447`; A, B, C, D are ordinary waiters with `locked == 0`; H
  rewrites `next` pointers behind itself and then performs its single handoff
  store to C. One head before, one head after. Feasible.
* **UNSUPPORTED TARGET (previous document, out of scope).** A has already
  received `A->locked = 1` and is spinning at `:413` as head, then A's vCPU is
  preempted, and some other party wants B or C to acquire instead. This requires
  either a second promotion store (two heads, mutual-exclusion failure at the
  plain `WRITE_ONCE` in `set_locked()`, `qspinlock.h:196-199`) or unlinking a
  node that nothing points to (impossible — `lock->val` encodes only the tail).
  The previous verdict stands for this and nothing here changes it.

The distinction is a single instruction: everything is feasible **before**
`smp_store_release(&next->locked, 1)` and nothing is feasible **after** it.

---

## 9. Implementation plan (Q9)

### 9.1 Files and exact edit sites

| file | site | edit |
|---|---|---|
| `kernel/locking/qspinlock.c` | `:149-164` (native stub block) | add `__pv_handoff_rotate()` no-op + `#define pv_handoff_rotate __pv_handoff_rotate` |
| `kernel/locking/qspinlock.c` | `:481-484` (`_GEN_PV_LOCK_SLOWPATH` block) | add `#undef pv_handoff_rotate` |
| `kernel/locking/qspinlock.c` | between `:453` and `:455` | one call: `pv_handoff_rotate(lock, node, &next);` |
| `kernel/locking/qspinlock_paravirt.h` | `:95-100` (`struct pv_node`) | add `u8 skips;` at offset 21 |
| `kernel/locking/qspinlock_paravirt.h` | `:629-656` (`pv_init_node`) | add `pn->skips = 0;` |
| `kernel/locking/qspinlock_paravirt.h` | new, before `pv_kick_node` (`:888`) | `pv_handoff_rotate()` |
| `arch/x86/include/asm/qspinlock.h` | near `:315-316` | `extern unsigned long ivh_handoff_rotate;` |
| `arch/x86/kernel/kvm.c` | near `:1130`, `:1411`, `:1865` | knob definition, per-CPU counters, sysctl entry |
| `arch/x86/include/asm/ivh_tsc_beat.h` | near `:259` | `DECLARE_PER_CPU` for the new counters |

### 9.2 Generic-path edit (native build unchanged)

```c
	/*
	 * contended path; wait for next if not observed yet, release.
	 */
	if (!next)
		next = smp_cond_load_relaxed(&node->next, (VAL));

	/*
	 * IVH: optionally rotate the first live waiter into the successor
	 * slot before promoting it. No-op in the native build and when the
	 * sysctl is 0. May rewrite @next; must run BEFORE the handoff store.
	 */
	pv_handoff_rotate(lock, node, &next);

	arch_mcs_spin_unlock_contended(&next->locked);
	pv_kick_node(lock, next);
```

Native stub (alongside the four that already exist at `:149-164`):

```c
static __always_inline void __pv_handoff_rotate(struct qspinlock *lock,
						struct mcs_spinlock *node,
						struct mcs_spinlock **nextp) { }
#define pv_handoff_rotate	__pv_handoff_rotate
```

### 9.3 Per-node state and the byte budget

`pahole -C pv_node kernel/locking/qspinlock.o` on this tree, run independently
for this report:

```
struct pv_node {
        struct mcs_spinlock        mcs;                  /*     0    16 */
        int                        cpu;                  /*    16     4 */
        u8                         state;                /*    20     1 */

        /* XXX 3 bytes hole, try to pack */

        u64                        head_ctl;             /*    24     8 */

        /* size: 32, cachelines: 1, members: 4 */
        /* sum members: 29, holes: 1, sum holes: 3 */
};
```

and `pahole -C qnode` gives `size: 32` (`mcs` 16 + `reserved[2]` 16), so the
`BUILD_BUG_ON(sizeof(struct pv_node) > sizeof(struct qnode))` at
`qspinlock_paravirt.h:633` has **zero** slack at the end and exactly **3 bytes**
free at offsets 21-23. The brief's figures are exact.

The design needs **one** of those bytes:

```c
struct pv_node {
	struct mcs_spinlock	mcs;
	int			cpu;
	u8			state;
	u8			skips;		/* offset 21; anti-starvation */
	u64			head_ctl;
};
```

`sizeof` stays 32, the hole shrinks to 2 bytes, `head_ctl` is untouched, and IVH
Idea 2 Stage 1's reserved `gen`/`yields` bits are unaffected. Everything else the
rotation needs is already present: the candidate's CPU number is `pn->cpu`, and
nothing else is required because **we never change the tail**, so CNA's
`encoded_tail` has no analogue here. If the optional debug tail assertion of §5
mechanism (2) is wanted, a `u8 idx` takes a second byte and
`encode_tail(pn->cpu, pn->idx)` (`__pure`, two shifts, `qspinlock.h:52-60`)
reconstructs the encoded tail; that is debug-only and can be dropped.

### 9.4 The PV helper

```c
#define IVH_ROT_MAX_HOPS	6	/* bounded work with the lock held */
#define IVH_ROT_MAX_SKIPS	4	/* per-node demotion cap           */

static void pv_handoff_rotate(struct qspinlock *lock,
			      struct mcs_spinlock *node,
			      struct mcs_spinlock **nextp)
{
	struct mcs_spinlock *a = *nextp, *b, *c, *d, *n;
	struct pv_node *pa = (struct pv_node *)a;
	int hops;

	if (likely(!READ_ONCE(ivh_handoff_rotate)))
		return;

	this_cpu_inc(ivh_rot_handoffs);

	/* Immediate successor looks runnable, or we've demoted it enough. */
	if (!is_wait_preempted(pa->cpu, true))
		return;
	if (pa->skips >= IVH_ROT_MAX_SKIPS) {
		this_cpu_inc(ivh_rot_giveup_starve);
		return;
	}
	this_cpu_inc(ivh_rot_eligible);

	for (b = a, hops = 0; hops < IVH_ROT_MAX_HOPS; hops++, b = c) {
		c = READ_ONCE(b->next);
		if (!c)					/* b may be the tail */
			break;
		d = READ_ONCE(c->next);
		if (!d) {				/* P3: c may be the tail */
			this_cpu_inc(ivh_rot_giveup_tail);
			break;
		}
		if (is_wait_preempted(((struct pv_node *)c)->cpu, true))
			continue;

		/* --- candidate found: c is live, non-tail, unpromoted --- */
		IVH_ROT_ASSERTS(node, a, b, c, d);

		/* bump the demotion counters of everyone we pass over */
		for (n = a; n != c; n = READ_ONCE(n->next)) {
			struct pv_node *pn = (struct pv_node *)n;

			if (pn->skips < U8_MAX)
				pn->skips++;
		}

		WRITE_ONCE(b->next, d);		/* 1: unlink c            */
		WRITE_ONCE(c->next, a);		/* 2: c heads the skipped */
		WRITE_ONCE(node->next, c);	/* 3: splice c behind us  */
		*nextp = c;			/* 4: hand off to c       */

		this_cpu_add(ivh_rot_hops_sum, hops + 1);
		this_cpu_inc(ivh_rot_done);
		return;
	}
	this_cpu_inc(ivh_rot_giveup_nolive);
}
```

Every store in the loop targets a node strictly behind `node`, which §2.1 proves
is still queued and §4.1 proves nobody else is reading. Every `next` read is
`READ_ONCE`, preserving the address-dependency chain off the relaxed load at
`:453` — the same dependency the existing `next->locked` store at `:455` already
relies on. No `smp_mb()`, no CAS, no atomic on `lock->val`.

### 9.5 Races prevented, and by what

| race | prevented by |
|---|---|
| concurrent enqueue writing `c->next` at `:373` | P3: `c->next != NULL` is a permanent non-tail certificate (§5) |
| concurrent enqueue writing `b->next` | `b->next == c != NULL`, same certificate |
| a walked node completing and recycling its slot | §2.1: no node behind us is promoted, promotion is the only exit |
| a walked node reading its own `next` mid-rotation | `:384`/`:453` are post-promotion only (§4.1 item 1) |
| second head / double `set_locked()` | exactly one `locked` store, on exactly one node (§4) |
| tail field of `lock->val` desynchronised | never written; node set and last node unchanged (§4.2) |
| two PV hash entries for one lock | one `pv_kick_node()` on one node (§4.2) |
| nested context on our own CPU rotating the same nodes | qnode belongs to one queue at a time via `idx = node->count++` (§4.1 item 4) |
| promotion visible before the pointer stores | `smp_store_release()` at `:455` orders them; `smp_cond_load_acquire()` at `:376` pairs |
| stale `prev` in a skipped waiter | heuristic-only; zero `->next` uses in the PV layer, all `pp` uses are reads (§4.3) |
| unbounded work with the lock held | `IVH_ROT_MAX_HOPS` |
| starvation of a repeatedly-skipped waiter | `pv_node.skips` + `IVH_ROT_MAX_SKIPS` (§9.6) |
| queue changes mid-scan | it cannot: `next` is monotonic and we are the only mutator (§2.2). The scan needs no revalidation and no retry loop. |

That last row is worth emphasising, because it is what makes the whole thing
tractable: **there is no "the queue changed under me" case to detect or fall back
from.** The only mutation any other CPU can make to the prefix we walk is
`WRITE_ONCE(tail->next, new)` on the real tail, and P3 keeps us off the tail.

### 9.6 Starvation

A skipped waiter loses exactly one queue position per rotation, and stays ahead
of every node behind the promoted candidate. That is much gentler than CNA,
which moves the displaced waiter to the tail of a secondary queue and needed
three extra patches for starvation control. But it is not free: under sustained
overcommit, a chronically-preempted vCPU could be demoted at every handoff.
`pv_node.skips` bounds this at `IVH_ROT_MAX_SKIPS` demotions, after which the
node is promoted normally regardless of its apparent state. The counter is reset
in `pv_init_node()` on every queue entry, so it is per-acquisition, not
per-CPU-lifetime.

Note the interaction flagged in the previous work's risk list, which still
applies: `pv_hybrid_queued_unfair_trylock()` (`qspinlock_paravirt.h:127`) lets
arrivals bypass the queue entirely, and the effective queue dynamics it sees will
change. Any measurement must hold the steal window fixed or the two effects will
confound — the same confound already documented for the adaptive-lock work.

### 9.7 Correctness invariants to encode as assertions

Under `CONFIG_DEBUG_SPINLOCK` (or a `ivh_handoff_rotate == 2` debug level),
`IVH_ROT_ASSERTS(node, a, b, c, d)` should check:

```c
	/* I1: nobody behind us has been promoted -- the one-head invariant */
	WARN_ON_ONCE(READ_ONCE(a->locked) != 0);
	WARN_ON_ONCE(READ_ONCE(b->locked) != 0);
	WARN_ON_ONCE(READ_ONCE(c->locked) != 0);

	/* I2: the tail rule */
	WARN_ON_ONCE(READ_ONCE(c->next) == NULL);
	WARN_ON_ONCE(READ_ONCE(b->next) != c);

	/* I3: we really are the predecessor we think we are */
	WARN_ON_ONCE(READ_ONCE(node->next) != a);

	/* I4: no self-splice, no cycle of length 1 or 2 */
	WARN_ON_ONCE(c == node || c == a || b == c || d == c);

	/* I5 (optional, needs pv_node.idx): c is not the encoded tail */
	WARN_ON_ONCE((atomic_read(&lock->val) & _Q_TAIL_MASK) ==
		     encode_tail(((struct pv_node *)c)->cpu,
				 ((struct pv_node *)c)->idx));
```

and immediately after the three stores:

```c
	/* I6: the queue is well-formed at the moment of promotion */
	WARN_ON_ONCE(READ_ONCE(node->next) != c);
	WARN_ON_ONCE(READ_ONCE(c->next) != a);
	WARN_ON_ONCE(READ_ONCE(b->next) != d);
```

A seventh invariant deserves a comment rather than an assertion, because it is
the one most likely to be silently invalidated by a future change:

> **I7: no code outside `qspinlock.c` may read or write `mcs_spinlock.next`, and
> `pv_wait_node()`'s `prev` argument is a hint only.** Verified 2026-09-13:
> `grep -n -- '->next' kernel/locking/qspinlock_paravirt.h` returns nothing, and
> all five `pp` uses (`:668`, `:714`, `:744`, `:746`, `:795`) are reads of
> `state`/`head_ctl` feeding halt heuristics. If a future PV change makes a
> waiter act on `prev` in a way that affects correctness, or makes it write
> through `prev`, the rotation must be re-audited.

### 9.8 Staging and instrumentation

This matches the Step-0/Step-1 shape the project already uses for the
lock-skipping work (commit `f421035ff`).

**Step 0 — detect-only, no stores.** Ship `pv_handoff_rotate()` with the three
`WRITE_ONCE`s and the `*nextp` assignment compiled out (`ivh_handoff_rotate == 1`
= scan and count; `== 2` = scan, count and rotate). Counters, following the
`ivh_head_spin_enter` pattern (`DEFINE_PER_CPU` at `arch/x86/kernel/kvm.c:1411`,
`DECLARE_PER_CPU` at `arch/x86/include/asm/ivh_tsc_beat.h:259`):

| counter | question it answers |
|---|---|
| `ivh_rot_handoffs` | denominator: contended handoffs reaching `:455` |
| `ivh_rot_eligible` | how often the immediate successor looks preempted |
| `ivh_rot_done` | how often a live, non-tail candidate exists |
| `ivh_rot_hops_sum` | mean rotation distance (`/ ivh_rot_done`) |
| `ivh_rot_giveup_tail` | how often the tail rule blocks a rotation |
| `ivh_rot_giveup_nolive` | how often everything in range looks preempted |
| `ivh_rot_giveup_starve` | how often the skip cap blocks a rotation |

The go/no-go for Step 1 is `ivh_rot_done / ivh_rot_handoffs`. If that ratio is
small on the real NHextend3 workload, the mechanism cannot pay for the scan and
the work should stop there — the same conclusion the previous document reached
about its §5 variant, but this time measured rather than argued.

**Step 1 — enable stores** behind `ivh_handoff_rotate == 2`, default 0, with
`CONFIG_DEBUG_SPINLOCK` assertions on. Validation before any benchmark:
`locktorture` with `torture_type=spin_lock` and `spin_lock_irq` under an
overcommitted guest, plus a deliberately-preempting host load, watching for any
`WARN_ON_ONCE` and for `smp_cond_load_relaxed(&node->next)` hangs (RCU stall /
`hung_task` reports are the signature of an orphaned sub-queue).

**Step 2 — measure** with the steal window held fixed, per §9.6.

Per this project's standing rule, kernel changes are staged with `grub-reboot`
and the reboot is left to the user.

---

## 10. Answer to the success criterion

> Can the existing MCS handoff be changed from "wake `next`" to "find the first
> live waiter, rotate that waiter into `next` position, then wake it," while
> preserving qspinlock's one-head invariant and qnode lifetime guarantees?

**Yes.**

* **One-head invariant:** preserved. The number of `X->locked = 1` stores per
  handoff is unchanged at one; only the target changes, and the queue is
  well-formed at the instant of the store (§3.1 store 3, §4).
* **qnode lifetime:** preserved. At `qspinlock.c:455` the actor is the acquirer,
  not the releaser: it still holds its own slot (freed at `:468`) and it has
  promoted nobody, so by §2.1 every node it walks is provably still queued. This
  is CNA's structural guarantee, unweakened.
* **Tail safety:** preserved by the `c->next != NULL` rule, which is CNA's
  `if (nnext)` and ShflLock's lines 80/91-92.
* **Cost:** one `u8` in an existing 3-byte padding hole, one no-op hook in the
  generic path, and no new atomics or barriers anywhere.

## Sources

- [PATCH v15 3/6 locking/qspinlock: Introduce CNA into the slow path of qspinlock (qspinlock_cna.h)](https://www.spinics.net/lists/arm-kernel/msg894733.html)
- [Re: [PATCH v15 0/6] Add NUMA-awareness to qspinlock — Alex Kogan, lore.kernel.org](https://lore.kernel.org/linux-arm-kernel/50F3F13D-705C-4213-89C3-043B0DA2C5AF@oracle.com/)
- [PATCH v12 3/5 locking/qspinlock: Introduce CNA into the slow path of qspinlock](https://lists.infradead.org/pipermail/linux-arm-kernel/2020-November/619062.html)
- [Scalable and Practical Locking with Shuffling (ShflLock), Kashyap et al., SOSP 2019 — PDF](https://gts3.org/assets/papers/2019/kashyap:shfllock.pdf)
- [sslab-gatech/shfllock — public source](https://github.com/sslab-gatech/shfllock)

Kernel sources read (`/root/kernels/linux-6.17-vanilla`): `kernel/locking/qspinlock.c`,
`kernel/locking/qspinlock.h`, `kernel/locking/qspinlock_paravirt.h`,
`kernel/locking/mcs_spinlock.h`, `include/asm-generic/mcs_spinlock.h`,
`arch/x86/include/asm/qspinlock.h`, `arch/x86/include/asm/ivh_tsc_beat.h`,
`arch/x86/kernel/kvm.c`. Struct layout re-verified with
`pahole -C pv_node|qnode|mcs_spinlock kernel/locking/qspinlock.o`.
