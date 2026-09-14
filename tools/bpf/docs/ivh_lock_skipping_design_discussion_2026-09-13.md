# Lock skipping: the idea, why it's unsafe as originally conceived, and the safe path forward

2026-09-13. Companion to the AFL work — this is a kernel-side idea (MCS/qspinlock queue
reordering), unrelated to the userspace adaptive lock.

## 1. The original idea

When a lock is freed and the current queue head is PREEMPTED, instead of handing the lock to that
stalled head, walk forward through the MCS wait queue and hand it to the first waiter that is (a)
ALIVE (not itself preempted) and (b) has some minimum runway left (the owner floated reusing
`last_cs_ns` for this) — then fix up the queue so that promoted waiter becomes the new effective
head, with everyone previously between the old head and the new one now following behind it.

The pointer surgery as originally described: four writes on a doubly-linked structure —
`new_holder.next = old_head`, `new_holder.prev.next = new_holder.old_next`,
`old_head.prev = new_holder`, `new_holder.old_next.prev = new_holder.old_prev`.

## 2. Scoping — what's actually true, corrected after an initial tree mixup

A scoping investigation (Opus, full agent pass) was dispatched to answer this rigorously. It
initially read `/root/linux-6.17` (a stale kernel checkout sitting inside the *docs* repo, dated
weeks earlier) instead of `/root/kernels/linux-6.17-vanilla` (the actual, actively-developed tree)
despite being pointed at the latter explicitly. This was caught and one number corrected directly
against the real tree before trusting anything: the agent claimed `struct pv_node` had 8 spare
bytes under the 32-byte `qnode` ceiling; in the real tree it has **zero** — `head_ctl` (added by
this session's own earlier "hybrid head-role takeover" work) already fills that headroom exactly.
This correction doesn't change the verdict below, because the verdict never depended on needing a
new field in the first place (see §3.1).

## 3. Why it's unsafe — not "risky," structurally broken

### 3.1 You don't need a `prev` pointer at all

The queue only ever needs forward traversal to find the promotion target. The whole four-pointer
splice reduces to two writes on the existing singly-linked `next` chain: walk from the preempted
head via `next` to find the predecessor `P` of the target node `N`, then `P->next = N->next` (skip
N out of its old slot) and `N->next = old_head` (N now leads the rest of the queue). No new
per-node state needed, no byte-budget question to resolve.

### 3.2 The actual killer: nodes cache their successor in PRIVATE state before using it

This is the one that matters, and it's worth being concrete about, since "public vs. private" is
easy to say and easy to miss in practice. From `kernel/locking/qspinlock.c` (line numbers as of
this kernel):

```c
        next = READ_ONCE(node->next);   // PUBLIC -> PRIVATE. node->next is a real, shared memory
        if (next)                       // location any CPU with the pointer can read or write.
                prefetchw(next);        // `next` (bare, no arrow) is now a LOCAL VARIABLE -- it
                                         // lives in THIS CPU's own register/stack, physically
                                         // unreachable by any other CPU, not just "hard to reach."

        /* ~70 lines of other code run here -- waiting for the lock, claiming it.
         * This CPU is not looking at node->next again during any of this. */

        if (!next)
                next = smp_cond_load_relaxed(&node->next, (VAL));  // only re-reads if the FIRST
                                                                    // read came back NULL

        arch_mcs_spin_unlock_contended(&next->locked);  // uses the PRIVATE copy, unconditionally.
        pv_kick_node(lock, next);                       // Whatever node->next says RIGHT NOW is
                                                          // irrelevant -- this CPU already decided,
                                                          // ~70 lines and possibly milliseconds ago.
```

The comment at the `READ_ONCE` site says the quiet part out loud: *"we optimistically load the next
pointer... to reduce latency."* Skipping the re-check is the design's whole point — a real,
load-bearing performance optimization that has held for years because exactly one predecessor ever
writes a given node's `next` field, exactly once, by construction. Queue-skip logic reaching in and
rewriting `node->next` on a node some other CPU has already read into its own private `next` would
be the first thing to ever violate that invariant. That CPU is not coming back to check; there is no
code path that does.

**Concretely, what breaks**: acquirer A sees successor H preempted, walks to alive node N, does the
splice, marks N as the new target. N wakes, gets the lock, finishes, releases its qnode slot back to
the small per-CPU pool (`qnodes[_Q_MAX_NODES]`, only 4 deep). N's CPU immediately queues for a
*different*, unrelated lock B, reusing that same freed slot. Meanwhile P — N's *original*
predecessor in lock A's queue, who cached `next = N` long before any of this — finally reaches the
front of lock A's own queue and executes its already-decided, already-cached handoff: write into
what it still believes is N's `locked` field. But that memory is now lock B's queue slot. P's stale
write can hand lock A's baton to a CPU that's actually mid-wait for a completely different lock,
producing a double-acquire of lock B — a lock the skip logic never touched, in a subsystem that has
nothing to do with the original change. This is why it's not just "risky" — there is no fix that
stays within "rewrite some pointers," because the corruption isn't in the pointers, it's in a
decision another CPU already made and is holding privately.

### 3.3 Two more independent breaks found, for completeness

- **The queue tail is encoded in the lock word itself** (`xchg_tail`), not in any node. If N happens
  to be the current tail when promoted, a concurrent new arrival's `WRITE_ONCE(prev->next, node)`
  can race the splice's own write to the same field — whichever lands second silently orphans a
  subchain, which then spins forever with nobody obligated to ever wake it.
- **The PV hash table assumes exactly one entry per lock**, enforced by a `BUG()` on collision
  (`pv_hash()`/`pv_unhash()`). A promoted node re-entering the head-wait path can hash the same lock
  a second time; whichever unhash wins leaks the other entry, eventually tripping that `BUG()`.

### 3.4 The in-tree precedent that proves the point

`kernel/locking/osq_lock.c`'s optimistic queue *is* doubly-linked (it has `next`, `prev`, `locked`,
`cpu` per node) and *does* support removing a node from the middle of the queue — the exact
capability this idea wants. But it only works because of three properties this proposal doesn't
have: (1) a node only ever removes **itself**, never a third party removing someone else's node; (2)
it uses a `cmpxchg`/`xchg` handshake specifically to make its own predecessor and successor pointers
provably stable before touching them; (3) the outcome is **cancellation** (the caller gives up and
goes to sleep some other way), never a **handoff** — so the stale-cached-successor problem in §3.2
can't arise, because nobody is ever told "you now own this lock" as a side effect of the removal.
qspinlock hands out mutual exclusion on every promotion; osq_lock's queue never does.

## 4. Verdict

Not safely buildable as originally described, for a structural reason (private, already-acted-upon
state in another CPU's register/stack) that no amount of additional pointer-management fixes. Two
further independent hazards (§3.3) exist even if §3.2 were somehow solved.

## 5. The recommended safe path (not yet built — this is the plan for a follow-up session)

**Step 0 — detect-only, zero pointer writes, behavior-neutral.** At the qspinlock unlock site,
before handing off to the cached `next`, check `is_wait_preempted(next->cpu)` (the same TSC-heartbeat
check tier-2 already uses) and count it. If preempted, walk forward via `READ_ONCE` (read-only, hard
hop cap, e.g. 8) looking for the first non-preempted node, and histogram how deep it had to go (or
"none found within the cap"). This measures the entire addressable value of the idea — how often
does it even matter, and is there usually someone alive nearby — without writing a single pointer.
If the number is small, the idea is dead on economics, cheaply, before any risky code exists.

**Step 1 — if Step 0's number justifies it, the safe lever already exists in this kernel.**
`pv_hybrid_queued_unfair_trylock()` already lets a running (not-yet-queued) waiter steal the lock
ahead of a stalled queue, by racing the lock word itself rather than mutating anyone's node — this
is "skipping" implemented the only way that's actually safe here. It's already instrumented
(`ivh_lock_steals`). The tunable surface is the pending-bit policy governing when that steal window
is open, not `->next`. Widening that window specifically when `is_wait_preempted(head_cpu)` is true
delivers the original intent with zero queue-invariant risk, at a documented, pre-existing cost
(weakens the starvation bound — already called out in `ivh_cs_head_check()`'s own comment).
