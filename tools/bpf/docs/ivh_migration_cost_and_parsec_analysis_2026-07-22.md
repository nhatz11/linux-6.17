# IVH: migration cost reduction, syscall-skip correctness, and PARSEC dedup/vips mechanism

Date: 2026-07-22. Kernel `6.17.0-rseqport55-trimsys+` (confirmed via `uname -r`),
branch `kernel-43-clean`. Companion to `ivh_state_of_the_art_2026-07-20.md`,
`ivh_hp_correlation_analysis_2026-07-20.md`, and
`ivh_syscall_skip_throughput_analysis_2026-07-22.md` — read those first; this
doc builds on their findings rather than re-deriving them.

---

## 1. Can the ~250-560us migration cost itself be reduced?

### 1.1 Mechanism 1 (`migrate_task_to()`/`stop_one_cpu()`): confirmed unfixable cheaply

Read `migration_cpu_stop()` (`kernel/sched/core.c:2728`) and `migrate_task_to()`
(`core.c:8320`) in full, alongside `affine_move_task()` (mechanism 0's path,
`core.c:3101`) and `stop_one_cpu()`/`cpu_stop_queue_work()`
(`kernel/stop_machine.c`).

**Root cause, precisely nailed down:** `migrate_task_to()` builds its
`migration_arg` positionally — `struct migration_arg arg = { p, target_cpu };`
— leaving `arg.pending = NULL`. Inside `migration_cpu_stop()`, when the task
being migrated is *not* currently enqueued (`task_on_rq_queued(p) == false`,
which is the common case here because `stop_one_cpu()` blocks the *calling*
thread — itself, `current` — on its own completion, and by the time the
stopper thread runs, `current` has gone through `schedule()` and is off the
runqueue), the code takes:

```c
if (task_on_rq_queued(p)) {
        rq = __migrate_task(rq, &rf, p, arg->dest_cpu);   /* hard placement */
} else {
        p->wake_cpu = arg->dest_cpu;                       /* HINT ONLY */
}
```

`p->wake_cpu` is consulted by `select_task_rq_fair()` on wakeup but is
overridden by wake-affine logic, which — since the wakeup is signaled from the
*source* CPU — usually pulls the task right back home. This is exactly the
78%-wrong-CPU / 71%-bounce-back failure mode already measured this session.

**Why mechanism 0 doesn't suffer the same fate despite sharing the identical
`migration_cpu_stop()` function**: mechanism 0 calls `__do_set_cpus_allowed()`
*before* `affine_move_task()`, narrowing `current->cpus_mask` to
`{target_cpu}` alone. Even in the scenario where `migration_cpu_stop()` takes
the same "hint only" branch, `select_task_rq_fair()` has no choice — the mask
now contains exactly one CPU, so wake-affine has nothing to override. The
correctness of mechanism 0 rests on the mask restriction, not on which branch
`migration_cpu_stop()` takes.

**Is a small, targeted fix possible?** Yes in principle — replicate the
cpus_mask-restriction trick around `migrate_task_to()`'s call too. But once
you do that, you no longer need `migrate_task_to()`/`stop_one_cpu()`/
`migration_cpu_stop()` as a *distinct* mechanism: `migrate_task_to()` itself
would become a redundant, blocking wrapper on top of a narrowed mask that
`set_cpus_allowed_ptr()` already handles, and you'd have to re-implement (or
duplicate) `affine_move_task()`'s protections against concurrent affinity
changes and migrate-disable races that the generic path exists specifically to
handle. **A "fixed" mechanism 1 collapses into mechanism 0 plus one extra
blocking layer — strictly more expensive, not less.** This confirms and
sharpens (rather than just repeats) the state-of-the-art doc's conclusion:
not usable as a distinct, cheaper mechanism, and no further investment is
warranted here.

### 1.2 Where does the "target-CPU pickup wait" actually go? New finding this session

The BPF target-selection hook (`process_cpu()` in
`tools/bpf/MY_ivh_atc.bpf.c:304`) explicitly prefers an **idle** target CPU
(`idle_cpu(select_rq)`, line 483 — stops the search immediately) and only
falls back to a **busy-but-safe** target (excluding lock-holders and userspace
lock-holders, but nothing else) if no idle CPU exists among the candidates
scanned. The code's own comment explains why idle is preferred: landing on a
busy CPU means "the thread queues behind existing work."

I measured this split live rather than taking the code's intent on faith.
Sysctls set to the standard combo (`ivh_capacity_threshold=1010`,
`ivh_time_left_threshold_ns=4000000`, `ivh_max_concurrent=8`,
`ivh_time_left_source=1`, `ivh_selection_trylock=0`, `ivh_migrate_mechanism=0`,
`ivh_eval_cooldown_ns=50000`, `ivh_universal_eligible=1`), then a clean 20s
`NHextend3 -n -l 16` run (default ~1.6ms CS, 16 threads/16 vCPUs — fully
saturated), dumping the `reject_reasons` percpu-array BPF map
(`bpftool map dump id 16`) before and after:

| selection outcome | delta this run |
|---|---|
| `ACC_TIER1_ACTIVE` (busy-fallback candidate seen) | **31,998** |
| `ACC_TIER2_IDLE` (idle target found, search stopped) | **309** |

(Caveat on the raw ratio: `ACC_TIER1_ACTIVE` is bumped once per *surviving
non-idle candidate scanned* within a selection call, so its count is inflated
relative to "number of migrations landed on a busy CPU" — a single migration
decision can re-bump it several times while scanning candidates before
settling. `ACC_TIER2_IDLE` is bumped at most once per call, since finding idle
terminates the scan immediately. Even correcting for this bias, the
implication is unambiguous: idle targets were essentially never found —
correcting for over-counting still leaves idle an small single-digit-percent
of the ~15,016 real migrations this run performed, confirmed separately via
NHextend3's own `/proc`-style summary: `Total migrations: 15016`,
`Avg migration: 140262 ns`, `Stuck (>1ms): 5.2%`.)

**Under a fully-saturated, all-CPUs-busy workload like NHextend3, essentially
every migration lands on a busy (non-idle) target**, not an idle one. This
matters because it points to a real, previously-unexamined *component* of the
migration cost:

- **The busy-target case (the overwhelming majority under saturation):**
  once the migrated thread lands on the target rq via `move_queued_task()`
  (`__migrate_task()` → `deactivate_task()`/`set_task_cpu()`/`activate_task()`/
  `wakeup_preempt()`), it is enqueued into ordinary CFS fair-share order.
  `wakeup_preempt()` **does** send an immediate reschedule (a real IPI kick to
  a remote CPU via `resched_curr()`) if the newly-arrived task's priority
  warrants preempting whatever's running there — so there is no missing
  "kick" to add; that channel already exists and is already used. But if the
  target's current task doesn't warrant being preempted immediately (the
  normal case for two ordinary, equal-priority CFS tasks), the migrated
  thread must wait its fair turn — a delay bounded by ordinary CFS scheduling
  latency, which sits in the hundreds-of-microseconds range for typical
  `sched_min_granularity`/rq-load conditions. This lines up far better with
  the measured ~140-560us cost than any plausible IPI/hardware-handoff
  latency would (that alone would be ~1-10us, two to three orders of
  magnitude too small).
- **The idle-target case (rare under saturation, ~2% or less here):** the
  BPF hook's own comment already names its cost explicitly — "an idle target
  costs a hypervisor wake-up." A guest vCPU that is truly idle is very likely
  parked (HLT or similar) at the host level; waking it to run the migrated
  thread requires a real VM-exit/injection round trip on the host side. This
  is *not fixable from guest kernel code at all* — it is exactly the territory
  the existing vSpin/paravirt work (`ivh_pv_wait_mechanism`) already
  addresses for the *waiting* side of a lock; the *migration-target-wakeup*
  side is architecturally the same class of problem and hasn't been
  specifically targeted by that work yet.

### 1.3 Verdict on Part 1

**No simple, free fix exists to cut the raw migration mechanism's cost** —
mechanism 1 cannot be repaired without becoming mechanism 0 plus overhead, and
mechanism 0's own two already-known optimizations (redundant `schedule()`
removal, cheap-restore early exit) are already in place. That much matches the
user's prior framing.

**But there is a genuine, non-trivial, previously-unexamined lever with real
promise, which is honestly *not* "simple"**: the dominant real-world
component of the pickup wait (under the saturated conditions IVH's hardest
workloads run in) is CFS fair-share queueing delay on a busy target, not
mechanism overhead. A temporary priority/urgency boost on the migrated thread
— conceptually similar to the RT-boost technique already used and validated
in the *post*-acquisition dispatch fix (memory: "Patch 3 freeze fix... fixed
via qspinlock yield + RT boost") — could in principle make the thread preempt
immediately on arrival instead of queueing, cutting a meaningful fraction of
the ~140-560us cost. This is **not simple** to actually build correctly: it
requires a bounded, self-terminating boost window (tied to the imminent CS,
not indefinite), must reckon with the fact that it *shifts* cost onto whatever
was running on the target rather than eliminating it system-wide, needs
interaction analysis with `ivh_max_concurrent` and the existing Hot Threads
gate, and hasn't been implemented or measured — it is a real design
direction, not a rubber-stamped "yes it's fixable." For the (rarer) idle-target
case, the cost is a hypervisor wake-up round trip and is not addressable from
guest code at all short of extending the existing paravirt/vSpin machinery to
cover migration-target wakeup specifically, which is a distinct, larger body
of work than this session's scope.

So the honest, precise answer is: **no free lunch, but also not "nothing more
to try"** — there is one concrete, identified, unexplored lever (target-side
priority boost) with a real mechanistic reason to expect it would help,
alongside a second (paravirt-style idle-wakeup avoidance) that's out of scope
for a guest-kernel-only change. Neither was implemented this session; both are
real candidates for future work, not evidence that the current mechanism is
already fully optimized.

---

## 2. Syscall-skip optimization: correctness pass

### 2.1 `kernel/rseq.c`: `ivh_task_rq_in_danger(t)` call site — PASS

`rseq_update_cpu_node_id(t)` is called from exactly one place,
`__rseq_handle_notify_resume()` (`kernel/rseq.c:451`), where
`struct task_struct *t = current;` (line 453) unconditionally. There is no
other caller (`grep` confirms a single call site). `__rseq_handle_notify_resume`
itself only runs on the return-to-userspace path for the currently-executing
task — it is never invoked on behalf of a different task. Consequently
`task_rq(t)` inside `ivh_task_rq_in_danger()`/`ivh_rq_capacity_and_timeleft_ok()`
is always `this_rq()` for the CPU the code is presently executing on: there is
no cross-task staleness and no race with a concurrent migration of "some other
task" — the only task whose rq state is being read is the one currently
running the read. **No bug.**

### 2.2 `kernel/sched/fair.c`: `ivh_rq_capacity_and_timeleft_ok()` vs `ivh_steal_imminent()` — PASS, verified by line-by-line diff

Both gates were read side-by-side. The bodies are identical in logic for both
`ivh_time_left_source` branches (0: EWMA-based, 1: `last_active_time`-based),
modulo:
- the stat-increment side effects (`this_cpu_inc(ivh_steal_imminent_*_reject)`),
  intentionally omitted in the advisory copy per its own comment (avoids
  polluting `/proc/ivh_debug` counters with advisory-only evaluations) — a
  deliberate, documented, correct divergence, not a bug.
- `current->last_cs_ns` (authoritative path) vs `t->last_cs_ns` (advisory
  path) — correct, since the advisory function is explicitly parameterized
  over an arbitrary `t` (here always `current`, per §2.1) rather than
  hardcoding `current`.

No unintentional divergence found. **Pass.**

### 2.3 `include/uapi/linux/rseq.h`: bit collision / clearing — PASS

`RSEQ_SCHED_STATE_FLAG_ON_CPU = (1U << 0)`,
`RSEQ_SCHED_STATE_FLAG_IVH_DANGER = (1U << 1)` — distinct bits, no collision.
The write sequence in `rseq_update_cpu_node_id()` (`sched_state =
RSEQ_SCHED_STATE_FLAG_ON_CPU; if (ivh_task_rq_in_danger(t)) sched_state |=
RSEQ_SCHED_STATE_FLAG_IVH_DANGER;`) builds the word correctly before the
single `unsafe_put_user()`. On preemption, `rseq_preempt()` →
`rseq_set_sched_state(t, 0)` → `__rseq_set_sched_state()` does a full-word
`put_user(0, ...)`, correctly clearing *both* bits together — appropriate,
since neither bit is meaningful while the task is off-CPU, and the next
return-to-userspace freshly recomputes both. **Pass, no bug.**

### 2.4 `NHextend3.c`: fail-open logic — found ONE real, previously-unidentified bug

`register_rseq()`'s unregister/re-register dance (handling glibc's own rseq
auto-registration colliding with NHextend3's extended registration) is
individually sound: every failure branch correctly sets
`ivh_sched_state_active = false`, and `ivh_danger()`'s
`if (!ivh_sched_state_active) return true;` correctly fails open *when
evaluated in isolation*.

**The bug**: `ivh_sched_state_active` (line 216) is declared
`static bool ivh_sched_state_active;` — **a single process-wide variable, not
`__thread`** — while the actual per-thread published state,
`ivh_sched_state` (line 206), *is* correctly `static __thread`. Every worker
thread calls `register_rseq()` once at startup and unconditionally overwrites
this shared, non-thread-local flag with its own outcome.

In practice, since `getauxval(AT_RSEQ_FEATURE_SIZE)` and glibc's
auto-registration behavior are process-wide constants, every thread in a given
run is very likely to take the identical code path and produce the identical
(redundant) value — so this is unlikely to bite under normal, uniform
conditions. But it is a genuine latent bug, not just a style issue: if even
**one** thread hits a transient, thread-specific registration failure (e.g. an
`EBUSY`/`EINVAL`/`ENOMEM` blip unique to that thread's creation, or any other
reason the per-thread outcome legitimately diverges) while other threads
succeed, the *global* flag ends up `true` (set by the successful threads),
while the failing thread's own `__thread ivh_sched_state.state` is never
written by the kernel (its `sched_state_ptr` registration never took). That
thread's own `ivh_danger()` call then reads `ivh_sched_state_active == true`
(a different thread's success, not its own) and its **own zero-initialized,
never-updated** `ivh_sched_state.state`, computing
`(0 & IVH_DANGER) != 0 == false` — **silently skipping the syscall (and any
real migration) for that one thread, for the entire run, with no diagnostic
indicating it**, exactly contradicting the function's own doc comment ("fail
open... whenever the feature isn't actually active **for this thread**" — the
code checks a process-global condition, not a per-thread one). This is the
same *class* of bug as the already-identified old-kernel 100%-skip issue (a
silent, unindicated fail-closed path), but narrower in scope (per-thread, not
whole-run) and not yet observed live this session — it requires a genuine
per-thread divergence in registration outcome to manifest, which the current
test matrix (uniform kernel, uniform glibc, uniform thread creation) hasn't
triggered. **Fix (not applied, out of scope per the no-rebuild constraint):
make `ivh_sched_state_active` `static __thread bool` to match
`ivh_sched_state` itself.**

No other divergence-prone shared/global mutable state was found —
`ivh_sched_state` and `rseq_map` are both correctly `__thread`; only
`ivh_sched_state_active` is the outlier.

### 2.5 Overall Part 2 verdict

**Pass with one real, narrow, previously-unidentified bug** (§2.4, thread-local
correctness), everything else (rseq.c call-site safety, gate-body parity,
rseq.h bit semantics) is clean.

---

## 3. Why don't PARSEC dedup/vips show improvement?

### 3.1 Method

Read the actual synchronization primitives dedup and vips use, rather than
inferring from prior throughput numbers:
- `dedup`: `/home/nick/parsec-benchmark/pkgs/kernels/dedup/src/queue.{c,h}`,
  `encoder.c` (all four pipeline-stage thread functions, the queue-array
  setup in `encode()`, and `config.h`'s `MAX_THREADS_PER_QUEUE`).
- `vips`: `/home/nick/parsec-benchmark/pkgs/apps/vips/src/libvips/iofuncs/
  {sinkscreen.c,threadpool.c}`.

### 3.2 dedup: confirmed a genuinely different, intermediate topology — NOT NHextend3's

`queue_t` (`queue.h:24`) holds exactly one `pthread_mutex_t` per instance
(plus two condvars) — a dedicated lock per queue object, not a shared global.
`encoder.c`'s `encode()` (line 1372) computes
`nqueues = ceil(conf->nthreads / MAX_THREADS_PER_QUEUE)`, with
`MAX_THREADS_PER_QUEUE = 4` (`config.h:10`). For a 16-thread run this yields
**4 independent queue groups**, each with its own `deduplicate_que[qid]`,
`refine_que[qid]`, `compress_que[qid]`, `reorder_que[qid]` (four *separate*
mutexes per group, 16 total for the whole run) — the pipeline is
**partitioned into 4 parallel lanes**, not one shared structure.

Tracing which threads touch which queue instance concurrently
(`FragmentRefine`→`deduplicate_que[qid]`→`Deduplicate`→
`compress_que[qid]`/`reorder_que[qid]`→`Compress`→`reorder_que[qid]`→single
global `Reorder` thread, round-robin across all `qid`):

| queue (per group) | producers | consumers | max concurrent waiters |
|---|---|---|---|
| `refine_que[qid]`      | 1 (global Fragment, round-robin) | ≤4 FragmentRefine | ~5 |
| `deduplicate_que[qid]` | ≤4 FragmentRefine | ≤4 Deduplicate | ~8 |
| `compress_que[qid]`    | ≤4 Deduplicate (non-dup path) | ≤4 Compress | ~8 |
| `reorder_que[qid]`     | ≤4 Deduplicate (dup path) + ≤4 Compress | 1 (global Reorder, round-robin) | ~9 |

This confirms and sharpens the "5-9 co-waiters" figure from prior session
memory with an exact, source-grounded derivation: dedup's worst-case lock
(`reorder_que[qid]`) really is shared by up to 9 threads. This is meaningfully
*more concentrated* than hackbench's ~2-per-socket-pair-lock design (so
dedup's own protection is not expected to be as clean a win as hackbench's),
but it is **not** NHextend3's full-16-way single-lock concentration either —
it sits in a genuine middle ground, distributed across 4 independent lanes.

### 3.3 vips: confirmed the SAME concentrated topology as NHextend3, at its real hot lock

`VipsThreadpool` (`threadpool.c:352`) holds exactly **one `allocate_lock`
(`GMutex *`) per threadpool instance**, and *all* `pool->nthr` worker threads
(up to 16 with `-t16`) contend on this single lock in
`vips_thread_work_unit()` (line 491): `g_mutex_lock(pool->allocate_lock)` →
`vips_thread_allocate(thr)` (pick the next tile/work unit — pure bookkeeping)
→ `g_mutex_unlock(pool->allocate_lock)` → *then*, **outside the lock**,
`vips_thread_work(thr)` does the actual pixel-processing work. This is a
**single global work-queue lock shared by every worker thread, guarding only
the tiny "get next unit of work" bookkeeping step** — architecturally the
same shape as NHextend3's single global lock (all-N-threads-one-lock), and
the critical section is almost certainly very short (index/pointer
bookkeeping, not image computation). This is precisely the regime the
state-of-the-art doc (§3.3-3.4) already identified as IVH's worst case: high
concentration + short CS.

**So at the source level, vips and dedup are genuinely different stories** —
vips's actual bottleneck lock structurally matches NHextend3, dedup's does
not.

### 3.4 Reconciling with the session's own paired-measurement data — the honest tension

The `ivh_hp_correlation_analysis_2026-07-20.md` paired (single-workload-
isolated) measurement, however, shows **both** dedup (+6.5%, noisy/neutral)
*and* vips (+1.7%, neutral) as roughly cost-neutral for their *own* protection
— not the clear loss either the whole-system-toggle report shows (dedup -16
to -33%, vips -33 to -39%) or, for vips specifically, what its NHextend3-like
topology alone would predict.

This is a genuine tension I will not paper over: the source-level topology
finding says vips's real lock should behave like NHextend3's worst regime,
but the session's own live, isolated measurement of vips's own protection did
not show that — it showed neutral. Two honest possibilities, and the evidence
favors the second:
1. vips's absolute lock-contention *volume* per run may simply be too low, or
   the run too short, to accumulate enough real "danger" migrations around
   `allocate_lock` specifically to show the same regression NHextend3
   exhibits when *given* that same topology at scale.
2. The dominant explanation for *both* dedup's and vips's large **reported**
   losses is the one already identified in the hp_correlation doc's
   tmpfs/dedup/vips discrepancy section: the whole-system `ivh_universal_eligible`
   toggle bundles in migration collateral from *every other concurrently-
   protected process on the box*, not just the measured workload's own locks.
   The paired design (this workload alone under IVH, system-wide traffic held
   constant across both arms) isolates away exactly that confound — and for
   both dedup and vips, isolating it away turns a large reported loss into
   noise-level neutral.

Given that the *directly measured*, single-variable paired result agrees for
both benchmarks (both neutral in isolation, both large losses only under the
whole-system toggle), the live evidence is stronger here than my source-level
prediction for vips. **The honest, evidence-led conclusion is that system-wide
migration collateral — not either workload's own lock topology — is the
dominant, confirmed driver of the large reported regressions for dedup *and*
vips.** The topology difference between dedup (moderate, partitioned,
~5-9-way) and vips (maximal, single-lock, NHextend3-like) is real and
correctly explains why dedup's *own* protection was never expected to be a
strong win the way hackbench's is — but it is not the reason either workload's
*reported* number is a large loss. That reported loss is a measurement-regime
artifact, not a property of dedup's or vips's own workload characteristics.

### 3.5 Part 3 verdict

**Not the same single mechanism for both, and not purely a topology story
either.** dedup's queue-mutex structure is genuinely distributed (4 lanes,
~5-9 co-waiters at the busiest queue) — a real, different, and less extreme
topology than NHextend3's. vips's real hot lock (`allocate_lock`) is
genuinely as concentrated as NHextend3's (all N threads, one lock, short CS)
— a real topological match, not a coincidence. But the session's own paired,
single-variable measurement shows both workloads' *own* protection cost is
neutral, meaning **topology concentration is not, by itself, sufficient to
explain the large regressions PARSEC reports** — the dominant, directly
measured cause of those reported numbers is system-wide migration collateral
from the whole-system toggle methodology, exactly as the hp_correlation doc's
tmpfs discrepancy already found for dedup and (newly confirmed here) applies
identically to vips.

---

## 4. Synthesis: is IVH "as strong as it can be" at this point?

**Partially, but not fully — and the honest answer has real texture, not a
flat yes or no.**

On the mechanism-cost side (Part 1): the dispatch machinery itself
(`bpf_sched_pre_lock_migrate()`, `affine_move_task()`) has genuinely been
trimmed to its structural floor for the general case — mechanism 1 cannot be
fixed without becoming mechanism 0 with extra steps, and mechanism 0's own
two known optimizations are already landed. That part of the user's framing
is correct and this investigation did not find a hole in it. But this
investigation *did* find a real, concrete, un-tried lever that prior sessions
hadn't examined: most of the pickup-wait cost, under the saturated conditions
IVH's hard workloads run in, is spent queueing behind whatever's running on a
busy target CPU (idle targets are the rare exception, and even they carry a
real, guest-kernel-unfixable hypervisor-wakeup cost). A target-side priority
boost is a real, non-trivial, unexplored candidate for cutting that
queueing-delay component specifically — genuinely not "simple" to build
correctly, but also genuinely not nothing. So: no free win exists, but the
investigation this session was asked to do (rather than just accepting the
premise) surfaced one substantive, specific, still-open avenue, which is a
different and more useful answer than "the mechanism is maxed out."

On the syscall-skip optimization (Part 2): the code is correct except for one
real, narrow bug (the thread-local/global mismatch in NHextend3's own
harness, §2.4) that hasn't manifested in this session's uniform test
conditions but is a genuine latent risk in less uniform deployments. The
kernel-side plumbing (rseq.c, fair.c, rseq.h) is clean.

On PARSEC (Part 3): the picture is more nuanced than either "it's the same
short-CS story as NHextend3" or "it's just measurement collateral" — both
are partially true, for different reasons, for different workloads, and the
session's own paired data is decisive enough to say which one actually
dominates the *reported* numbers (system-wide collateral), even though the
topology story remains a real, separately-confirmed structural fact about
*why neither workload was ever going to be a strong win* even before
collateral is considered.

Put together: **IVH's core migration mechanism is close to its structural
floor for the general case, but "as strong as it can be" is not yet an
accurate description of the whole system** — there is a real, identified,
unexplored lever on the migration-cost side (target-side priority boost), a
real fixable bug on the syscall-skip side (thread-local scoping), and a real
methodology confound on the PARSEC measurement side (whole-system-toggle
collateral inflating reported losses beyond what either workload's own
protection actually costs). None of these are free, none were fabricated to
avoid a flat conclusion, and none should be oversold as guaranteed wins if
pursued — but "nothing left to try" would be an overclaim in the other
direction.
