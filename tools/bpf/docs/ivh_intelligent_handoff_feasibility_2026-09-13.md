# Intelligent handoff to a live non-head waiter: feasibility against CNA and ShflLock

**Date:** 2026-09-13
**Question:** when a PV qspinlock is released and the MCS queue head's vCPU is
preempted by the hypervisor, can the lock be handed to the first *alive* waiter
further down the queue instead of to the head?
**Tree read:** `/root/kernels/linux-6.17-vanilla` (this project's 6.17 base, with
the IVH G-LOCK instrumentation already in place).
**Explicitly out of scope:** lock stealing via
`pv_hybrid_queued_unfair_trylock()` (`kernel/locking/qspinlock_paravirt.h:127`).
That is a not-yet-queued arrival racing for the lock word, it already exists,
and it is not an answer to this question. It is not proposed below.

---

## VERDICT (read this first)

**(B) Not achievable as proposed.**

The single invariant that cannot be maintained:

> **Exactly one node is in the *head state* at any instant, and a node enters the
> head state only by its unique predecessor performing the one-shot
> `prev -> next->locked = 1` handoff.**

That invariant is what entitles the head to run `set_locked(lock)` at
`kernel/locking/qspinlock.c:447`, which is a **plain, non-atomic store**
(`WRITE_ONCE(lock->locked, _Q_LOCKED_VAL)`, `kernel/locking/qspinlock.h:196-199`).
There is no CAS, no token and no arbitration there: mutual exclusion for the
*queue* half of qspinlock rests entirely on there being one head. Setting a
non-head waiter's `locked` flag creates a second node in the head state, and
both then execute line 447. **That is a mutual-exclusion failure, not a fairness
wobble.**

Neither CNA nor ShflLock ever hits this, because **neither design ever moves,
demotes or bypasses the head.** Both reorder strictly *behind* the head, and
both do the reordering *from* the head (or from a waiter ahead of what it
touches). This proposal's whole purpose is to demote a head — an operation
that appears in neither protocol, because neither structure admits it: in
qspinlock the head is not pointed to by anything. `lock->val` carries only the
**tail** (`encode_tail`/`decode_tail`, `kernel/locking/qspinlock.h:52-69`), the
list is singly linked head->tail, and the head's predecessor has already left.
You cannot unlink a node that nothing links to; you can only ignore it, and
ignoring it is exactly the double-head break.

A second, independent killer stands behind the first (section 3.3): the
proposed splicer is **the releaser**, which is not a queue member at all and
therefore has *zero* lifetime guarantee on the nodes it would walk. CNA's and
ShflLock's splicers derive their memory safety purely from queue position.

A genuinely buildable, *scope-reduced* variant does exist and is written up in
section 5 — it prevents a preempted waiter from *becoming* head, rather than
bypassing one that already is. It is the CNA protocol verbatim with a liveness
policy swapped in for the NUMA policy. It is **not** the thing that was asked
for, and it is labelled as such.

---

## 1. Phase 1 — what the two papers actually do

### 1.1 CNA (Dice & Kogan, "Compact NUMA-aware Locks", EuroSys 2019; LKML v2..v15, 2019-2021, never merged)

The LKML series is the useful artifact: it is CNA grafted onto *this exact*
qspinlock. Structure, from the header comment of `kernel/locking/qspinlock_cna.h`
(v15 3/6):

```
 * In CNA, spinning threads are organized in two queues, a primary queue for
 * threads running on the same NUMA node as the current lock holder, and a
 * secondary queue for threads running on other nodes.
 *
 * N.B. locked := 1 if secondary queue is absent. Otherwise, it contains the
 * encoded pointer to the tail of the secondary queue, which is organized as a
 * circular list.
```

**Who reorders, and what exclusion they hold.** The **MCS queue head** — the node
that has already been handed `locked` and is now spinning on the lock word. CNA
hooks the head's spin loop:

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
```

The exclusion is **queue position, nothing else**. There is no lock on the
queue. Being the head means: every node it will touch is strictly behind it,
therefore none of them can have acquired the lock, therefore none of them can
have returned from the slow path. That is the entire safety argument, and it is
a *structural* one.

**Which nodes it may touch.** Exactly three, and no more:

```c
static int cna_order_queue(struct mcs_spinlock *node)
{
        struct mcs_spinlock *next = READ_ONCE(node->next);
        ...
        if (!next)
                return 0;
        ...
        if (next_numa_node != numa_node) {
                struct mcs_spinlock *nnext = READ_ONCE(next->next);

                if (nnext)
                        cna_splice_next(node, next, nnext);

                return 0;
        }
        return 1;
}
```

`node` (its own), `next`, `nnext`. The `if (nnext)` guard is the **tail rule**:
if `next->next == NULL`, `next` may be the queue tail, and a concurrent arrival
is about to do `xchg_tail()` followed by `WRITE_ONCE(prev->next, node)` on it.
Splicing a possible tail would race that store. CNA therefore never touches the
tail. (ShflLock has the identical rule; see 1.2.)

**Exact pointer sequence.**

```c
static void cna_splice_next(struct mcs_spinlock *node,
                            struct mcs_spinlock *next,
                            struct mcs_spinlock *nnext)
{
        /* remove 'next' from the main queue */
        node->next = nnext;

        /* stick `next` on the secondary queue tail */
        if (node->locked <= 1) { /* if secondary queue is empty */
                /* create secondary queue */
                next->next = next;
        } else {
                /* add to the tail of the secondary queue */
                struct mcs_spinlock *tail_2nd = decode_tail(node->locked);
                struct mcs_spinlock *head_2nd = tail_2nd->next;

                tail_2nd->next = next;
                next->next = head_2nd;
        }

        node->locked = ((struct cna_node *)next)->encoded_tail;
}
```

Each step is safe because: the splicer owns `node`; `next` and `nnext` are
provably still queued behind the splicer (position argument above); and the
secondary queue is reachable *only* through `node->locked`, which only the
splicer writes. The secondary queue is re-merged later by `cna_splice_head()`,
whose empty-primary case is the only place an atomic is needed — it must
CAS the lock word because it is changing the *tail*:

```c
                tail_2nd->next = NULL;
                ...
                new = ((struct cna_node *)tail_2nd)->encoded_tail | _Q_LOCKED_VAL;
                if (!atomic_try_cmpxchg_release(&lock->val, &val, new)) {
                        /* Restore the secondary queue's circular link. */
                        tail_2nd->next = head_2nd;
                        return NULL;
                }
```

**Per-node state added.**

```c
struct cna_node {
        struct mcs_spinlock     mcs;
        u16                     numa_node;
        u16                     real_numa_node;
        u32                     encoded_tail;   /* self */
};
```

8 bytes on top of `struct mcs_spinlock`, **plus** the overload of the existing
`mcs.locked` field to carry the secondary-queue encoded tail (`locked <= 1`
means "no secondary queue").

**Does CNA's safety depend on the reordered waiters RUNNING?** **No — and this
must be conceded honestly.** A spliced node is pure passive data: it sits
spinning on its own `locked` and takes no action until `locked` becomes
non-zero. Whether its vCPU is on-CPU or descheduled is irrelevant to the
splice. *But* — and this is the load-bearing part — **CNA never skips anyone.**
Every node remains in exactly one of the two queues, and the lock is still only
ever handed to the head of the primary queue. CNA *defers*; it does not
*bypass*. Liveness of the moved node is irrelevant precisely because the
protocol never asks the moved node to do anything.

**CNA is native-only.** The series' own installer refuses to run under a PV
slowpath:

```c
void __init cna_configure_spin_lock_slowpath(void)
{
        if (numa_spinlock_flag < 0)
                return;

        if (numa_spinlock_flag == 0 && (nr_node_ids < 2 ||
                    pv_ops.lock.queued_spin_lock_slowpath !=
                        native_queued_spin_lock_slowpath))
                return;
        ...
}
```

`CONFIG_NUMA_AWARE_SPINLOCKS` `depends on PARAVIRT_SPINLOCKS` only to borrow the
`pv_ops` *patching* machinery; the guard above then declines to install CNA if
anything other than the **native** slowpath is present. CNA has never been run
in a paravirt guest, and lock-holder preemption is outside its problem domain
entirely.

### 1.2 ShflLock (Kashyap, Calciu, Cheng, Min, Kim — SOSP 2019)

**Who reorders.** A **waiter** — the *shuffler*. Four invariants, quoted from
§4.2.1:

> 1) The successor of the lock holder, if it exists, always keeps its position
> intact in the queue. 2) Only one waiter can be an active shuffler, as
> shuffling is single threaded. 3) Only the head of the queue can start the
> shuffling process. 4) A shuffler may pass the shuffling role to one of its
> successors.

**What exclusion the shuffler holds.** A single logical token, `is_shuffler`,
handed forward explicitly (`qlast.is_shuffler = True`, line 108 of Fig. 4), plus
the rule that only the queue head may *originate* shuffling. So again: the
shuffler is a queue member positioned strictly ahead of everything it touches,
and again that position is the whole memory-safety argument.

**Which nodes, and the tail rule.**

```
76    while True: # Walking the linked list in sequence
77      qcurr = qprev.next
78      if qcurr is None:
79        break
80      if qcurr == lock.tail: # Do not shuffle if at the end
81        break
```

Same tail prohibition as CNA, for the same reason ("there might be waiters
joining at the end of the tail, which it cannot move"), and it also bails when
`qnext is None` (lines 91-92).

**Exact pointer sequence** (Fig. 4, lines 89-98):

```
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
```

**Per-node state added.** The qnode carries `status`
(`S_WAITING` / `S_READY`, plus `S_PARKED` / `S_SPINNING` in the blocking
version), `batch`, `is_shuffler`, `next`, `skt` (socket id), and — in the
blocking version — a `task` pointer for `wake_up_task()`. The paper gives the
lock as 12 bytes and the qnode as "4B+4B+8B, per-thread, on stack" for the
non-blocking variant, growing with the blocking fields.

**Does ShflLock's safety depend on waiters RUNNING?** This is the design that
*does* confront non-running waiters — and the way it confronts them is decisive
for our question. It **wakes them; it never skips them**:

```
 6       # Notify the very next waiter
 7   -   qnext.status = S_READY
 8   +   # Atomically SWAP the qnode status
 9   +   prev_status = SWAP(&qnext.status, S_READY)
10   +   if node_pstate == S_PARKED: # Required for avoiding lost wakeup
11   +     wake_up_task(qnext.task) # Explicitly wake up the very next waiter
```

and the shuffler proactively wakes sleepers it passes (Fig. 5: "t2 is sleeping,
but t1 wakes it up"; t4 is moved *and* woken "to mitigate the wakeup latency").

Two properties of that make it inapplicable here:

1. **Parking is voluntary, self-inflicted and published.** `park_waiter(qcurr)`
   is called by the waiter *on itself*, after `task_timed_out(qcurr.task)`. The
   waiter chooses to park, and publishes `S_PARKED` before doing so. Hypervisor
   preemption is none of those things.
2. **The status transition is an atomic SWAP**, precisely so the parked/ready
   race is *latched*. `S_PARKED` is a stable, settled fact that the releaser
   observes atomically. "This vCPU is preempted" is not a latchable fact — see
   section 3.4.

**Net Phase-1 finding.** The asymmetry the task asked me to adjudicate is real
but it is *not* where I expected it. CNA does **not** depend on moved waiters
being alive (concede that). ShflLock *does* handle dead waiters, but by waking
them, under a cooperative, atomically-latched parked protocol. What **both**
share, and what this proposal breaks, is something more basic:

> Every node stays in the queue, and the lock is only ever handed to the queue
> head. Reordering happens strictly behind the head, performed by a party that
> is itself in the queue ahead of everything it touches.

Skipping the head violates all three clauses at once.

---

## 2. Phase 2 — mapping both mechanisms onto this kernel

All citations are `/root/kernels/linux-6.17-vanilla`.

### 2.1 The flow, in this tree

| step | site |
|---|---|
| claim per-CPU node slot, bump nesting | `qspinlock.c:300-301` (`idx = node->count++`) |
| bail to pure spinning if >4 nested | `qspinlock.c:315-320` |
| init node | `qspinlock.c:336-338` (`locked = 0; next = NULL; pv_init_node()`) |
| publish tail | `qspinlock.c:362` (`old = xchg_tail(lock, tail)`) |
| link behind predecessor | `qspinlock.c:373` (`WRITE_ONCE(prev->next, node)`) |
| **wait to become head** | `qspinlock.c:375-376` (`pv_wait_node()`, then `arch_mcs_spin_lock_contended(&node->locked)`) |
| **head spin** | `qspinlock.c:410` (`pv_wait_head_or_lock()`) / `:413` (`atomic_cond_read_acquire`) |
| tail-clear + uncontended claim | `qspinlock.c:437-440` |
| **contended claim (plain store)** | `qspinlock.c:447` (`set_locked(lock)`) |
| wait for successor to appear | `qspinlock.c:453` (`smp_cond_load_relaxed(&node->next, (VAL))`) |
| **hand off** | `qspinlock.c:455-456` (`arch_mcs_spin_unlock_contended(&next->locked)`; `pv_kick_node()`) |
| **release node slot** | `qspinlock.c:468` (`__this_cpu_dec(qnodes[0].mcs.count)`) |

`arch_mcs_spin_unlock_contended(l)` is `smp_store_release((l), 1)`
(`kernel/locking/mcs_spinlock.h:30-37`).

### 2.2 Where CNA's splice point would live here

Directly answerable: **CNA replaces `qspinlock.c:410` and `:413`.** Its
`cna_wait_head_or_lock()` occupies the same slot as `pv_wait_head_or_lock()`,
returning 0 so the caller still falls through to the real wait — the comment
`/* we lied; we didn't wait, go do so now */` says exactly that. The splicing
runs inside the head's otherwise-idle spin on `_Q_LOCKED_PENDING_MASK`.

CNA additionally replaces:

* `qspinlock.c:437-440`, the tail-clear, with `cna_try_clear_tail()` — because
  an "empty primary queue" is no longer an empty queue.
* `qspinlock.c:455`, the handoff, with `cna_lock_handoff()` — because the value
  written into `next->locked` must carry the secondary-queue tail forward
  rather than being a literal `1`.

**Does this kernel provide the exclusion CNA relies on at that point?** **Yes.**
At `qspinlock.c:410` the caller is by construction the unique queue head: it got
there through `arch_mcs_spin_lock_contended(&node->locked)` at `:376`, which
only its unique predecessor's `:455` can satisfy. Everything from `node->next`
onward is strictly behind it and cannot have completed `:468`. CNA's structural
exclusion exists here unchanged. (What does *not* exist is CNA itself — see the
`cna_configure_spin_lock_slowpath()` guard in 1.1: CNA declines to install when
a PV slowpath is present, which in this guest it is.)

### 2.3 Where ShflLock's shuffler role would live here

ShflLock's shuffler is a *waiter*, so its natural home is `pv_wait_node()`
(`qspinlock_paravirt.h:664-879`) — specifically inside the bounded spin loop at
`:683-825`, which already re-reads the predecessor's `pp->state` and
`pp->head_ctl` on a `PV_PREV_CHECK_MASK` (`:47`, `0xff`) cadence. That loop is
structurally the same place ShflLock's `spin_until_very_next_waiter()` calls
`shuffle_waiters(lock, qcurr, False)` (Fig. 4 line 50-51). Origination by the
queue head (ShflLock invariant 3) would sit in `pv_wait_head_or_lock()`
(`qspinlock_paravirt.h:940`), i.e. the same slot CNA uses.

**Does the exclusion exist here?** Partly. The position argument holds for the
head-originated case. It does **not** hold for a mid-queue shuffler unless the
`is_shuffler` token is added as real per-node state — this tree has no such
token, and `head_ctl` (`qspinlock_paravirt.h:99`) is a *state publication*
field, not a mutual-exclusion token: every write to it is a plain `WRITE_ONCE`
(`:637`, `:984`, `:1069`, `:1075`, `:1119`), never a CAS. So ShflLock's
single-shuffler invariant would have to be built from scratch.

### 2.4 Byte budget — verified, not assumed

`struct pv_node` (`qspinlock_paravirt.h:95-100`):

```c
struct pv_node {
        struct mcs_spinlock     mcs;
        int                     cpu;
        u8                      state;
        u64                     head_ctl;
};
```

`BUILD_BUG_ON(sizeof(struct pv_node) > sizeof(struct qnode));` at
`qspinlock_paravirt.h:633`, inside `pv_init_node()`. Verified independently with
`pahole` against the built `kernel/locking/qspinlock.o`:

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

and `sizeof(struct qnode)` = 32 (`qspinlock.h:40-45`: `mcs` 16 + `reserved[2]`
16). **So: 32 == 32, zero slack at the end.** The claim in the brief is exact.

Free space actually available inside the ceiling:

| source | bytes | notes |
|---|---|---|
| the padding hole at offset 21-23 | **3** | genuinely free today |
| unused `head_ctl` bits | **6** | `HC(gen, y, st)` packs `{gen:32 \| yields:16 \| state:16}` (`:61`, `:93`) and the comment at `:60-65` says only `state` is used; `gen` and `yields` are Stage-1 forward-compat placeholders |
| **total** | **9** | and 6 of those are already spoken for by Idea 2 Stage 1 |

**Would CNA's fields fit?** `struct cna_node` adds `numa_node` (u16) +
`real_numa_node` (u16) + `encoded_tail` (u32) = **8 bytes**. Against a 3-byte
real hole: **no.** It fits only by also consuming `head_ctl`'s reserved
`gen`/`yields` fields — which are exactly the fields IVH Idea 2 Stage 1 is
holding. And CNA *additionally* needs `mcs.locked` overloaded to carry an
encoded secondary tail, which collides conceptually with PV's use of
`node->locked` as the halt/wake predicate (`qspinlock_paravirt.h:694`, `:847`,
`:871`) — survivable, since those are truthiness tests and any encoded tail is
> 1, but it is a real coupling that would need auditing.

**Would ShflLock's fields fit?** Worse. `skt` + `batch` + `is_shuffler` +
`status` + a `task` pointer for the blocking variant is comfortably past 8
bytes before the task pointer is even counted. No.

**What would have to give.** One of: (a) drop `head_ctl` to `u32` and give up
Idea 2 Stage 1's `gen`/`yields`, freeing 7 bytes total; (b) raise
`struct qnode` to 48 bytes, which halves per-cacheline node density — the
comment at `qspinlock.h:30-39` is explicit that the current 32 is a deliberate
trade ("only two of them can fit in a cacheline ... we don't want to penalize
pvqspinlocks"); (c) derive rather than store — e.g. `encode_tail()` is `__pure`
(`qspinlock.h:52`) and needs only `(cpu, idx)`; `cpu` is already in `pv_node`,
so a `u8 idx` in the 3-byte hole reconstructs `encoded_tail` for free. Option
(c) is the only one that costs nothing, and it is the one section 5 uses.

---

## 3. Phase 3 — the decisive question

**Proposal:** at release time, if the MCS queue head's vCPU is preempted, set
some later, alive waiter's `locked` instead.

### 3.1 Does CNA's or ShflLock's protocol transfer? No — the geometry does not exist

Both protocols are *"the head reorders what is behind it."* The node this
proposal needs to move **is the head**. Concretely, in this tree:

* The head cannot splice itself. `cna_order_queue()` operates on
  `node->next` and `node->next->next`; there is no `self` case, and there could
  not be, because a node's own removal would require rewriting a pointer *to*
  it.
* **Nothing points to the head.** After the predecessor executes
  `qspinlock.c:455` it proceeds to `:468` and leaves. The head is anchored by no
  pointer. `lock->val` holds only the tail (`encode_tail`, `qspinlock.h:52-60`;
  `decode_tail`, `:62-69`). The head is discoverable *only* via the PV hash, and
  only if it hashed itself (`qspinlock_paravirt.h:1021`) or was hashed by its
  predecessor (`:919`).
* Therefore the head cannot be *unlinked*. It can only be *ignored*. Ignoring it
  means a second node enters the head state, which is the invariant break.

ShflLock's invariant 1 — *"The successor of the lock holder, if it exists,
always keeps its position intact in the queue"* — is the same prohibition stated
from the other side. ShflLock will not move even the head's *successor*, let
alone the head.

### 3.2 Does a releaser setting a non-head's `locked` strand the skipped waiters?

**Yes — and worse than stranding: it breaks mutual exclusion first, and can
hard-hang second.** Trace, with queue `H -> A -> B -> N` (H = preempted head):

**(a) Mutual exclusion breaks.** Suppose the releaser sets `N->locked = 1`.
N returns from `arch_mcs_spin_lock_contended()` at `qspinlock.c:376`, reads
`node->next` at `:384`, and arrives at the head spin, `:410`/`:413`. H is
*already* at `:413` (or blocked in `pv_wait()` at `qspinlock_paravirt.h:1070`).
Both now wait for `!(VAL & _Q_LOCKED_PENDING_MASK)`. When the lock word clears,
**both pass**. At `:437`, `(val & _Q_TAIL_MASK) == tail` can be true for at most
one of them (the real tail). The other falls through to `:447`:

```c
	set_locked(lock);
```

which is `WRITE_ONCE(lock->locked, _Q_LOCKED_VAL)` — an unconditional store
(`qspinlock.h:196-199`). Both nodes proceed into the critical section. There is
no arbitration at this point in the code *by design*, because the design
guarantees only one caller can be here.

**(b) Then the queue is destroyed.** Two sub-cases:

* If N happened to be the tail, N takes the `:437-440` branch and executes
  `atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL)`, which **zeroes
  the tail field**. The lock word now advertises an empty queue while H, A and B
  are still queued and spinning. A subsequently arriving CPU's `xchg_tail()` at
  `:362` returns `old & _Q_TAIL_MASK == 0`, so it *skips* `WRITE_ONCE(prev->next,
  node)` at `:373` and becomes head immediately — never linking behind B. B,
  when it eventually reaches `:453`, executes
  `next = smp_cond_load_relaxed(&node->next, (VAL))` waiting for a successor
  **that will never arrive**. That loop is unbounded and has no timeout, no
  preemption point and no escape. **Hard hang**, frequently with interrupts
  disabled.
* If N was mid-queue, N hands off *downstream* at `:455` to `N->next`, while H
  independently hands off to A at `:455`. The queue has forked into two
  independent chains, both of which believe they own the lock in turn. Every
  node in both chains will run `set_locked()`.

So "do the skipped waiters get stranded?" — in the linear case they are reached
eventually (H still hands to A, A to B), which is why this failure is *worse*
than a starvation bug: it is silent double-entry into critical sections, not a
stall you would notice in a hang trace. The stall only shows up in the
tail-clearing sub-case.

### 3.3 Per-CPU node-slot recycling: can a stale handoff land on a slot reused for a *different* lock?

**Yes, and this is an independent, sufficient reason on its own.**

Nodes are per-CPU statics, not allocations:

```c
static DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[_Q_MAX_NODES]);
```

`qspinlock.c:138`, with `#define _Q_MAX_NODES 4` at `qspinlock.h:16`. Slot
tenure is bounded by the nesting counter alone:

* claim: `idx = node->count++` — `qspinlock.c:301`
* **release: `__this_cpu_dec(qnodes[0].mcs.count)` — `qspinlock.c:468`**

That `:468` decrement is the release path that makes reuse possible, and it runs
the instant a node finishes acquiring. Immediately afterwards the same CPU may
take a hardirq, re-enter `queued_spin_lock_slowpath()` **for an entirely
different lock**, receive the *same* `idx` at `:301`, and re-initialise the slot
at `:336-337`:

```c
	node->locked = 0;
	node->next = NULL;
```

Now consider the proposed releaser. It is **not a queue member** — it acquired,
ran its critical section, and is releasing. To find "the first alive waiter" it
must walk `node->next`, `node->next->next`, ... Each hop dereferences another
CPU's `qnodes[idx].mcs`, and *nothing* pins any of them. Between the releaser's
load of `X->next` and its store to `Y->locked`, Y can have acquired (via the
real head's handoff), returned through `:468`, and been re-queued on a different
lock. The releaser's `WRITE_ONCE(Y->locked, 1)` then lands on a node waiting for
**some other lock**, which promptly becomes head of *that* lock's queue and
enters *that* critical section without owning it. `struct mcs_spinlock` has no
generation counter, no lock back-pointer and no ABA protection
(`include/asm-generic/mcs_spinlock.h:4-8`: `next`, `locked`, `count` — that is
the whole struct), so this is undetectable.

**This is exactly what CNA's and ShflLock's position rule buys them.** The CNA
splicer is the primary head; it has *not yet acquired*, so nothing behind it can
have acquired either, so nothing behind it can have reached its `:468`
equivalent. Lifetime safety is a free consequence of queue position. The
proposed releaser has abandoned that position — it is the one party in the whole
protocol that provably holds no queue-derived lifetime guarantee over anything.

### 3.4 Preemption is not a latchable fact

Even setting aside 3.1-3.3, the predicate itself is unsound *for a correctness
decision*.

`vcpu_is_preempted()` and this project's TSC heartbeat
(`qspinlock_paravirt.h:305-457`, `is_wait_preempted()` at `:330`) are
**unlatched observations**. A vCPU observed as preempted at time T can be
running at T+1ns. There is no instant at which "the head is preempted" becomes a
settled fact the releaser can act on, because the hypervisor can reschedule the
head between the releaser's test and its store — including at the worst possible
point, between the head's `atomic_cond_read_acquire()` returning at `:413` and
its `set_locked()` store at `:447`.

Contrast: ShflLock's `S_PARKED` **is** latched, by construction —
`prev_status = SWAP(&qnext.status, S_READY)` is an atomic exchange, and the
waiter itself entered `S_PARKED` voluntarily and published it before sleeping.
CNA's `numa_node` is stable for the duration. Both designs put their policy
predicate strictly **off** the correctness path: guess the socket wrong and you
lose cache locality; guess the shuffler wrong and you lose a little throughput.
Nothing miscompiles.

Here the predicate would sit **on** the correctness path: guess wrong once and
two CPUs are in the same critical section. That difference — not "the papers
assume waiters are running" — is the sharpest formulation of the asymmetry.

A handshake could in principle latch it (e.g. the head CASes its own `head_ctl`
from `HEAD_SPINNING` to a new `HEAD_DEMOTED` before re-reading the lock word,
and the releaser only skips a head it successfully CASed into `HEAD_DEMOTED`).
But that handshake **requires the head to run** in order to honour it — and the
head being unable to run is the entire precondition for wanting to demote it. A
protocol whose safety depends on the participation of the party you have just
concluded cannot participate is not a protocol. This is where the asymmetry is
genuinely fatal rather than merely awkward.

### 3.5 The PV hash one-entry-per-lock invariant

The invariant is stated in `pv_hash()`'s own comment, `qspinlock_paravirt.h:269-279`:

```c
	/*
	 * Hard assume there is a free entry for us.
	 *
	 * This is guaranteed by ensuring every blocked lock only ever consumes
	 * a single entry, and since we only have 4 nesting levels per CPU
	 * and allocated 4*nr_possible_cpus(), this must be so.
	 *
	 * The single entry is guaranteed by having the lock owner unhash
	 * before it releases.
	 */
	BUG();
```

and symmetrically in `pv_unhash()`, `:295-302`:

```c
	/*
	 * Hard assume we'll find an entry.
	 *
	 * This guarantees a limited lookup time and is itself guaranteed by
	 * having the lock owner do the unhash -- IFF the unlock sees the
	 * SLOW flag, there MUST be a hash entry.
	 */
	BUG();
```

**"every blocked lock only ever consumes a single entry"** is a direct corollary
of "there is exactly one head." There are exactly two `pv_hash()` call sites:

* `qspinlock_paravirt.h:919`, in `pv_kick_node()` — the releaser hashing its
  *successor* when that successor is confirmed `VCPU_HALTED` by the
  `try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED)` at `:908`;
* `qspinlock_paravirt.h:1021`, in `pv_wait_head_or_lock()` — the head hashing
  *itself* before `xchg(&lock->locked, _Q_SLOW_VAL)` at `:1034`.

Two simultaneous heads means both can reach `:1021` for the **same** lock and
insert **two** entries. The release side calls `pv_unhash()` exactly once
(`:1162`) and clears exactly one `he->lock` (`:291`). The second entry is
permanently leaked with `he->lock` non-NULL. Leak enough of them and
`pv_hash()`'s `for_each_hash_entry()` walk exhausts the table and takes the
**`BUG()` at `:279`**. That is a deterministic, non-silent crash — and given
`__pv_init_lock_hash()` sizes the table at `4 * num_possible_cpus()`
(`:232`), it does not take many.

There is also a `_Q_SLOW_VAL` interaction: `__pv_queued_spin_unlock()`'s
`try_cmpxchg_release(&lock->locked, &locked /* == _Q_LOCKED_VAL */, 0)` at
`:1196` is *predicated* on the lock word being plain `_Q_LOCKED_VAL`. A second
head doing `xchg(&lock->locked, _Q_SLOW_VAL)` at `:1034` under a genuine holder
diverts that holder into `__pv_queued_spin_unlock_slowpath()` (`:1138`), which
unhashes an arbitrary one of the two entries and `pv_kick()`s whichever node it
found (`:1178`). The "wrong" head is then kicked and the right one is not.

---

## 4. Phase 4 — verdict

### (B) Not achievable.

**The single specific invariant that cannot be maintained:**

> At most one node is in the head state at any instant, and a node enters the
> head state only via the one-shot handoff its unique predecessor performs at
> `kernel/locking/qspinlock.c:455`.

**The code that establishes it:**

* `qspinlock.c:447` — `set_locked(lock)`, expanding to
  `WRITE_ONCE(lock->locked, _Q_LOCKED_VAL)` (`qspinlock.h:196-199`). A plain
  store. The queue path's mutual exclusion *is* the one-head invariant; there is
  no second line of defence.
* `qspinlock.c:437-440` — the tail-clearing `atomic_try_cmpxchg_relaxed()`,
  which only the genuine tail may execute; a spurious head running it publishes
  an empty queue while nodes are still queued.
* `qspinlock.c:453` — `smp_cond_load_relaxed(&node->next, (VAL))`, an unbounded
  wait with no escape, which is where the resulting orphaned sub-queue hangs.
* `qspinlock.c:468` — `__this_cpu_dec(qnodes[0].mcs.count)`, which makes a node
  slot immediately reusable **for a different lock** and gives any non-queue-member
  walker a use-after-recycle.
* `qspinlock_paravirt.h:279` and `:302` — the two `BUG()`s enforcing
  one-hash-entry-per-lock, itself a corollary of one-head.
* `qspinlock.h:52-69` — `encode_tail`/`decode_tail`: `lock->val` carries only
  the **tail**, so no pointer to the head exists anywhere and a head cannot be
  unlinked, only ignored.

**Why CNA and ShflLock don't hit it and this does:**

CNA and ShflLock reorder *strictly behind* the head, *from* a party that is
itself in the queue ahead of everything it touches (CNA: the primary head;
ShflLock: the token-holding shuffler, originated by the head). Queue position is
simultaneously their mutual exclusion and their memory-lifetime guarantee, and
neither ever changes *who gets the lock next* — they change *who is standing
where behind the head*. ShflLock even codifies the prohibition explicitly:
"The successor of the lock holder, if it exists, always keeps its position
intact in the queue."

This proposal inverts every one of those: the actor is the **releaser**, which
holds no queue position and therefore no exclusion and no lifetime guarantee; the
target is the **head**, which nothing points to and which cannot be unlinked; and
the effect is to change *who gets the lock next*, creating a second head. On top
of that the triggering predicate (vCPU preempted) is unlatchable and sits on the
correctness path, whereas both papers' predicates (NUMA node, `S_PARKED`) are
stable and sit only on the performance path.

CNA's own installer declines to run in this environment at all
(`cna_configure_spin_lock_slowpath()` bails when the PV slowpath is installed),
which is a fair summary of the situation: neither published design was built for
a paravirt guest, and the reason is not incidental.

**Byte budget, for completeness:** even if the protocol worked, CNA's 8 bytes
and ShflLock's larger field set do not fit. `sizeof(struct pv_node) == 32 ==
sizeof(struct qnode)` is exact (verified by `pahole`), with only a 3-byte
padding hole at offset 21-23 genuinely free; the other 6 reclaimable bytes are
`head_ctl`'s `gen`/`yields` fields, already reserved for IVH Idea 2 Stage 1.

## 5. What *is* buildable (scope-reduced — NOT what was asked)

Stated plainly so it is not mistaken for a rescue of the above: this does **not**
let you bypass a head that is already preempted. It stops a preempted waiter
from *becoming* head in the first place.

**Protocol: CNA verbatim, with a liveness policy substituted for the NUMA policy.**

* **Who splices:** the current queue head, inside `pv_wait_head_or_lock()`
  (`qspinlock_paravirt.h:940`) — the same slot CNA uses, while it is already
  burning its `SPIN_THRESHOLD` spin doing nothing useful.
* **What exclusion it holds:** queue position — the identical, unchanged
  structural argument CNA relies on, which section 2.2 confirms this kernel
  still provides. Everything it touches is provably still queued behind it.
* **What it does:** `next = node->next`; if `next->next != NULL` (the CNA tail
  rule, non-negotiable) and `next` is judged preempted, run `cna_splice_next()`
  to move `next` onto a secondary "parked" queue rooted in the splicer's own
  `mcs.locked`. Hand off to `nnext` instead. The preempted waiter is
  **deferred, not skipped** — it is still in a queue, still gets the lock, and
  the one-head invariant is untouched. Re-merge via `cna_splice_head()` /
  `cna_try_clear_tail()` exactly as CNA does.
* **Per-node state and where it fits:** only `encoded_tail` is needed (the NUMA
  fields are not). Do not store it: put a **`u8 idx` in the 3-byte padding hole**
  at `pv_node` offset 21 and recompute `encode_tail(pn->cpu, pn->idx)` on demand
  — `encode_tail()` is `__pure` and two shifts (`qspinlock.h:52-60`). Cost: 1 of
  the 3 free bytes. `head_ctl` is left entirely alone, so Idea 2 Stage 1 is
  unaffected.
* **Why the unlatchable predicate is tolerable here:** because it is back on the
  *performance* path. Splice a healthy waiter by mistake and you have merely
  reordered the queue — precisely CNA's own failure mode, and harmless.

**Top risks, in order:**

1. **`mcs.locked` overload vs. PV.** CNA's `locked > 1` convention must coexist
   with PV's use of `node->locked` as the halt/wake predicate
   (`qspinlock_paravirt.h:694`, `:847`, `:871`) and with `pv_kick_node()`'s
   `VCPU_HALTED -> VCPU_HASHED` cmpxchg at `:908`. The truthiness tests survive
   (an encoded tail is always > 1), but `arch_mcs_spin_unlock_contended()` at
   `qspinlock.c:455` must become a `cna_lock_handoff()` equivalent that passes
   the secondary tail through, and every PV site that assumes `locked == 1`
   needs an audit. This is the item most likely to produce a subtle bug.
2. **Starvation of the secondary queue.** CNA needed three extra patches
   (v15 4/6 through 6/6) for this. A "parked" queue under sustained host
   overcommit could be starved far longer than CNA's remote-NUMA queue ever is.
   An intra-queue threshold plus unconditional flush is mandatory, not optional.
3. **Interaction with `pv_hybrid_queued_unfair_trylock()`
   (`qspinlock_paravirt.h:127`).** Stealers bypass the queue entirely; a longer
   effective queue changes the stealing/pending-bit dynamics that the current
   adaptive-lock work is already measuring. Any measurement of this must hold
   the steal window fixed, or the two effects will confound — the same confound
   already documented in `ivh_afl_confound_and_threshold_search_2026-09-12-night.md`.
4. **Benefit is structurally capped.** It only helps when the *successor* is
   preempted while the head is healthy. The head-already-preempted case — the
   one actually asked about — remains unaddressed by this or by anything else
   in the queue protocol.

Given risk 4, the honest read is that section 5 is a modest win at
non-trivial complexity, and the owner's stated fallback (redirect to benchmark
research) is a defensible call.

---

## Sources

Papers and patch series:

- [Scalable and Practical Locking with Shuffling (ShflLock), Kashyap et al., SOSP 2019 — PDF](https://gts3.org/assets/papers/2019/kashyap:shfllock.pdf)
- [ShflLock, ACM DL](https://dl.acm.org/doi/10.1145/3341301.3359629)
- [sslab-gatech/shfllock — public source (klocks / ulocks)](https://github.com/sslab-gatech/shfllock)
- [PATCH v15 3/6 locking/qspinlock: Introduce CNA into the slow path of qspinlock](https://www.spinics.net/lists/arm-kernel/msg894733.html)
- [PATCH v15 0/6 Add NUMA-awareness to qspinlock](https://lkml.iu.edu/hypermail/linux/kernel/2105.1/10243.html)
- [Add NUMA-awareness to qspinlock — LWN.net](https://lwn.net/Articles/804137/)

Kernel sources read (`/root/kernels/linux-6.17-vanilla`):

- `kernel/locking/qspinlock.c`
- `kernel/locking/qspinlock.h`
- `kernel/locking/qspinlock_paravirt.h`
- `kernel/locking/mcs_spinlock.h`
- `include/asm-generic/mcs_spinlock.h`

Struct sizes independently confirmed with `pahole -C pv_node
kernel/locking/qspinlock.o` and a standalone `sizeof`/`offsetof` program.
