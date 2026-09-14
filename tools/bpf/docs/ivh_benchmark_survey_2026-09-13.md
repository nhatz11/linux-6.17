# IVH benchmark survey: per-benchmark candidate evaluation, 2026-09-13

Kernel `6.17.0-G-LOCK-25-tier1confirm+`, branch `kernel-43-clean`. Pure
research/planning document — **no benchmarks were run and no code was modified
to produce it**. Companion to `ivh_benchmark_search_2026-07-20.md` (which
contains the only real measured numbers we have for most of the non-NHextend
workloads) and `ivh_nhextend3_migration_validation_2026-09-11.md` (the
migration CS-length valley), `ivh_nhextend_adaptive_futex_lock_2026-09-12.md`
and `ivh_afl_low_threadcount_fix_2026-09-13.md` (the userspace adaptive lock).

## 0. Research-environment caveat, stated up front

This session had **no network access** (WebSearch and WebFetch both denied) and
**no filesystem access outside `/root/linux-6.17`** (Bash and Read denied for
`/root/kernels/...`, `/root/parsec*`, etc.). Everything below is grounded in:

1. **Measured numbers from this project's own docs** — cited with the doc name.
   These are the only numbers in this document that should be treated as facts.
2. **Source read directly out of this tree** — `ivh_adaptive_futex_lock.h`,
   `NHextend-full.c`, `kernel/locking/locktorture.c`, `tools/perf/bench/`.
   Also facts; file/line references are real.
3. **Recalled knowledge of the public benchmark suites' source** — PARSEC,
   SPLASH-2, will-it-scale, fxmark, stress-ng, MOSBench, RocksDB, PostgreSQL,
   memcached. **These are NOT verified against a checkout.** Every claim of the
   form "benchmark X locks in file Y at function Z" in the third category is
   flagged inline as *unverified*. Treat them as a starting hypothesis to
   confirm with `grep`, not as established fact. Where I am unsure of an exact
   test name or option spelling I say so rather than inventing one.

Nothing here should be promoted into a plan of record until the specific
file/function claims have been checked against the actual source on disk.

## 1. The decision model this survey applies

From `ivh_state_of_the_art_2026-07-20.md`, `ivh_migration_cost_and_parsec_analysis_2026-07-22.md`
and the 2026-09-11/12/13 validation docs, four independent axes decide whether a
workload can show IVH benefit. All four have to line up; any one of them being
wrong is enough to produce a flat or negative result.

**(a) Is there real, sustained, multi-threaded contention on a *shared* lock?**
Embarrassingly-parallel workloads have nothing to protect. The clearest
in-project demonstration is ebizzy: malloc mode -2% (pure userspace memcpy, no
shared lock) vs. mmap mode **+136%** (every chunk contends `mmap_lock`) — same
binary, same thread count, opposite sign, the difference being entirely whether
the hot path holds a contended lock.

**(b) Critical-section length, and it is NOT monotonic.** The confirmed
migration cost valley (`ivh_nhextend3_migration_validation_2026-09-11.md` §3–4):

| ~CS length | migration delta |
|---|---|
| ~13µs | **+20.4% / +21.2%** (confirmed 3 ways, t≈10.2) |
| ~22µs | **-14.6%** (an apparent +26.6% that *reversed* under an 8-round confirm) |
| ~50µs | -4.0% (screened only) |
| ~102µs | **-19.0%** (screened only — the worst point) |
| ~234µs | **-11.1%** (screened only) |
| ~400µs | ~0% |
| ~800µs | +8.0% |
| ~1.6ms | **+59.6% / +67.5%** |

Do not reason "longer CS ⇒ more LHP exposure ⇒ more benefit." In the ~50–234µs
band this project *measured the opposite*, because migration's fixed cost
(~250–560µs/event) is not amortized there, and because a mid-CS migration
directly inflates the wall-clock CS it interrupts (measured: 24.8µs → 33.5µs,
+35%, at loop_spin=10,000). Note the valley region itself was only ever measured
at 3-round/10s rigor and is explicitly flagged as provisional.

**(c) Lock topology / concentration.** NHextend's all-N-threads-one-lock shape
is the maximal case. hackbench's ~2-waiters-per-socketpair is the minimal case.
dedup sits in between (4 independent lanes × 4 queues, ~5–9 co-waiters at the
busiest queue — derived from source in
`ivh_migration_cost_and_parsec_analysis_2026-07-22.md` §3.2). Concentration
raises the payoff of protecting the holder *and* raises the collateral cost of
migrating him mid-CS; which wins depends on (b).

**(d) Which mechanism is even applicable.** This matters more than it sounds:

- **Kernel migration** helps when the stall is a *host* preemption of a vCPU
  that is holding or about to hold a lock, and when the CS is outside the
  valley. It is transparent (via `_raw_spin_lock()`'s `ivh_pre_lock()` and the
  scheduler tick) so it applies to *every* workload with zero source changes —
  including the ones where it is a net loss. **But the two halves do not cover
  the same locks**: `ivh_pre_lock()` is hooked into the four `_raw_spin_lock*`
  functions *only*, so mutex-, rwsem- and rtmutex-bottlenecked workloads get the
  tick path alone. This is verified against the tree and has real consequences
  for attributing existing results — see II.A.
- **Kernel adaptive spinning** (tier-1/tier-2, `qspinlock_paravirt.h`) applies
  automatically to any workload that contends kernel qspinlocks. Real but
  marginal on its own (~1%, uncertain sign).
- **The userspace adaptive lock (AFL)** applies *only* where we can edit the
  benchmark's own lock, and it needs genuine thread-level contention.
  Validated at 16 threads (= vCPU count): **+27.8%** in the original session
  and **+32.7%** after the low-thread-count fix (3 interleaved rounds × 20s,
  3/3 consistent, `ivh_afl_low_threadcount_fix_2026-09-13.md`), with
  **+0.1% to +0.3% (neutral) at 8/4/2/1 threads** after that fix — versus
  -18% to -24% at those counts before it.

  **The most important property for this survey**: the AFL's own independent
  contribution of **~18–20% was confirmed at two CS lengths two orders of
  magnitude apart** — loop_spin=5,000 (~13µs) and loop_spin=600,000 (~1.6ms)
  (`ivh_nhextend_adaptive_futex_lock_2026-09-12.md` §§134–175). **The AFL does
  not exhibit migration's valley.** It is flat across the CS-length range where
  migration swings from +67% to -19%.

  That is the central strategic fact of this document. It means the AFL is the
  right tool precisely where migration is worst, and it is *why* several PARSEC
  "migration regressions" (vips above all) are listed below as **good**
  adaptive-lock candidates rather than as write-offs. Caveat worth keeping
  honest: "no valley" is inferred from two well-separated points, not from a
  sweep — the 50–234µs region has **never been measured for the AFL at all**.
  Filling that gap is cheap (sysbench `mutex --mutex-loops`, §I.4.1) and would
  either confirm the strategy or overturn a large part of this survey.

**(e) The one confirmed-bad shape.** Pipeline / producer-consumer with bounded
queues and frequent short handoffs: PARSEC dedup (+16–33% slower) and vips
(+33–39% slower) under the whole-system migration toggle. Every candidate with
that shape is flagged below. **Important honest qualifier**, from
`ivh_migration_cost_and_parsec_analysis_2026-07-22.md` §3.4: those large losses
were measured with a *whole-system* `ivh_universal_eligible` toggle, which
bundles in migration collateral from every other protected process on the box.
The paired, single-workload-isolated measurement in
`ivh_hp_correlation_analysis_2026-07-20.md` showed both dedup (+6.5%, noisy) and
vips (+1.7%) as roughly **cost-neutral for their own protection**. So the
pipeline shape is a confirmed *reported* loser and a plausible mechanistic
loser, but the dominant measured driver of the headline regression numbers is
methodology, not the workload's own topology. Any re-test must use the paired
design.

## 2. The three integration primitives, and their verified constraints

Read out of `NHextend-full.c` (`thread_func`'s lock path, lines ~550–745) and
`ivh_adaptive_futex_lock.h` in this tree. This is the canonical template to
graft into any userspace candidate:

```c
ivh_cs_enter_checked();      /* 1. pre-lock migration trigger, syscall-free
                              *    fast path via the rseq danger bit        */
ivh_afl_lock(&lock);         /* 2. adaptive spin -> heartbeat check -> futex */
extend();                    /* 3. arm rseq timeslice extension AFTER the
                              *    acquire, never before the wait           */
    /* ... critical section ...
     * if the CS can exceed ~50µs, it must also call
     * ivh_afl_publish_heartbeat(&lock) periodically from inside      */
unextend();                  /* disarm; yields if a deferral was granted    */
ivh_afl_unlock(&lock);       /* FUTEX_WAKE gated by the 3-state wake-skip   */
```

Five constraints that are *verified from the source in this tree* and that
directly determine the engineering lift for every candidate below:

1. **`extend()` must be armed after the acquire, not before the wait.**
   `NHextend-full.c` lines 603–637 document this as a measured defect, not a
   style preference: arming before `ivh_afl_lock()` means the kernel spends the
   deferral during a potentially multi-millisecond wait, and the CS itself gets
   nothing. Measured cost of getting it wrong at loop_spin=600000/8 threads:
   CS *active* time identical (936µs vs 929µs) but CS *overall* 1,048µs vs
   948µs — 113µs/CS of off-CPU time on the serialized critical path, growing
   with thread count. This is the single easiest integration mistake to make.

2. **`IVH_AFL_STALE_NS` is 50,000 (50µs)** (`ivh_adaptive_futex_lock.h:115`).
   A holder whose CS runs longer than 50µs *without republishing a heartbeat*
   is declared stale and waiters go to sleep on a perfectly healthy holder. So:
   a candidate whose CS is comfortably under ~50µs is a **drop-in** (one
   heartbeat is published at acquire, `ivh_afl_lock.h:324`); a candidate with a
   longer CS requires **editing the CS body** to call
   `ivh_afl_publish_heartbeat()` (or driving the `IVH_AFL_BEAT_INTERVAL` counter
   from the CS's own loop, the preferred pattern — see `NHextend-full.c:690`),
   or raising `IVH_AFL_STALE_NS`. For an opaque third-party CS body (e.g. a
   database holding its mutex across a memtable insert) this is the hard part
   of the port, not the lock swap.

3. **The futex ops are `FUTEX_WAIT_PRIVATE` / `FUTEX_WAKE_PRIVATE`**
   (`ivh_adaptive_futex_lock.h:584, 625, 635, 657`). **The lock is
   single-process only as written.** Any process-based candidate —
   PostgreSQL, Apache prefork/worker MPM, nginx's `ngx_shmtx` accept mutex,
   MOSBench exim — requires dropping the PRIVATE flag, which changes futex key
   derivation from (mm, addr) to (inode, offset) and makes both the wait and
   the wake measurably more expensive. That cost lands on the exact syscall the
   wake-skip flag exists to avoid, so it is not a cosmetic change: it must be
   re-validated, not assumed. **This is the single biggest structural filter in
   this survey** and it demotes several otherwise-attractive candidates.

4. **`struct ivh_afl_lock` is 192 bytes, three 64-byte-aligned cachelines**
   (`state`, `hb_tsc`, config — deliberately split, see the struct comment).
   Any candidate with a *large array* of locks (PARSEC fluidanimate's per-cell
   mutexes, memcached's `item_locks[]`, InnoDB's buffer-pool block mutexes)
   pays a real memory/cache-footprint cost swapping a 40-byte
   `pthread_mutex_t` for 192 bytes. For fluidanimate, with one mutex per grid
   cell, this alone could change the benchmark's cache behaviour enough to
   invalidate the comparison. Fine-grained-lock-array workloads are therefore
   structurally worse AFL candidates than single-hot-lock workloads, independent
   of their contention level.

5. **`ivh_cs_enter_checked()` is cheap only when the danger bit is clear.**
   `NHextend-full.c:176` skips the syscall entirely when this thread's last
   published rseq danger bit is clear, and the bit is refreshed on every
   return-to-userspace. A workload that rarely returns to userspace (long
   compute CSes with no syscalls) sees a stale bit; the code fails *open*
   (assume danger, make the real syscall — `NHextend-full.c:124–149`), so a
   compute-bound candidate pays full syscall cost on every acquisition. Budget
   for that in any tight-loop candidate.

**Priority scale used below**: **High** = worth building now, plausible ≥10%
effect, acceptable lift. **Medium** = worth building after the Highs, or
valuable as a control/negative result. **Low** = run it if it's free (already
built, no source changes), don't invest. **Unlikely** = the model predicts no
effect or a loss; include only to document *why*, or as a deliberate negative
control.

---

# PART I — USERSPACE CANDIDATES

These have their own locks in their own source, so all three primitives are in
play. They *also* get the kernel migration engine and kernel adaptive spinning
automatically, as everything does.

## I.0 Priority table (userspace)

| # | Benchmark | Suite | Priority | One-line reason |
|---|---|---|---|---|
| 1 | **vips (`allocate_lock`)** | PARSEC | **High** | Single global work-queue lock, all N workers, very short CS — topologically identical to NHextend. Already measured as a migration *loser*; that is precisely the regime the AFL was built for. Best single AFL experiment available. |
| 2 | **sysbench `mutex`** | sysbench | **High** | A purpose-built pthread_mutex contention microbenchmark with tunable lock count/hold time — lets us sweep the CS-length valley on a *third-party* code base instead of our own. Trivial lift. |
| 3 | **SPLASH-2 `radiosity`** | SPLASH-2/3 | **High** | Distributed task queues with per-queue locks + work stealing; the most lock-intensive SPLASH-2 application. |
| 4 | **SPLASH-2 `raytrace`** | SPLASH-2/3 | **High** | Global work-pool lock plus a notoriously contended global counter lock. Concentrated + short CS. |
| 5 | **SPLASH-2 macro layer (all 14)** | SPLASH-2/3 | **High (as a vehicle)** | Every SPLASH benchmark's `LOCK()/UNLOCK()` goes through *one* m4 macro file. One edit ⇒ AFL in 14 benchmarks. Highest leverage-per-hour in this survey. |
| 6 | **memcached** | MOSBench / standalone | **High** | `item_lock()`/`item_unlock()` in `thread.c` is a single function wrapping a bucket-indexed mutex array — a genuinely clean one-function AFL swap. Threaded, so PRIVATE futex is fine. |
| 7 | **SPLASH-2 `barnes`** | SPLASH-2/3 | **Medium-High** | Per-octree-cell locks during tree build; real fine-grained lock traffic, short CSes. |
| 8 | **SPLASH-2 `volrend`** | SPLASH-2/3 | **Medium-High** | Task-queue locks, same family as raytrace. |
| 9 | **LevelDB `db_bench`** | standalone | **Medium-High** | One global `DBImpl::mutex_` — maximal concentration, and far simpler to patch than RocksDB. |
| 10 | **RocksDB `db_bench`** | standalone | **Medium** | `DBImpl::mutex_` + `WriteThread::AwaitState`, which *already* implements spin-then-futex — a direct head-to-head against our staleness heuristic. But the write group is a leader/follower handoff = the bad shape. |
| 11 | **PARSEC `streamcluster`** | PARSEC | **Medium** | User-flagged as lock-intensive. Genuinely sync-saturated, but the sync is **barriers**, not mutexes — see the entry for why that changes which primitive applies. |
| 12 | **PARSEC `raytrace`** | PARSEC | **Medium** | Task-queue/work-stealing with per-queue locks. |
| 13 | **PARSEC `bodytrack`** | PARSEC | **Medium** | `WorkerGroup` thread pool, condvar + mutex dispatch per frame. |
| 14 | **SPLASH-2 `water_nsquared`** | SPLASH-2/3 | **Medium** | Per-molecule lock array for force accumulation — real lock traffic, but array-of-locks (constraint 4). |
| 15 | **SPLASH-2 `cholesky`** | SPLASH-2/3 | **Medium** | Task queue + per-supernode locks, irregular. |
| 16 | **SPLASH-2 `fmm`** | SPLASH-2/3 | **Medium** | Per-box locks + barriers. |
| 17 | **PARSEC `facesim`** | PARSEC | **Medium** | `TaskQ` with locks, plus heavy barriers. |
| 18 | **PostgreSQL / pgbench** | DB | **Medium** (blocked on constraint 3) | `LWLockAcquire()` is a single, ideal integration point and LWLocks are textbook LHP victims — but PG is process-per-backend with locks in shared memory, so the PRIVATE-futex limitation must be lifted first. |
| 19 | **MySQL / InnoDB** | DB | **Medium** | Real, famous contention (`lock_sys`, `trx_sys`, buffer-pool mutexes) and InnoDB already has its own spin-then-`os_event` adaptive lock to compare against. Very large lift. |
| 20 | **PARSEC `fluidanimate`** | PARSEC | **Low-Medium** | Measured ~neutral (1 round, inconclusive). Huge per-cell mutex array ⇒ constraint-4 footprint problem; contention per lock is low by construction. |
| 21 | **Phoenix++** | MapReduce | **Low-Medium** | Mostly per-thread partitions + barriers; the shared task queue is the only lock. Per-workload breakdown in its entry. |
| 22 | **PARSEC `x264`** | PARSEC | **Low** | Frame-level pipeline with per-frame mutex/condvar dependency signalling — the confirmed-bad shape. |
| 23 | **PARSEC `swaptions`** | PARSEC | **Low** (as a lock target) / **keep as control** | Already a confirmed **+8.4%** win — but it's embarrassingly parallel with no shared lock, so the win is kernel-side rebalancing. Nothing for the AFL to do. |
| 24 | **PARSEC `blackscholes`** | PARSEC | **Low** / control | Measured **-2.8%** (a real win). Barrier-only, no locks. Good zero-lock control. |
| 25 | **PARSEC `canneal`** | PARSEC | **Low** / control | Measured **-2.8%** (a real win). **Lock-free** (atomic CAS on net elements). The cleanest "no lock at all, still wins" control we have. |
| 26 | **PARSEC `freqmine`** | PARSEC | **Low** | OpenMP, barrier/reduction dominated, negligible locking. |
| 27 | **SPLASH-2 `water_spatial`** | SPLASH-2/3 | **Low** | Box decomposition removes most of water_nsquared's lock traffic. |
| 28 | **SPLASH-2 `radix`** | SPLASH-2/3 | **Low** | Barriers + a global histogram/prefix step; little lock traffic. |
| 29 | **SPLASH-2 `fft`** | SPLASH-2/3 | **Low** | Barrier-only (transpose phases). Zero locks. |
| 30 | **SPLASH-2 `lu_cb`** | SPLASH-2/3 | **Low** | Barrier-only. Contiguous-block variant. |
| 31 | **SPLASH-2 `lu_ncb`** | SPLASH-2/3 | **Low** | Barrier-only. Differs from lu_cb only in data layout, not sync — useful as a paired memory-effects control. |
| 32 | **SPLASH-2 `ocean_cp`** | SPLASH-2/3 | **Low** | Barrier-dominated multigrid; essentially no locks. |
| 33 | **SPLASH-2 `ocean_ncp`** | SPLASH-2/3 | **Low** | Same; differs from ocean_cp in partitioning/layout only. |
| 34 | **Metis** | MapReduce | **Low** | Per-core hash tables, minimal sharing by design. |
| 35 | **nginx** | server | **Low** | Event-driven; `ngx_accept_mutex` is off by default on modern builds with `reuseport`. Cross-process (constraint 3). |
| 36 | **Apache httpd** | MOSBench / server | **Low** | Event MPM pushes the work into the kernel; the accept mutex is cross-process. |
| 37 | **PARSEC `dedup`** | PARSEC | **Unlikely** | **Confirmed-bad shape.** Pipeline, 4 lanes × 4 bounded queues, ~5–9 co-waiters. Reported +16–33% slower. |
| 38 | **PARSEC `ferret`** | PARSEC | **Unlikely** | Six-stage pipeline with per-stage bounded queues — structurally dedup. |
| 39 | **Redis / memtier** | DB | **Unlikely** | Measured **-5%**. Single-threaded command execution: there is no multi-threaded lock holder to protect. Model predicts no benefit at any tuning. |
| 40 | **Cassandra** | DB | **Unlikely** | JVM — locks are `ObjectMonitor`/`AbstractQueuedSynchronizer`, not patchable without JVM work, and GC noise swamps a 10–30% signal. |
| 41 | **SPEC CPU 2017** | CPU | **Unlikely** | SPECrate is N independent copies (zero shared state); SPECspeed is OpenMP barrier/reduction. Nothing to protect. Documented here only to close the question. |
| 42 | **pbzip2** | standalone | **Unlikely** | Already tried: neutral/noisy, and it is a producer-consumer queue — dedup's shape. Also has a measurement-floor problem. |

---

## I.1 PARSEC, per benchmark

PARSEC is the suite where this project has the most real data, and the most
misleading suite-level summaries. The four benchmarks with measured numbers
(blackscholes, canneal, dedup, vips) span the entire range of outcomes.

### I.1.1 vips — **High**. The best userspace AFL experiment available.

**Userspace.** Measured: **+33% to +39% SLOWER** under the whole-system
migration toggle; **+1.7% (neutral)** under the paired, single-workload-isolated
design. Both numbers are real; they measure different things (see §1(e)).

Source-grounded topology, from
`ivh_migration_cost_and_parsec_analysis_2026-07-22.md` §3.3 (this one *was*
verified against the real source in that session, so it is category-2, not
category-3): `VipsThreadpool` (`src/libvips/iofuncs/threadpool.c:352`) holds
**exactly one `allocate_lock` (`GMutex *`) per threadpool**, and all `pool->nthr`
workers contend it in `vips_thread_work_unit()` (line 491):

```
g_mutex_lock(pool->allocate_lock);
  vips_thread_allocate(thr);       /* pick next tile — pure bookkeeping */
g_mutex_unlock(pool->allocate_lock);
vips_thread_work(thr);             /* the actual pixel work, OUTSIDE the lock */
```

That is **all-N-threads-one-lock guarding a tiny bookkeeping step** — the exact
NHextend topology, and the exact regime (`high concentration + short CS`) where
migration was identified as worst and where the adaptive futex lock is the
purpose-built answer. The fact that vips is a *migration loser* is the argument
*for* it being an AFL candidate, not against.

**Integration plan.**
- **`ivh_afl_lock.h`**: replace `pool->allocate_lock` (`GMutex *`) with a
  `struct ivh_afl_lock` inside `struct _VipsThreadpool` (`threadpool.c:352`),
  `ivh_afl_init()` in `vips_threadpool_new()`, and swap the single
  `g_mutex_lock`/`g_mutex_unlock` pair at `threadpool.c:491`. There is exactly
  **one** call site for the hot lock, which is as good as this ever gets.
  The CS is bookkeeping-only and near-certainly well under 50µs, so **no
  in-CS heartbeat instrumentation is needed** (constraint 2 satisfied for free).
- **`sys_ivh_cs_enter()`**: `ivh_cs_enter_checked()` immediately before the
  `g_mutex_lock` at line 491. Note vips workers are compute-heavy between
  acquisitions and may not return to userspace often, so constraint 5 applies —
  expect the danger bit to fail open and the syscall to be taken more often than
  in NHextend. Measure `syscall_skipped_count` vs `syscall_made_count` first.
- **`extend()`/`unextend()`**: arm `extend()` *after* `ivh_afl_lock()` returns,
  `unextend()` before `ivh_afl_unlock()`. Per constraint 1 this is the
  easy-to-get-wrong step.

**Lift**: low-to-moderate. vips is a big library but the hot lock is one field
and one call site. The real work is building vips (it has a large dependency
chain — glib, libjpeg, libtiff, ...) and confirming via `perf lock contention`
or a counter that `allocate_lock` really is the hot lock at 16 threads for the
chosen input, since §3.4 explicitly raised the possibility that vips's absolute
lock-contention *volume* is simply too low per run to matter. **Do that
volume check before doing the port** — it is the cheap way to avoid wasting the
port entirely.

**How to run it**: 16 threads (`--vips-concurrency=16` / `-t 16`, matching vCPU
count, which is where the AFL's 16-thread win was validated), native input,
paired design, host corunner active.

### I.1.2 dedup — **Unlikely**. The confirmed-bad shape, documented.

**Userspace.** Measured **+16% to +33% SLOWER** (whole-system toggle);
**+6.5%, noisy/neutral** (paired isolated).

Source-grounded (category 2, verified in the 2026-07-22 session):
`queue_t` (`src/queue.h:24`) has one `pthread_mutex_t` + two condvars per queue
instance. `encode()` (`src/encoder.c:1372`) computes
`nqueues = ceil(nthreads / MAX_THREADS_PER_QUEUE)` with
`MAX_THREADS_PER_QUEUE = 4` (`config.h:10`), so a 16-thread run gets **4
independent lanes**, each with its own refine/deduplicate/compress/reorder
queue — 16 mutexes total. Worst-case co-waiters, by queue:

| queue (per lane) | producers | consumers | max concurrent waiters |
|---|---|---|---|
| `refine_que[qid]` | 1 (global Fragment) | ≤4 FragmentRefine | ~5 |
| `deduplicate_que[qid]` | ≤4 | ≤4 | ~8 |
| `compress_que[qid]` | ≤4 | ≤4 | ~8 |
| `reorder_que[qid]` | ≤4 + ≤4 | 1 (global Reorder) | ~9 |

This is the archetype of the shape §1(e) flags: bounded queues, frequent short
handoffs, moderate (not maximal) concentration spread over independent lanes.
Migration's fixed cost is paid per handoff and amortized over nothing.

**Verdict**: do not invest. The one thing dedup is genuinely useful for is as a
**deliberate negative control** — if a future migration change (e.g. the
Step-0/Step-1 lock-skipping work in
`ivh_lock_skipping_step0_step1_plan_2026-09-14.md`) claims to have fixed the
mid-CS regime, dedup regressing *less* is the cheapest evidence. Run it paired,
not whole-system, or the number measures methodology.

**If ported anyway**: the AFL would go in `queue.c`'s `enqueue()`/`dequeue()`
around `pthread_mutex_lock(&que->mutex)`. But the queues are condvar-based
(`notEmpty`/`notFull`) and the AFL has no condvar equivalent — you would be
replacing only the mutex, leaving the condvar waits (which are where the real
blocking happens) untouched. **That alone probably makes the port pointless**,
and it is a general lesson: *the AFL replaces mutexes, not condition variables,
so any workload whose blocking is condvar-shaped is a poor fit by construction.*
That single observation disqualifies most of the pipeline family below.

### I.1.3 ferret — **Unlikely**. dedup's shape, one stage deeper.

**Userspace.** Not measured by this project. Ferret is a six-stage
content-similarity search pipeline (load → segment → extract → index/vector →
rank → out) with a bounded queue between each stage, using the same
mutex+condvar queue idiom as dedup (*unverified*, but the suite's own
documentation describes both as the "pipeline" model, and the 2026-07-20 doc
already predicted ferret would behave like dedup).

Same disqualifier as dedup: the blocking is condvar-shaped, so the AFL has
nothing to replace. Same recommendation: use as a negative control only, paired
design.

### I.1.4 vips vs. dedup — why they are listed on opposite ends

Worth stating explicitly, because a suite-level "PARSEC pipeline apps regress"
summary would have put them together and it would be wrong. Both were reported
as large regressions. But vips's regression comes from a **single global
short-CS mutex** (an AFL target), and dedup's from **condvar-gated bounded
queues** (not an AFL target). The reported numbers look the same; the
mechanisms and the remediation are completely different. This is the strongest
argument in this document for per-benchmark rather than per-suite evaluation.

### I.1.5 blackscholes — **Low** as a target, keep as a zero-lock control.

**Userspace.** Measured **-2.8% runtime (a real win)** at 16 threads, native.
Blackscholes is the canonical embarrassingly-parallel PARSEC kernel: threads
partition the option array, compute independently, and meet at a barrier at the
end of each run iteration. There is essentially **no lock at all**.

So why did it win 2.8%? Not lock protection — almost certainly kernel-side
rebalancing (moving a thread off a vCPU that is being stolen from, which helps
any CPU-bound thread regardless of locks). That makes blackscholes valuable
precisely as a **control**: it isolates the "migration helps compute throughput
under host contention" component from the "migration protects a lock holder"
component. Keep it in every sweep, expect ~-3%, and treat a *large* movement
here as a sign that something systemic changed.

**Integration plan**: none. There is no lock to swap. `extend()` has no CS to
protect. This is a kernel-mechanism-only benchmark despite being userspace.

### I.1.6 canneal — **Low** as a target, keep as the lock-free control.

**Userspace.** Measured **-2.8% (a real win)**, same as blackscholes.

Canneal is the interesting one because it is *deliberately lock-free*: the
simulated-annealing netlist swap uses atomic compare-and-swap on the element
pointers rather than a mutex (this is canneal's stated design point in PARSEC's
own description — *unverified against source, but it is the benchmark's headline
characteristic*). A lock-free workload winning 2.8% is a second, independent
confirmation of the same "rebalancing, not lock protection" mechanism as
blackscholes, from a completely different memory-access pattern
(cache-thrashing, pointer-chasing, huge working set vs. blackscholes's tiny
streaming one).

**Integration plan**: none, and that's the point — you cannot put the AFL in a
lock-free algorithm. Use it as the pair-mate to blackscholes.

### I.1.7 streamcluster — **Medium**. Lock-intensive, but the sync is barriers.

**Userspace.** Not measured by this project. The user specifically flagged this
as lock-intensive, and that is correct in spirit: streamcluster is famous for
being **synchronization-saturated** — `pgain()` calls a barrier an enormous
number of times per iteration, and at high thread counts streamcluster spends a
large fraction of wall time in `pthread_barrier_wait` rather than in the
clustering math. It is the PARSEC benchmark that scales worst for
synchronization reasons.

But the distinction matters enormously for IVH: **a barrier is not a mutex.**

- **For the migration engine, barriers are the *maximal* LHP target.** One
  preempted participant stalls all N, for the full preemption duration, with no
  critical section to shorten and no way to hand off. This is arguably a better
  migration story than any mutex workload: if you can migrate the laggard off a
  stolen vCPU before the barrier, you save (N-1) × stall. Worth testing on
  migration alone.
- **For the adaptive futex lock, there is nothing to swap.** `ivh_afl_lock` is
  a mutual-exclusion lock; a barrier is an N-way rendezvous. Porting the
  *idea* (spin, check whether the outstanding participants' heartbeats are
  stale, futex-wait if so) to a barrier would be **a new primitive**, not an
  integration — call it `ivh_afl_barrier`, and note that glibc's
  `pthread_barrier_wait` already does spin-then-futex, so the novel part is only
  the staleness heuristic.
- **For `extend()`**, the natural arming point is "the last thread to arrive"
  — which you don't know in advance. Arming `extend()` on arrival at the barrier
  is the mistake constraint 1 warns about, at scale.

**Verdict: Medium, and split the test.** Run streamcluster under the migration
engine alone first (zero source changes, and it is a genuinely promising
migration target). Only build `ivh_afl_barrier` if that shows a signal. Note
that PARSEC's streamcluster does also use a mutex + condvar to implement its
barrier when `ENABLE_PTHREADS` is set without native barriers (*unverified* —
check whether the build used `pthread_barrier_t` or the hand-rolled
mutex/condvar barrier, because if it's the latter there *is* a mutex to swap,
and streamcluster jumps to High).

### I.1.8 bodytrack — **Medium**.

**Userspace.** Not measured. Bodytrack uses a `WorkerGroup` / `ThreadGroup`
abstraction (`src/TrackingBenchmark/threads/`) with a condition-variable
dispatch: the main thread sets up a frame's particle-filter work, signals the
workers, workers process, and rendezvous before the next frame (*unverified* —
but bodytrack's threading layer is a documented reusable `threads/` library with
`Mutex`, `Condition`, `Barrier` wrappers).

Two things make it interesting and one makes it risky. Interesting: (a) the
threading layer is a **C++ abstraction with `Mutex`/`Condition` classes**, so
swapping the implementation of `Mutex::Lock()` is a *one-class* change that
propagates everywhere — nearly as good as the SPLASH macro layer. (b) The
per-frame structure gives naturally long, regular CSes. Risky: the dispatch is
condvar-based (same disqualifier as dedup) and the frame rendezvous is a
barrier, so what's left for a pure mutex swap may be thin.

**Integration plan**: subclass/replace `Mutex` in
`src/TrackingBenchmark/threads/Mutex.cpp` with `ivh_afl_lock`; add
`ivh_cs_enter_checked()` inside `Mutex::Lock()` and `extend()`/`unextend()`
around the acquired region — note that a generic `Mutex::Lock()`/`Unlock()`
wrapper is the *ideal* place for all three primitives because it's one function
pair covering every call site. **Lift: low, if the abstraction is as clean as
described.** Verify first.

### I.1.9 facesim — **Medium**.

**Userspace.** Not measured. Facesim (PhysBAM face simulation) uses a `TaskQ`
work-queue library with per-queue locks plus frequent barriers between
simulation phases (*unverified*). The CSes in a task queue are short
(pop a task descriptor) which puts it near the good end of the valley; the
barriers between phases are a migration target as in streamcluster.

**Integration plan**: the `TaskQ` library (`pkgs/libs/tasktracker` or facesim's
bundled `TaskQ/`) is a single abstraction — same one-place-to-patch advantage as
bodytrack. Swap the queue mutex; heartbeat not needed (short CS).
**Lift: moderate** — facesim is a large C++ build with a heavy input.

### I.1.10 fluidanimate — **Low-Medium**. Measured ~neutral; footprint problem.

**Userspace.** Measured: `fluidanimate 16 100 in_500K.fluid`, r1 off 31.55s / on
31.22s (~-1%) — **one round only, explicitly inconclusive** in
`ivh_benchmark_search_2026-07-20.md`. The prior session expected a dedup-like
regression and got roughly neutral.

Fluidanimate uses **one `pthread_mutex_t` per grid cell** — an array of
thousands of mutexes, each guarding the particle-force accumulation for that
cell. This gives it a distinctive profile: enormous *aggregate* lock traffic but
very *low per-lock* contention (neighbouring cells are mostly touched by one
thread each; contention only happens on partition boundaries). That is close to
the hackbench end of the concentration axis, which predicts a small effect —
consistent with the neutral measurement.

**Constraint 4 is disqualifying here.** Swapping thousands of 40-byte
`pthread_mutex_t` for 192-byte `ivh_afl_lock` would multiply the lock array's
footprint ~5×, and fluidanimate's performance is already sensitive to the
cell-grid's cache behaviour. The measurement would no longer be comparing
lock algorithms; it would be comparing cache footprints. **If you want to test
fluidanimate, first build a footprint-neutral AFL variant** (single cacheline:
`state` and `hb_tsc` in one line, config hoisted to a global — which the struct
comment explicitly warns degrades the heartbeat by invalidating spinners' lines,
so this is a real tradeoff, not a free win).

**Verdict**: finish the measurement (it's ~31s/run, so a proper 8-round paired
run is ~10 minutes) before investing in a port. Migration-only, no source
changes. Only port if migration shows a real signal.

### I.1.11 raytrace (PARSEC) — **Medium**.

**Userspace.** Not measured. PARSEC's raytrace (Intel RTView) uses a task-based
renderer with a work queue and work stealing (*unverified*). Short CSes (grab a
tile), moderate-to-high concentration. That is the favourable corner of the
valley (~13µs-ish CSes) and a reasonable AFL shape.

Distinguish it from **SPLASH-2 raytrace** (I.2.6), which is a different program
with a different (and better-documented) lock structure. Both are in this
survey; don't conflate them.

**Integration plan**: find the work-queue pop (likely a `Mutex`/`AtomicCounter`
in the RTView task scheduler), same three-primitive template. **Lift: moderate**
— C++, and the task scheduler may already be atomic-counter-based rather than
mutex-based, in which case there is nothing to swap (check before porting).

### I.1.12 swaptions — **Low** as a lock target; **keep** as a confirmed win.

**Userspace.** Measured **+8.4%** at the best config
(`-ns 256 -sm 500000 -nt 16`, 2 rounds, both rounds -8.4%/-8.5% runtime), and
**neutral at `-ns 64`** — the win only appears once there is enough per-thread
work. This is one of only five clean ≥5% wins this project has ever recorded.

But the mechanism is *not* lock protection: swaptions is Monte-Carlo HJM
swaption pricing where threads pull work items from a shared queue **once** and
then compute independently with per-thread `malloc`. Minimal shared locking.
Like blackscholes and canneal, the win is kernel-side rebalancing.

**Integration plan**: none worth doing — the one-time work-item pull isn't a hot
lock. **Keep swaptions in the standard regression set** as the "confirmed
positive, low-collateral, compute-bound" anchor: if a future IVH change makes
swaptions regress, it has broken the rebalancing path, and that's worth knowing
independently of any lock work.

### I.1.13 freqmine — **Low**.

**Userspace.** Not measured. Freqmine is OpenMP (not pthreads), FP-growth
frequent-itemset mining, dominated by parallel-for regions with reductions and
implicit barriers, with the FP-tree construction largely partitioned
(*unverified*). Little explicit locking; what sync exists is the OpenMP runtime's
(GOMP) barriers and `#pragma omp critical` regions, which in libgomp are
implemented over futexes.

Two consequences: (a) as an AFL target it's poor — you'd be patching libgomp,
not freqmine; (b) as a *migration* target it's a barrier workload like
streamcluster, so it's a free ride-along in any migration sweep. Note also that
freqmine's thread count is set by `OMP_NUM_THREADS`, not a CLI flag, which is
an easy configuration mistake in a 16-thread sweep.

### I.1.14 x264 — **Low**.

**Userspace.** Not measured. x264's PARSEC configuration uses frame-level
parallelism: each frame-encoding thread waits on the reference frames it
depends on, signalled via per-frame mutex+condvar (`x264_pthread_cond_wait` on
`frame->cv`). That is a **dependency-signalling producer-consumer** structure —
condvar-shaped blocking, the dedup disqualifier again.

Additionally, frame encode times are long and highly variable, so the effective
"critical section" (the dependency wait) is milliseconds and irregular, which is
the wrong end of the valley for the wrong reason (it's a wait, not a held lock).

**Verdict: Low.** Free to include in a migration-only sweep; don't port.

---

## I.2 SPLASH-2 / SPLASH-2x / SPLASH-3, per benchmark

**The single highest-leverage finding in this survey**: SPLASH-2 codes do not
call `pthread_mutex_lock` directly. They use the **m4 PARMACS macro layer** —
`LOCK(x)` / `UNLOCK(x)` / `BARRIER(b,n)` / `ALOCK(a,i)` — expanded at build time
from a single macro definition file (`c.m4.null.POSIX`, or the equivalent in
whichever distribution is used; in the Stanford/SPLASH-2 release this is
`codes/null_macros/c.m4.null.POSIX`, and PARSEC's SPLASH-2x repackaging carries
its own copy). *This is category-3 recalled knowledge and MUST be verified
against the checkout*, but if it holds:

> **One edit to one m4 file puts the adaptive futex lock — plus
> `ivh_cs_enter_checked()` and `extend()`/`unextend()` — into all fourteen
> SPLASH benchmarks simultaneously, with zero per-benchmark source changes.**

That makes SPLASH the best value-per-engineering-hour in this entire survey, and
it should probably be done *before* any individual PARSEC port. The macro layer
also naturally satisfies constraint 1: `LOCK()` expands to
`{ ivh_cs_enter_checked(); ivh_afl_lock(&x); extend(); }` and `UNLOCK()` to
`{ unextend(); ivh_afl_unlock(&x); }` — the correct ordering baked in once,
everywhere, instead of hand-placed at N call sites.

Caveats to resolve first: (a) `ALOCK(array, index)` (lock arrays) hits
constraint 4 — barnes and water_nsquared use these heavily. (b) SPLASH-2 codes
allocate their shared state in a shared-memory arena via `G_MALLOC`; if the
build uses `fork()`+`mmap(MAP_SHARED)` rather than pthreads, constraint 3
(PRIVATE futex) bites. The POSIX-threads macro file uses pthreads, so check
which macro file the build selects. (c) Prefer **SPLASH-3** (Sakalis et al.) over
stock SPLASH-2 if available — it fixes data races and sync bugs in the originals
that would otherwise be blamed on IVH.

### Applications

**I.2.1 barnes — Medium-High.** Barnes-Hut N-body. The tree-build phase locks
individual octree cells as bodies are inserted (`ALOCK(cell->lock)`-style), plus
barriers between phases. Real fine-grained lock traffic with genuinely short
CSes (insert a pointer) — the good end of the valley. Concentration is
moderate-to-low per cell but rises sharply near the root of the tree, where a
few cells are touched by everyone: that root contention is the interesting part.
**Constraint 4 applies** (lock array per cell). Best SPLASH *application*
candidate after radiosity/raytrace.

**I.2.2 fmm — Medium.** Fast Multipole Method. Per-box locks during the
upward/downward passes plus barriers between passes (*unverified*). Similar
shape to barnes but with more barrier structure and less lock traffic. Include
in the macro-layer sweep; don't port individually.

**I.2.3 ocean_cp — Low.** Ocean simulation, **c**ontiguous **p**artitions.
Red-black SOR / multigrid solver: the parallelism is a regular grid
decomposition with **barriers between sweeps and essentially no locks**. Nothing
for the AFL. As a migration target it's a barrier workload (see streamcluster),
so it rides along free.

**I.2.4 ocean_ncp — Low.** Identical algorithm, **n**on-**c**ontiguous
partitions. The *only* difference from ocean_cp is data layout/false-sharing
behaviour, not synchronization. That makes the cp/ncp pair a genuinely useful
**control pair**: any IVH delta that differs between them is a memory/cache
effect, not a locking effect. Worth running both purely for that.

**I.2.5 radiosity — High (best SPLASH candidate).** Hierarchical radiosity with
**distributed task queues and work stealing**: each processor has its own task
queue with its own lock, and steals from others' queues when empty. That gives
(a) high lock *traffic*, (b) CSes that are short (push/pop a task descriptor),
(c) contention that *rises* exactly when the system is imbalanced — which is
precisely what host preemption causes. A vCPU that gets stolen from falls behind,
its queue drains, and everyone else starts hammering its lock to steal. This is
a mechanistically excellent LHP story and the most lock-intensive SPLASH-2 code.
Also uses a global shared-memory allocator with its own lock. **Port target #1
in SPLASH** (or, better, get it for free from the macro layer).

**I.2.6 raytrace (SPLASH-2) — High.** Distinct from PARSEC raytrace. SPLASH-2's
raytrace has a **global work-pool lock** plus a famously contended **global
counter lock** (the ray/ID counter), which every thread hits on every ray. That
counter lock is the closest thing in the standard suites to NHextend's
all-N-threads-one-tiny-CS shape, and it is the canonical "this benchmark doesn't
scale because of one lock" example in the SPLASH literature. Short CS, maximal
concentration ⇒ exactly the AFL's validated regime. **Port target #2.**

Practical note: SPLASH-2 raytrace's global counter is small enough that on a
modern machine the lock may be the *entire* bottleneck, which is good for signal
but means absolute performance is poor. Check that the run length is ≥15s at
16 threads (`car` / `balls4` inputs; SPLASH-2x provides larger ones).

**I.2.7 volrend — Medium-High.** Volume rendering by ray casting. Task-queue
based with per-queue locks and stealing, same family as raytrace/radiosity
(*unverified*, but volrend is consistently described alongside them as
task-queue-based). Short CSes, adaptive imbalance. Include; expect a smaller
effect than radiosity because the per-task work is larger.

**I.2.8 water_nsquared — Medium.** O(n²) molecular dynamics. Uses a **per-molecule
lock array** (`gl->MolLock[]`) to serialize force accumulation into shared
molecule records, plus barriers between the force/motion phases. Real lock
traffic, short CSes (accumulate a few doubles). Contention per lock is modest
(the lock array is sized to spread it) but total traffic is high.
**Constraint 4 applies.** A good mid-range data point: not maximal
concentration, not hackbench-minimal.

**I.2.9 water_spatial — Low.** Same physics, spatial-decomposition algorithm:
molecules are binned into boxes owned by one processor, which **eliminates most
of water_nsquared's lock traffic** and replaces it with boundary exchange +
barriers. The nsquared/spatial pair is therefore a second useful **control
pair** — same physics, same inputs, deliberately different lock intensity. If
IVH helps nsquared and not spatial, that is a clean attribution to locking.

### Kernels

**I.2.10 cholesky — Medium.** Sparse Cholesky factorization: a **global task
queue** plus per-supernode locks, with highly irregular task sizes. The
irregularity is interesting (it spans the valley within a single run, which
makes it a poor place to *isolate* a CS-length effect but a realistic place to
test whether IVH's net effect is positive on a mixed workload). Also has a
shared-memory allocator lock. Include in the macro sweep.

**I.2.11 fft — Low.** Six-step FFT. Pure barrier synchronization around the
transpose phases; **zero locks**. Nothing for the AFL. Useful only as a
barrier/migration data point, where it is essentially a smaller, cleaner
ocean_cp.

**I.2.12 lu_cb — Low.** Blocked LU factorization, **c**ontiguous **b**locks.
Barrier-only (*unverified*, but LU's SPLASH-2 implementation is documented as
barrier-synchronized with no locks in the inner loops). Nothing for the AFL.

**I.2.13 lu_ncb — Low.** Same, **n**on-**c**ontiguous blocks. Third **control
pair** (with lu_cb): identical sync, different layout.

**I.2.14 radix — Low.** Radix sort. Barriers plus a global prefix-sum/histogram
step; some implementations use a lock around the global histogram merge
(*unverified*). Even if a lock exists it is hit O(passes) times, not O(keys), so
the traffic is negligible. Low.

---

## I.3 Databases, key-value stores and servers

For each of these the question is "what is the actual internal locking
architecture," because the answers differ far more than the "it's a server, it
should contend" intuition suggests.

### I.3.1 memcached — **High**.

**Userspace (multi-threaded).** Not measured as a target by this project (the
2026-07-20 doc measured *redis* via memtier, not memcached).

Architecture: memcached is genuinely multi-threaded with a per-thread libevent
loop, and its shared hash table is protected by a **bucket-indexed array of
mutexes** — `item_locks[]`, with the bucket derived from the item's hash — plus
separate `lru_locks[]`, a `slabs_lock`, and a `cache_lock` in older versions
(*unverified against a checkout, but this is memcached's well-documented
design since 1.4.x*). Crucially, **all item-lock acquisition goes through two
functions in `thread.c`: `item_lock(uint32_t hv)` and `item_unlock(uint32_t hv)`**
(plus `item_trylock`/`item_lock_type` variants for the hash expansion path).

That is the cleanest integration point in this entire survey after vips and the
SPLASH macro layer: **one function pair, covering every call site.** And since
memcached is threads-not-processes, constraint 3 (PRIVATE futex) is satisfied
for free.

**Integration plan.**
- **`ivh_afl_lock.h`**: change `item_locks` from `pthread_mutex_t *` to
  `struct ivh_afl_lock *` in `thread.c`; `ivh_afl_init()` each entry in
  `thread_init()`; swap the body of `item_lock()`/`item_unlock()`.
  **Constraint 4 caveat**: the array is sized `hashsize(item_lock_hashpower)`
  (default 2^13 = 8192 or similar), so 192 bytes × 8192 = 1.5MB versus 320KB of
  `pthread_mutex_t`. That is large but not fatal on this machine — unlike
  fluidanimate, memcached's locks are not co-located with the hot data, so the
  footprint cost is a fixed allocation, not a cache-behaviour change. Still,
  measure with `item_lock_hashpower` lowered (raising per-lock contention, which
  is what we *want* for an LHP experiment) as well as at default.
- **`sys_ivh_cs_enter()`**: at the top of `item_lock()`. Memcached returns to
  userspace constantly (it's syscall-driven), so the danger bit will be fresh —
  constraint 5 is favourable here, unlike vips.
- **`extend()`/`unextend()`**: inside `item_lock()` after the acquire and inside
  `item_unlock()` before the release. Because acquisition and release are
  separate functions, the `extend()`/`unextend()` pairing is *structurally*
  guaranteed correct, which is much safer than hand-placing it.
- **Heartbeat**: item CSes are hash-bucket pointer manipulation — far under
  50µs. No in-CS instrumentation needed.

**How to run it**: `memcached -t 16` (thread count = vCPU count, the AFL's
validated regime), driven by `memtier_benchmark` or `mc-crusher` from **the same
host** (otherwise you're measuring the network). Use a **small key space** and a
**low `item_lock_hashpower`** to concentrate contention — the default
configuration is deliberately designed *not* to contend, which would give a null
result for uninteresting reasons. A heavy `SET` mix contends more than `GET`.

**Note on the MOSBench role**: memcached also appears in MOSBench, where it is
used to stress the *kernel* network stack (UDP socket locks). Those are two
genuinely different experiments on the same binary — see II.6.5.

### I.3.2 Redis / memtier — **Unlikely**. Already measured, already explained.

**Userspace.** Measured **-5%** (`memtier_benchmark -t 8 -c 25 --test-time 12`
against `redis-server 7.0.15`: off ~190k ops/s, on ~182k).

Redis executes **all commands on a single thread**. Redis 6+ added `io-threads`,
but those only do socket read/write and protocol parsing — command execution
remains serialized on the main thread by design. There is therefore **no
multi-threaded lock holder to protect**, and the only thing IVH can do is
migrate the *memtier client* threads, which is pure overhead.

This is the cleanest negative prediction the model makes, and it was confirmed.
Do not re-test, and do not tune. Its value now is purely as documentation that
"popular, heavily-contended-looking server" is not a sufficient criterion.

### I.3.3 RocksDB `db_bench` — **Medium**, with a genuinely novel comparison.

**Userspace.** Not measured. RocksDB's write path is the interesting part:

- **`DBImpl::mutex_`** (`db/db_impl/db_impl.h`) is a single global DB mutex held
  for memtable switches, compaction scheduling, superversion installs, and
  manifest writes. High concentration. CS lengths vary from very short
  (scheduling decisions) to long (memtable switch) — i.e. it **spans the valley
  within one run**, which is a measurement hazard.
- **`WriteThread`** (`db/write_thread.cc`) implements group commit: writers join
  a batch group, one becomes leader, followers wait. Crucially,
  **`WriteThread::AwaitState()` already implements exactly our algorithm** —
  bounded spin (tuned by `max_yield_usec_`), then `std::this_thread::yield()`,
  then a futex/condvar sleep.

That last point makes RocksDB the one candidate offering a **direct head-to-head
against a production-quality adaptive spin**: our contribution over
`AwaitState()` is specifically the **TSC-heartbeat staleness check** (bail when
the *holder* is demonstrably descheduled, rather than after a fixed spin
budget). If the IVH hypothesis is right, adding staleness detection to
`AwaitState()` should beat its fixed `max_yield_usec_` under host contention and
tie it without. That is a publishable-shaped result, not just a percentage.

**The risk, stated plainly**: the leader/follower write group *is* a
producer-consumer handoff — §1(e)'s confirmed-bad shape. Followers block on the
leader completing their batch. So RocksDB could easily reproduce the dedup
result for structural reasons that have nothing to do with `AwaitState()`'s
quality.

**Integration plan.**
- **`ivh_afl_lock.h`**: two separate, independently-testable experiments.
  (i) Replace the `port::Mutex` backing `DBImpl::mutex_` — one type, one place
  (`port/port_posix.h`), but note `DBImpl::mutex_` is used with condition
  variables (`InstrumentedCondVar`) throughout, which the AFL does not provide
  (the dedup disqualifier). This experiment is probably **not viable** without
  also building a condvar.
  (ii) Add the staleness check to `WriteThread::AwaitState()` directly, keeping
  its existing structure. **This is the viable one** and it is a much smaller
  diff: publish a heartbeat from the leader as it processes the group, and have
  followers bail to `FUTEX_WAIT` on staleness instead of on spin budget.
- **`sys_ivh_cs_enter()`**: before `JoinBatchGroup()`.
- **`extend()`/`unextend()`**: around the leader's batch-write region. The
  leader's CS is long (it writes the whole group's WAL), so **constraint 2
  applies**: the leader must publish heartbeats from inside `WriteBatchInternal`
  processing, or waiters will falsely declare it stale. This is the real work.

**How to run it**: `db_bench --benchmarks=fillrandom,overwrite,readwhilewriting
--threads=16 --duration=...`. `overwrite` and `fillrandom` maximize write-path
contention; `readrandom` is nearly lock-free (block cache) and is the natural
negative control within the same binary — the ebizzy malloc/mmap trick again.

### I.3.4 LevelDB `db_bench` — **Medium-High**. RocksDB's story, one-tenth the code.

**Userspace.** Not measured. LevelDB has **a single `DBImpl::mutex_`** guarding
essentially all shared state, with no group-commit machinery: writers queue on
`writers_` (a `std::deque<Writer*>`), the front writer becomes the batch leader,
others wait on their own condvar. Maximal concentration, small codebase, trivial
build (no dependencies beyond a C++ toolchain).

It is strictly easier to work with than RocksDB and tests the same hypothesis at
lower fidelity. **Recommendation: do LevelDB first as a scouting run for
RocksDB.** Same condvar caveat applies (the writer queue is condvar-based), so
the realistic experiment is again staleness-detection-in-the-wait, not a mutex
swap.

`db_bench --benchmarks=fillrandom --threads=16` is the contended case;
`readrandom` the control.

### I.3.5 PostgreSQL / pgbench — **Medium**, blocked on constraint 3.

**Userspace (process-per-backend).** Not measured.

PostgreSQL is mechanistically one of the *best* LHP stories in existence, and
the community has documented it as such for years:

- **`LWLock`** (`src/backend/storage/lmgr/lwlock.c`) — PG's own lightweight
  lock. `LWLockAcquire()` does a bounded attempt, then queues the backend and
  sleeps on its `PGPROC` latch (a semaphore/futex). Hot instances:
  `ProcArrayLock` (snapshot acquisition — every transaction),
  `WALInsertLock`(s), `XidGenLock`, `CLogControlLock`, buffer-mapping partition
  locks, and the per-buffer content locks.
- **`s_lock`** (`src/backend/storage/lmgr/s_lock.c`) — a genuine
  test-and-set **spinlock with a delay loop** used for the short
  `BufferDesc` header CSes and inside LWLock. This is the textbook LHP victim:
  spinning on a spinlock whose holder's vCPU has been descheduled, with a
  fixed backoff that knows nothing about it. `s_lock.c` even carries comments
  about this exact pathology.

`LWLockAcquire()`/`LWLockRelease()` and `s_lock()`/`S_UNLOCK()` are **single
functions**, so the integration points are ideal.

**But**: PG is process-per-backend with all of this in `mmap`'d shared memory.
Constraint 3 means the AFL's `FUTEX_*_PRIVATE` ops are simply wrong here — they
would key on (mm, addr), and two backends' mappings of the same shared page are
different mms. **A shared-futex variant of `ivh_adaptive_futex_lock.h` must be
built and validated before PostgreSQL is attempted at all.** The same variant
unblocks Apache prefork, nginx, and MOSBench exim, so it is a shared dependency
worth scheduling deliberately rather than discovering mid-port.

Secondary concerns: `extend()`/`unextend()` use rseq, which is per-thread and
works fine per-process; `sys_ivh_cs_enter()` likewise. The TSC heartbeat lives
in shared memory and works across processes unmodified (it's just a `rdtsc`
store) — so only the futex ops are the problem.

**How to run it**: `pgbench -c 64 -j 16 -S` (read-only, maximizes ProcArrayLock
/ buffer-mapping contention) and `-c 64 -j 16` (read-write, adds
WALInsertLock). Connection count well above vCPU count is the point — that's
the oversubscription that creates LHP. Use `pg_stat_activity`'s `wait_event`
and PG's own `LWLOCK_STATS` build to confirm *which* lock is hot before
porting.

### I.3.6 MySQL / InnoDB — **Medium**. Real contention, very large lift.

**Userspace (threaded).** Not measured. InnoDB has a long, well-documented
history of lock contention: `lock_sys->mutex`, `trx_sys->mutex`, the
adaptive-hash-index latch (`btr_search_latch`), buffer-pool instance mutexes and
per-block mutexes, and the log system mutex. It is threaded (not
process-per-connection like PG), so **constraint 3 does not apply** — a real
advantage over PostgreSQL.

InnoDB also, like RocksDB, already has its **own** adaptive sync primitives:
`ib_mutex_t`/`rw_lock_t` spin for `innodb_sync_spin_loops` iterations with
`innodb_spin_wait_delay` backoff, then block on an `os_event`. So the same
head-to-head opportunity exists as with RocksDB's `AwaitState()`, and MySQL's
knobs (`innodb_sync_spin_loops`, `innodb_spin_wait_delay`,
`innodb_thread_concurrency`) let you sweep the fixed-budget baseline properly
before claiming the staleness heuristic beats it.

**Against it**: the build is enormous, the sync layer
(`storage/innobase/include/sync0*.h`, `ut0mutex.h`) is templated and heavily
platform-conditional, and a realistic OLTP workload adds storage variance that
can easily exceed a 10–30% signal. **Medium, and only after LevelDB/RocksDB have
shown the staleness heuristic beating a fixed spin budget on something
cheaper.**

`sysbench oltp_read_write --threads=64` is the standard driver — note again the
sysbench dual-role caveat (§I.4.1).

### I.3.7 Cassandra — **Unlikely**.

**Userspace (JVM).** Not measured. Cassandra's locking is Java-level:
`synchronized` blocks (HotSpot `ObjectMonitor`, which spins then parks via
futex), `java.util.concurrent` locks (AQS, futex-backed), and heavy use of
lock-free structures. None of it is patchable without modifying HotSpot — and at
that point you are doing a JVM project, not a Cassandra benchmark.

Worse, JVM GC pauses and JIT warmup introduce run-to-run variance on the order
of tens of percent, which is the same magnitude as the effect being measured.
**Unlikely; do not attempt.** The only defensible Cassandra experiment is
migration-only (zero source changes), and even then the variance makes it a poor
place to look for a 10% signal.

This generalizes: **any managed-runtime workload (JVM, .NET, Go) is a poor AFL
target**, because the lock implementation belongs to the runtime and the runtime
adds variance. Go is a partial exception in that its runtime is statically
linked and patchable, but `sync.Mutex`'s interaction with the goroutine
scheduler makes "preempted lock holder" mean something different.

### I.3.8 Apache httpd — **Low**.

**Userspace (multi-process or hybrid).** Not measured. The classic contention
point is the **accept mutex** (`AcceptMutex`, implemented over
fcntl/flock/SysV-sem/pthread depending on build) serializing `accept()` across
workers — a textbook LHP target *if* it's in use. But:

- With the modern **event MPM** and `reuseport`, the accept mutex is largely
  gone; the work moves into the kernel's socket/accept path.
- prefork/worker MPMs are **multi-process**, so constraint 3 blocks the AFL.
- The remaining shared state (the scoreboard) is low-traffic.

**Verdict: Low.** Apache is worth running only as a MOSBench-style *kernel*
stressor (see II.6.3), where it needs no source changes at all.

### I.3.9 nginx — **Low**.

**Userspace (multi-process).** Not measured. nginx's `ngx_accept_mutex` is a
hand-rolled shared-memory lock (`ngx_shmtx_t`: an atomic word with a spin loop,
falling back to a POSIX semaphore) — architecturally *very* close to what the
AFL replaces, and cross-process, so it would be the natural first customer for
the shared-futex AFL variant. But `accept_mutex` defaults to **off** in modern
nginx (since 1.11.3) because `reuseport` supersedes it, so the default
configuration has essentially no userspace lock contention: each worker is a
single-threaded event loop.

You could force `accept_mutex on;` to create the contention artificially, but
then you are benchmarking a deprecated configuration. **Low**, unless the
shared-futex variant gets built for PostgreSQL anyway, in which case nginx with
`accept_mutex on` is a cheap second data point.

---

## I.4 Microbenchmarks and other userspace candidates

### I.4.1 sysbench `mutex` and `threads` — **High**. Cheapest real experiment here.

**Userspace.** **Role caveat first**: sysbench is already used in this project
as a **host-side corunner / load generator** (creating the host contention that
makes IVH relevant). Using it as a *target* workload is a completely separate
role, and the two must never run in the same experiment — a sysbench target
measured against a sysbench corunner would be uninterpretable. Use a different
corunner (`vcap_probe`, `stress-ng`) when sysbench is the target.

Two of sysbench's built-in tests are purpose-built lock microbenchmarks:

- **`sysbench mutex`** — `--mutex-num` mutexes, `--mutex-locks` acquisitions per
  thread, `--mutex-loops` iterations of an empty loop *inside* the critical
  section. That last knob is the important one: **it is a direct CS-length
  dial**, which means sysbench mutex can reproduce this project's own CS-length
  valley sweep on third-party code. That is exactly the independent
  confirmation the 2026-09-11 doc says the valley needs (it was only ever
  measured on NHextend, at 3-round rigor, in the 50–234µs region).
  `--mutex-num=1` gives the maximal-concentration NHextend shape;
  larger values sweep toward the distributed shape.
- **`sysbench threads`** — `--thread-yields`, `--thread-locks`: threads
  repeatedly lock a mutex from a small array and `sched_yield()`. Deliberately
  creates scheduler-interaction-heavy lock contention. Closer to a
  "preemption while holding" generator than `mutex` is.

**Integration plan.** sysbench's mutex test is a single small C file
(`src/tests/mutex/sb_mutex.c`, *unverified path*) using `pthread_mutex_t`
directly, with the lock/unlock in one loop body. Swapping in `ivh_afl_lock` is a
~20-line diff, and the three primitives go in the obvious places. Because
`--mutex-loops` controls CS length explicitly, the in-CS heartbeat is easy to add
correctly (drive it from the loop counter with `IVH_AFL_BEAT_INTERVAL`, exactly
as `NHextend-full.c:690` does) — meaning this candidate can test the *long*-CS
regime properly, which most candidates cannot.

**Verdict: High, and do it early.** It is the lowest-effort way to check whether
the NHextend results are a property of LHP or a property of NHextend.

### I.4.2 Phoenix / Phoenix++ — **Low-Medium**.

**Userspace.** Not measured. Shared-memory MapReduce. The architecture is: split
input into chunks, map workers emit into **per-worker intermediate buffers**
(deliberately unshared to avoid locks — this is Phoenix++'s headline design
improvement over Phoenix), then reduce/merge phases with barriers. The scheduler
has a task queue with a lock, but it is hit once per chunk, not per key.

Per-workload, for granularity:

| workload | shape | verdict |
|---|---|---|
| `word_count` | large intermediate key space, heaviest merge phase | **Low-Medium** — the most sync-active of the set |
| `reverse_index` | similar, string-heavy | Low-Medium |
| `histogram` | tiny fixed key space, pure per-thread reduction | Low |
| `linear_regression` | scalar reduction only | Low — essentially blackscholes |
| `matrix_multiply` | no shared state at all | Low |
| `string_match` | embarrassingly parallel | Low |
| `kmeans` | barrier per iteration, per-thread partial sums | Low |
| `pca` | barrier-structured, two passes | Low |

**Verdict**: the suite is *designed* to avoid locks, which makes it a poor AFL
target almost by construction. It is a reasonable **migration-only** sweep
(barrier + task-queue workloads) and a decent source of negative controls.

### I.4.3 Metis — **Low**.

**Userspace.** Not measured. MIT's MapReduce library (used in MOSBench). Like
Phoenix++, it is built around **per-core hash tables with no cross-core
locking** during map, with merging at defined phase boundaries. Metis's research
contribution was precisely that it avoids shared-structure contention. Nothing
for the AFL. Worth including in a MOSBench-style sweep as a kernel-side memory
allocator / page-fault stressor (it's `mmap`-heavy), which is where its value
actually is — see II.6.6.

### I.4.4 pbzip2 — **Unlikely**. Already tried.

**Userspace.** Measured: **neutral/noisy**, no clean ≥5% effect, across
`-p{4,8,12,16}` on 540MB–1GB inputs. Two independent reasons it fails:
(a) it is a producer-consumer queue with mutex+condvar — dedup's shape and
dedup's condvar disqualifier; (b) a **measurement-floor problem** documented in
`ivh_benchmark_search_2026-07-20.md`: compressible input finishes in ~4s
(too short to measure), incompressible input makes it I/O-bound instead. There
is no input that gives a long CPU-bound run.

**Verdict: closed.** Do not revisit.

### I.4.5 SPEC CPU 2017 — **Unlikely**, documented to close the question.

**Userspace.** Not measured, and should not be.

- **SPECrate** runs N *independent copies* of a single-threaded program. There
  is zero shared state and zero shared locking between copies. The only IVH
  effect possible is the kernel-side rebalancing one (blackscholes's ~-3%
  mechanism), and even that is muted because the copies are pure compute with no
  syscalls.
- **SPECspeed** uses OpenMP within a benchmark, but the parallelism is
  loop-level with reductions and implicit barriers — barrier-shaped, not
  lock-shaped, and the barrier counts are low (one per outer iteration, not
  thousands per second as in streamcluster).
- SPEC's run rules and 3-run medians also make a paired interleaved
  off/on design awkward.

**Verdict: Unlikely; skip.** If a "does IVH hurt normal software?" number is
ever needed for a paper, a single SPECrate run is a defensible *regression
check*, not a benefit demonstration.

### I.4.6 LEBench — **Medium as a cost check**, Unlikely as a win.

**Userspace, but see the note.** Not measured. LEBench is a set of ~20
single-threaded OS-primitive latency microbenchmarks (read, write, mmap,
munmap, page fault, fork, thread create, context switch, send/recv, poll,
select, epoll, ...), designed to isolate *syscall latency*, not throughput or
contention.

Being single-threaded, it has **no contention for IVH to fix** — so it cannot
show a benefit. But that is exactly what makes it valuable for a different
question this project should be able to answer: **what does IVH's own
instrumentation cost on the uncontended path?** IVH adds `ivh_pre_lock()` to
`_raw_spin_lock()` and per-tick evaluation in the scheduler; LEBench's
`fork`, `thread create`, `context switch`, `page fault` and `mmap` tests are
precisely the syscalls that take the most kernel spinlocks, and its
single-threaded design means any delta is *pure overhead*, not contention.

**Verdict: Medium priority, as an overhead audit rather than a candidate.** Run
it with `ivh_universal_eligible=0` vs `=1` and, separately, IVH kernel vs
vanilla 6.17 (`/root/kernels/linux-6.17-vanilla`) to separate "the engine firing"
from "the instrumentation existing."

### I.4.7 TPC-C-style OLTP (HammerDB / BenchmarkSQL / sysbench-tpcc) — **Medium**.

**Userspace (driver) + the database under test.** Not measured. This is the most
*realistic* workload in the survey — genuine multi-user OLTP with real lock
contention on hot rows (the district/warehouse counters in the New-Order
transaction are the classic contention point) and heavy internal DB lock
traffic.

But it inherits whatever DB it runs on (see I.3.5/I.3.6), adds a large setup and
warm-up cost, and introduces storage variance. Its role is **validation of a
result found elsewhere**, not discovery: if the staleness heuristic wins on
LevelDB and then wins on TPC-C-over-PostgreSQL, that is a strong story. Running
TPC-C first would be an expensive way to get an ambiguous number.

**Verdict: Medium, scheduled last.**

---

# PART II — KERNEL-SPACE CANDIDATES

These exercise **kernel-internal locks via syscalls**. Several are userspace
*binaries* (will-it-scale, fxmark, stress-ng, MOSBench) but there is no
benchmark-side lock to swap, so functionally they are kernel-space for IVH
purposes.

**All of them get the migration engine and kernel adaptive spinning (tier-1/
tier-2) automatically, with zero source changes**, via `_raw_spin_lock()`'s
`ivh_pre_lock()` instrumentation and the scheduler tick. The only "integration"
is setting sysctls. That makes this whole section dramatically cheaper to run
than Part I — and it is where this project's largest confirmed wins already live
(ebizzy +136%, dbench +34%, hackbench +29%/+74%).

**Read II.A before using that sentence.** The two mechanisms cover *different*
locks, and the `ivh_pre_lock()` half covers far less than its framing suggests.

## II.A ⚠ Verified hook coverage — which kernel locks actually get the pre-lock hook

This was checked directly against this tree and it materially changes how the
rest of Part II should be read. `ivh_pre_lock()` is defined at
`kernel/locking/spinlock.c:266` and called from **exactly four call sites, all
in that one file**:

| function | line | primitive |
|---|---|---|
| `_raw_spin_lock()` | `spinlock.c:788` | `raw_spinlock_t` / `spinlock_t` |
| `_raw_spin_lock_irqsave()` | `:803` | same, IRQs saved |
| `_raw_spin_lock_irq()` | `:817` | same, IRQs off |
| `_raw_spin_lock_bh()` | `:831` | same, softirqs off |

`grep -rl ivh_pre_lock kernel/locking/ kernel/sched/` returns only
`spinlock.c`, `bpf_sched.c` and `fair.c`. **There is no pre-lock migration hook
in `mutex.c`, `rwsem.c`, `rtmutex.c`, `percpu-rwsem.c`, or the rwlock paths.**
Note also that all four hooked functions are marked `noinline` — so a kernel
built with `CONFIG_INLINE_SPIN_LOCK*` would bypass them entirely; that is not
the case here, but it is worth knowing before comparing against another build.

**Three consequences that reshape this survey:**

1. **The project's single biggest confirmed win did not come from
   `ivh_pre_lock()`.** ebizzy `-m` (+136%) is bottlenecked on `mmap_lock`, which
   is an **rwsem**, not a spinlock — so it never touches the pre-lock hook. That
   win must be attributable to the **periodic scheduler-tick evaluation path**
   instead. The same applies to `i_rwsem` (dbench `-F` +33.7%, and every fxmark
   metadata workload) and to will-it-scale's `page_fault*` and `mmap*` tests.
   This is worth confirming empirically — a kprobe on
   `bpf_sched_pre_lock_migrate()` during an ebizzy `-m` run should show few or no
   hits, exactly as `ivh_adaptive_spinning_glock13_findings_2026-09-03.md` §7 did
   for hackbench when migration was dormant. **If that is right, the "IVH
   protects lock holders via `ivh_pre_lock()`" framing undersells the tick path
   and mis-attributes the headline result.** That is a correction worth making
   before the next write-up, and it is cheap to check.

2. **The spinlock-bottlenecked candidates are the ones that genuinely exercise
   the pre-lock path**: fxmark `MRPH`/`MRPM` (dcache `d_lock` / `d_lockref`),
   will-it-scale `lock1`/`lock2` (file-lock list spinlock), `context_switch1`
   (runqueue locks), the futex tests (futex hash-bucket spinlocks), hackbench
   (socket/wait-queue locks), and `perf bench sched messaging`. These should be
   prioritized specifically when the question is "does `ivh_pre_lock()` work,"
   as opposed to "does IVH help."

3. **locktorture's non-spinlock types become a deliberate coverage audit, not
   just a sweep.** `mutex_lock`, `rwsem_lock`, `rw_lock`, `rw_lock_irq`,
   `rtmutex_lock` and `percpu_rwsem_lock` are precisely the primitives with **no
   pre-lock hook**. Running them is how you measure what the tick path alone
   delivers, with the pre-lock path provably absent — a cleaner mechanism split
   than any sysctl toggle can give you. That raises their value above the
   "Low-Medium" the table assigns them; re-read row 28 with this in mind.

A corollary worth stating: **the userspace adaptive lock is irrelevant to every
entry in Part II.** If a Part II benchmark is slow because of a kernel
`i_rwsem`, no amount of userspace lock work helps it; conversely it needs no
porting effort at all.

## II.0 Priority table (kernel-space)

| # | Benchmark | Suite | Priority | One-line reason |
|---|---|---|---|---|
| 1 | **ebizzy `-m` (mmap mode)** | LTP | **High (confirmed)** | **+136% measured.** `mmap_lock` held across page faults — the textbook LHP target. Already the project's best result. **But `mmap_lock` is an rwsem, so this never touched `ivh_pre_lock()` — see II.A.** |
| 2 | **dbench `-F` on ext4** | dbench | **High (confirmed)** | **+33.7% / +20.2% measured.** Threads block on real disk I/O ⇒ low migration collateral. |
| 3 | **hackbench `-g4 -l30000`** | rt-tests | **High (confirmed)** | **+28.6%** (and **+73.9%** for `-T -g1 -f8 -l400000`). Group count is the dominant lever. |
| 4 | **will-it-scale `page_fault1/2/3`** | will-it-scale | **High** | Direct `mmap_lock` + per-VMA lock contention — the same mechanism ebizzy mmap-mode exploits, in a controlled form. |
| 5 | **fxmark `MRPH` / `MRPM`** | fxmark | **High** | Path-lookup on shared files: `d_lockref` / dentry-cache contention, the canonical kernel lockref scalability wall. |
| 6 | **will-it-scale `lock1` / `lock2`** | will-it-scale | **High** | POSIX file-lock (`flock`/`fcntl`) contention on a global-ish kernel lock. |
| 7 | **locktorture `spin_lock` / `spin_lock_irq`** | in-tree | **High (as instrumentation)** — ⚠ needs a rebuild | Directly stresses the exact primitive IVH instruments; the cleanest way to see tier-1/tier-2 adaptive spinning in isolation, and the only way to sweep the CS-length valley on *kernel* locks. But `CONFIG_LOCK_TORTURE_TEST is not set` in this tree's `.config` — see II.4. |
| 8 | **fxmark `MWCL` / `MWCM` / `MWUL` / `MWUM`** | fxmark | **High** | Create/unlink in shared dirs: parent `i_rwsem` write side + dcache. Classic LHP victim. |
| 9 | **stress-ng `--lockbus`** | stress-ng | **Medium-High** | Bus-locking atomics; directly attacks the cacheline-contention regime. |
| 10 | **perf bench `futex lock-pi`** | in-tree | **Medium-High** | PI-futex = kernel rtmutex with priority inheritance — the kernel's own LHP mitigation, a natural comparison point. |
| 11 | **Filebench `varmail`** | Filebench | **Medium-High** | fsync-heavy multithreaded — the shape that made dbench `-F` win +34%. |
| 12 | **will-it-scale `unlink1/2`, `open1/2/3`** | will-it-scale | **Medium-High** | Directory `i_rwsem` + dcache, same family as fxmark's metadata workloads. |
| 13 | **MOSBench `exim`** | MOSBench | **Medium-High** | fork/exec + per-message spool file creation: the heaviest VFS-metadata + process-churn mix available. |
| 14 | **perf bench `sched messaging`** | in-tree | **Medium-High** | This *is* hackbench, in-tree — and hackbench is a confirmed +29%/+74% win. Free to run. |
| 15 | **stress-ng `--futex` / `--mutex` / `--sem`** | stress-ng | **Medium** | Direct futex-path stressors; useful for isolating the kernel futex hash-bucket lock. |
| 16 | **locktorture `mutex_lock` / `rwsem_lock`** | in-tree | **Medium** | Sleeping locks — different path from qspinlock, tests whether IVH's coverage has a hole. |
| 17 | **fxmark `DWAL` / `DWOL` / `DWOM`** | fxmark | **Medium** | File append/overwrite: `i_rwsem` write vs read side, a controlled concentration sweep. |
| 18 | **will-it-scale `futex1-4`** | will-it-scale | **Medium** | Futex hash-bucket spinlock contention, varying sharing. |
| 19 | **MOSBench `psearchy`** | MOSBench | **Medium** | mmap/page-fault-heavy indexing; ebizzy-mmap's shape at application scale. Previously skipped for setup cost. |
| 20 | **Filebench `fileserver` / `webserver`** | Filebench | **Medium** | Multithreaded VFS mixes; less fsync-dominated than varmail. |
| 21 | **MOSBench `memcached` (kernel role)** | MOSBench | **Medium** | UDP socket + network stack locks — distinct from the userspace item-lock experiment in I.3.1. |
| 22 | **will-it-scale `context_switch1`, `pipe1`** | will-it-scale | **Medium** | Scheduler + pipe locks; hackbench's mechanism, controlled. |
| 23 | **perf bench `futex hash` / `wake` / `wake-parallel` / `requeue`** | in-tree | **Medium** | Futex internals; `wake-parallel` in particular creates the thundering-herd the AFL's wake-skip flag exists to avoid. |
| 24 | **MOSBench `apache`** | MOSBench | **Medium** | accept/socket path; kernel-side only. |
| 25 | **stress-ng `--flock` / `--locka` / `--lockf` / `--lockofd`** | stress-ng | **Medium** | File-locking paths; overlaps will-it-scale lock1/2 with more variants. |
| 26 | **dbench (tmpfs, no `-F`)** | dbench | **Medium** (as a documented loser) | Measured **-19.3%** at 16 clients. A confirmed negative that the model explains. Keep as a control. |
| 27 | **fxmark `MRDL` / `MRDM`, `DRBL/DRBM/DRBH`** | fxmark | **Low-Medium** | readdir / block-read paths: mostly rwsem *read* side, less LHP-shaped. |
| 28 | **locktorture `percpu_rwsem_lock`, `rtmutex_lock`, `ww_mutex_lock`, `rw_lock*`, `raw_*`** | in-tree | **Low-Medium** | Coverage sweep; some of these are exactly where IVH has *no* hooks, which is worth documenting. |
| 29 | **MOSBench `gmake`** | MOSBench | **Low** | Kernel build — measured **-11.8%**. See II.7. |
| 30 | **Kernbench / local kernel build** | build | **Low** as a win; **Medium** as a safety check | Measured **-11.8%** at `-j16` on tinyconfig. Concrete local plan in II.7. |
| 31 | **MOSBench `metis` (kernel role)** | MOSBench | **Low** | mmap/page-fault stressor; overlaps psearchy and ebizzy. |
| 32 | **stress-ng `--spinlock`, `--mcontend`, `--atomic`, `--switch`, `--yield`** | stress-ng | **Low** | Already used as *load generators* in this project's own scripts; as targets they're synthetic to the point of being uninformative. |
| 33 | **ebizzy (malloc mode)** | LTP | **Low** / control | Measured **-2%**. The control that proves ebizzy's win is `mmap_lock`, not ebizzy. Keep. |
| 34 | **perf bench `mem memcpy/memset`, `find-bit`, `syscall basic`, `breakpoint`, `uprobe`, `internals`** | in-tree | **Unlikely** | No contention by design; memory-bandwidth or single-syscall latency. Overhead-audit value only. |
| 35 | **perf bench `numa mem`** | in-tree | **Unlikely** here | NUMA placement benchmark; this is a 16-vCPU single-node CVM. |
| 36 | **`perf bench sched pipe`** | in-tree | **Unlikely** | Two-process ping-pong: **zero contention** (only one waiter ever). Pure context-switch latency. Good overhead probe, not a candidate. |

## II.1 will-it-scale, per test

**Userspace binaries; functionally kernel-space.** will-it-scale runs the same
tiny operation in N tasks (threads *and* processes — it builds both variants of
every test) and reports aggregate ops/s, which makes it the cleanest available
tool for isolating *one* kernel lock.

**Caveat on the test list**: the exact set varies by version and I could not
check out the repo from this host, so treat the names below as
*to-be-confirmed* (`ls tests/*.c` in the checkout). The ones I am confident
exist and are relevant:

| test | kernel lock stressed | priority | note |
|---|---|---|---|
| `page_fault1` | `mmap_lock` (read) + page allocation, single shared mapping | **High** | The direct analogue of ebizzy `-m`'s +136%. |
| `page_fault2` | same, private per-task mappings | **High** | Control pair with `page_fault1`: isolates the *shared-lock* component. |
| `page_fault3` | file-backed faults, page cache + `i_rwsem` | **High** | Adds the page-cache lock to the mix. |
| `lock1` | `flock()` / POSIX file locks | **High** | File-lock list is a coarse kernel lock — strong concentration. |
| `lock2` | `fcntl()` byte-range locks | **High** | Same, different path (`file_lock_context`). |
| `mmap1` | `mmap_lock` write side + VMA tree | **High** | Write-side rwsem = maximal concentration. |
| `mmap2` | `mmap`/`munmap` pairs | High | Adds TLB-shootdown IPIs, which interact with vCPU preemption interestingly. |
| `futex1`–`futex4` | futex hash bucket spinlocks, varying sharing | **Medium** | Directly relevant to the AFL's own syscall cost. |
| `unlink1`, `unlink2` | parent dir `i_rwsem` write + dcache | **Medium-High** | Same family as fxmark MWUL/MWUM. |
| `open1`, `open2`, `open3` | path walk + file table (`files_struct` lock) | **Medium-High** | `open3` (same file) concentrates hardest. |
| `context_switch1` | scheduler runqueue locks | **Medium** | hackbench's mechanism, minimal. |
| `pipe1` | pipe mutex + wait queues | **Medium** | Producer-consumer shaped — expect the §1(e) caution to apply. |
| `posix_semaphore1` | futex path via glibc sem | Medium | Pairs with `futex*`. |
| `pthread_mutex1`–`pthread_mutex5` | glibc mutex ⇒ futex | **Medium**, *and see below* | |
| `malloc1`, `malloc2` | glibc arena locks + `brk`/`mmap` | Medium | Userspace arena lock is glibc's, not ours. |
| `brk1`, `brk2` | `mmap_lock` write side | Medium | Cheap `mmap1` variant. |
| `signal1` | signal delivery, `sighand->siglock` | Low-Medium | |
| `tlb_flush1/2/3` | TLB shootdown IPIs | Low-Medium | IPI behaviour under vCPU preemption is genuinely interesting but hard to attribute. |
| `sched_yield` | runqueue lock | Low | |
| `dup1`, `eventfd1`, `poll1/2`, `read*`, `write*`, `lseek*`, `getppid1` | mostly per-task | Low/Unlikely | Little or no shared-lock contention. |

**The one exception to "no benchmark-side lock to swap": `pthread_mutex1`–`5`.**
These tests measure glibc's `pthread_mutex_t` directly. That means they can be
rebuilt against `ivh_adaptive_futex_lock.h` to give a **direct, controlled A/B
of our lock against glibc's**, at every thread count, with a harness that
already handles pinning, scaling curves and reporting. That is an unusually
cheap high-signal experiment and it deserves its own line in the plan. Note the
result would be interpreted carefully: will-it-scale's mutex tests have an
essentially empty critical section, which is the far-short end of the valley and
the regime where the AFL's spin path should look best. It is not a
representative workload — it is a *primitive* comparison, and should be reported
as one.

**How to run**: `./runtest.py <test>` sweeps task counts 1..N; for IVH, the
interesting region is **at and above 16** (vCPU count), since LHP requires
oversubscription. Run both the `_threads` and `_processes` variants — they
differ in whether `mmap_lock` is shared, which is the whole point.

## II.2 fxmark, per workload

**Userspace binary; kernel-space for IVH.** fxmark is a filesystem scalability
suite built to isolate *specific* VFS/FS locks, with a systematic workload
naming scheme. **The naming decode below is recalled, not verified** — check
against the repo's own `bin/` workload list before relying on it:

- Letter 1: `D` = data operation, `M` = metadata operation
- Letter 2: `R` = read, `W` = write
- Letter 3: the specific operation (`A`=append, `O`=overwrite, `S`=sync,
  `B`=block read, `C`=create, `U`=unlink, `R`=rename, `P`=path/stat, `D`=readdir)
- Letter 4: sharing/contention level — `L` = low (private directory/file per
  task), `M` = medium (shared directory), `H` = high (same file/entry)

That last letter is what makes fxmark valuable here: **it is an explicit
concentration dial on an otherwise-identical workload**, which is exactly the
axis §1(c) says decides IVH's sign. Running a full `L`→`M`→`H` ladder on one
operation gives a concentration sweep the way sysbench `--mutex-loops` gives a
CS-length sweep.

| workload | lock stressed (expected) | priority |
|---|---|---|
| `MRPH` | dentry cache / `d_lockref` on one shared path — the canonical kernel scalability wall | **High** |
| `MRPM` | dentry cache, shared directory | **High** |
| `MRPL` | private paths — the low-contention control | Medium (as control) |
| `MWCM` | create in a shared dir: parent `i_rwsem` write + dcache insert | **High** |
| `MWCL` | create in private dirs — control | Medium (as control) |
| `MWUM` | unlink in a shared dir: `i_rwsem` write + dcache + inode free | **High** |
| `MWUL` | unlink, private — control | Medium |
| `MWRM` | rename in shared dir: `i_rwsem` on two parents + `rename_lock` seqlock | **Medium-High** |
| `MWRL` | rename, private | Medium |
| `MRDM` / `MRDL` | readdir: `i_rwsem` read side | Low-Medium |
| `DWAL` | append to private files: `i_rwsem` write + block alloc | **Medium** |
| `DWOL` | overwrite, private: `i_rwsem` read side (shared) + page cache | Medium |
| `DWOM` | overwrite, shared file | **Medium** |
| `DWSL` | write + fsync — **the dbench `-F` shape that won +34%** | **Medium-High** |
| `DRBL`/`DRBM`/`DRBH` | block reads, page cache | Low-Medium |

**Strong prior from this project's own data**: dbench regressed **-19.3%** on
tmpfs (cached, short VFS CSes, high collateral) and won **+33.7%** on ext4 with
fsync (threads genuinely block). Expect fxmark to reproduce that split:
**page-cache-resident workloads (`DRB*`, `DWO*` on tmpfs) should be losers;
the fsync and metadata-write workloads on a real ext4 device should be
winners.** Run fxmark on `/dev/vda2`-backed ext4, not tmpfs, or the result is
predetermined. Run each workload at 1, 8, 16, 32, 64 tasks.

## II.3 stress-ng, per stressor

**Userspace binary; kernel-space for IVH.** Important framing note: this project
already uses `stress-ng --spinlock N` as a **load generator** in its own scripts
(`experiment_plan.md` lines 530, 549, 617, 822). Using stressors as *targets* is
a separate role; don't do both at once.

| stressor | what it stresses | priority |
|---|---|---|
| `--lockbus` | bus-locked atomics on a shared cacheline | **Medium-High** — directly attacks the contended-cacheline regime, and is one of the few stressors whose bottleneck is genuinely a lock-like serialization |
| `--futex` | `futex(FUTEX_WAIT/WAKE)` round-trips, kernel futex hash bucket spinlock | **Medium** — directly relevant to the AFL's own syscall cost |
| `--mutex` | `pthread_mutex` contention | **Medium** — userspace-lock shaped, and could host an AFL swap like will-it-scale's pthread_mutex tests |
| `--sem` / `--sem-sysv` | POSIX / SysV semaphores | **Medium** |
| `--flock`, `--locka`, `--lockf`, `--lockofd` | `flock()`, POSIX advisory locks, `fcntl` locks, open-file-description locks | **Medium** — overlaps will-it-scale `lock1/2` with more coverage |
| `--spinlock` | userspace spin loop | **Low** — already the project's load generator; as a target it measures nothing but spin throughput |
| `--mcontend` | deliberate memory/cacheline contention | Low |
| `--atomic` | atomic ops, no lock | Low |
| `--switch` / `--yield` / `--sched` | context-switch and scheduler churn | Low — relevant as *corunner* configuration, not targets |
| `--fork` / `--clone` | process churn, `tasklist_lock`, mm teardown | Low-Medium |
| `--msg` / `--sigq` | SysV message queues / signals | Low |
| `--pthread` | thread create/destroy | Low |

**`--rwlock`**: the task brief lists an rwlock stressor. I could **not verify**
whether this build has one (no shell access outside the project tree, no
network). Check with `stress-ng --help | grep -i rwlock` before scripting it; if
present it belongs at Medium alongside `--mutex`, since kernel and userspace
rwlock behaviour under LHP (readers blocked behind a preempted writer) is a
genuinely distinct and interesting case that nothing else in this survey covers
well.

**General caution**: stress-ng stressors are *synthetic by design* and tuned to
saturate a primitive. A large IVH delta on `--lockbus` is weak evidence about
real workloads. Their value is **mechanism isolation** (which lock is IVH
actually helping?) not benefit demonstration.

## II.4 locktorture, per torture type

**Pure kernel-space**, in-tree at `kernel/locking/locktorture.c`, zero setup
cost, and the types below were **read directly from this tree** (category 2 —
these names are verified):

| `torture_type` | source line | primitive | priority |
|---|---|---|---|
| `spin_lock` | `locktorture.c:282` | `spinlock_t` ⇒ qspinlock | **High** — the exact primitive `ivh_pre_lock()` instruments |
| `spin_lock_irq` | `:309` | same, IRQs disabled | **High** — IRQ-off changes preemptibility, which is the whole IVH question |
| `raw_spin_lock` | `:335` | `raw_spinlock_t` | Medium-High |
| `raw_spin_lock_irq` | `:362` | same, IRQs off | Medium-High |
| `mutex_lock` | `:594` | sleeping mutex (optimistic spin + rwsem-style owner tracking) | **Medium** — mutex already has owner-on-CPU spinning; a natural comparison for our staleness heuristic |
| `rwsem_lock` | `:838` | `rw_semaphore` | **Medium** |
| `rw_lock` | `:481` | `rwlock_t` | Medium |
| `rw_lock_irq` | `:524` | same, IRQs off | Medium |
| `rtmutex_lock` | `:779` | PI rtmutex | Medium — PI is the kernel's *own* LHP answer |
| `percpu_rwsem_lock` | `:890` | percpu rwsem | Low-Medium |
| `ww_mutex_lock` | `:692` | wait/wound mutex | Low |
| `raw_res_spin_lock` | `:389` | resilient spinlock (BPF) | Low |
| `raw_res_spin_lock_irq` | `:414` | same | Low |
| `lock_busted` | `:237` | intentionally broken, for testing the test | Unlikely — not a benchmark |

**Why this matters more than its "torture test" name suggests**: locktorture is
the only tool here that lets you stress **one specific kernel lock primitive at
a time, with controllable hold time** (`torture_type=`, `nwriters_stress=`,
`nreaders_stress=`, and the stutter/hold parameters). That makes it the kernel
equivalent of sysbench `mutex --mutex-loops` — **the right instrument for
reproducing the CS-length valley on kernel locks**, which this project has never
done (the valley was measured only on NHextend's userspace lock).

It is also the cleanest way to see **tier-1/tier-2 adaptive spinning in
isolation**, since `spin_lock`/`spin_lock_irq` go straight through
`qspinlock_paravirt.h`'s MCS queue with nothing else in the way. Given that
adaptive spinning's measured effect is only ~1% with uncertain sign, a tool that
removes all other noise is worth a lot.

**Caveat**: locktorture reports operations completed, not a wall-clock workload
metric, and it deliberately induces pathological contention. It is an
instrument, not a benefit demonstration.

**⚠ Verified blocker — locktorture is NOT currently available.**
`/root/linux-6.17/.config` has:

```
# CONFIG_LOCK_TORTURE_TEST is not set
# CONFIG_LOCK_STAT is not set
```

Confirmed by `grep` on this tree's `.config` (category 2 — verified). Two
qualifications: (a) this is the *docs* tree's config, and the running kernel was
built from `/root/kernels/linux-6.17-vanilla` per
`ivh_nhextend3_migration_validation_2026-09-11.md` §1, which I could not read
from this session; (b) `/proc/config.gz` is not present on this host, so the
running kernel's config could not be checked directly. **Confirm with
`modprobe locktorture` or by grepping the vanilla tree's `.config` before
scheduling any locktorture work** — if it is off there too, locktorture needs a
kernel rebuild, which moves it out of the "zero setup" tier entirely. That is a
meaningful cost, but locktorture is still worth it: it is the only instrument
that can reproduce the CS-length valley on *kernel* locks, and the rebuild can
be folded into whatever the next kernel build is anyway. `CONFIG_LOCK_STAT=n`
also means the tracepoint-based `perf lock` mode is unavailable; the newer
BPF-based `perf lock contention` does not need it and should work.

**Verified config facts that do hold** (same `.config`): `CONFIG_PARAVIRT_SPINLOCKS=y`
(the pv-qspinlock path tier-1/tier-2 adaptive spinning lives in is compiled in),
`CONFIG_RSEQ=y` (`extend()`/`unextend()` and the danger bit are available),
`CONFIG_PREEMPTION=y` with `CONFIG_PREEMPT_DYNAMIC=y` and `CONFIG_PREEMPT_LAZY=y`,
`CONFIG_HZ=1000`. The 1000Hz tick matters for the migration engine specifically,
since the scheduler-tick evaluation path fires at that rate — worth stating
whenever a tick-driven number is reported.

## II.5 perf bench, per subcommand

**In-tree**, verified against `tools/perf/bench/` and `tools/perf/builtin-bench.c`
in this tree (category 2 — these names are verified):

| subcommand | what it does | priority |
|---|---|---|
| `sched messaging` | **This is hackbench**, in-tree (`sched-messaging.c`) | **Medium-High** — hackbench is a confirmed **+28.6%/+73.9%** win, so this is free confirmation with a different harness. Supports `-t` (threads) and `-g` (groups), the lever that mattered. |
| `futex lock-pi` | PI-futex ⇒ kernel rtmutex with priority inheritance | **Medium-High** — PI is the kernel's *existing* LHP mitigation; a direct comparison with IVH's approach |
| `futex wake-parallel` | many threads waking many waiters simultaneously | **Medium** — creates the thundering herd the AFL's 3-state wake-skip flag exists to avoid; good for validating that design |
| `futex hash` | futex hash-bucket spinlock throughput | **Medium** |
| `futex wake` | single-waker wake latency | **Medium** |
| `futex requeue` | `FUTEX_REQUEUE` (glibc condvar's mechanism) | **Medium** — the closest thing to testing the condvar path the AFL doesn't cover |
| `epoll wait` / `epoll ctl` | epoll ready-list and `ep->mtx` contention | Medium |
| `sched seccomp-notify` | seccomp notify round-trip | Low |
| `sched pipe` | **two-task ping-pong** (`sched-pipe.c`) | **Unlikely as a candidate** — there is only ever one waiter, so there is no contention at all; it measures pure context-switch/pipe latency. Excellent as an **overhead probe** for IVH's instrumentation cost. |
| `mem memcpy` / `memset` | memory bandwidth | Unlikely — no locks |
| `find-bit` | bitmap scan | Unlikely |
| `numa mem` | NUMA placement | Unlikely on a 16-vCPU single-node CVM |
| `syscall basic` | bare syscall latency | Unlikely as a candidate; useful as an overhead probe alongside LEBench |
| `breakpoint`, `uprobe`, `kallsyms-parse`, `inject-buildid`, `synthesize`, `evlist-open-close`, `pmu-scan` | perf-internal | Unlikely |

`perf bench sched messaging` deserves particular emphasis: since it is
hackbench and hackbench is already a confirmed large win, running it via `perf
bench` costs nothing (it's in this tree), uses a different code path than the
standalone hackbench binary, and would **independently confirm the project's
third-biggest result** with essentially zero effort.

## II.6 MOSBench, per component

**All kernel-space for IVH purposes** — MOSBench was explicitly designed to find
Linux kernel scalability bottlenecks, so its components' bottlenecks are kernel
locks by construction.

Historical note: `ivh_benchmark_search_2026-07-20.md` skipped psearchy, citing
"heavy multi-component MIT-PDOS setup." That judgement still stands for a
casual sweep. MOSBench is also old (2010-era) and its harness has bit-rotted;
in practice it is usually easier to run the individual applications directly
than to use the MOSBench harness.

**II.6.1 exim — Medium-High.** Mail server, one process per message, each
creating and fsyncing spool files. This is the **heaviest available mix of
process churn (fork/exec) + VFS metadata + fsync**, and it was MOSBench's
star bottleneck-finder. Two of the three ingredients are confirmed IVH winners
here (dbench `-F` +34% for the fsync/blocking part; hackbench +29% for the
process-churn part). The fork/exec side stresses `mmap_lock`, `tasklist_lock`,
and the dcache — all migration-engine territory, zero source changes.
**Caveat**: exim is multi-*process*, so if a userspace experiment were ever
wanted, constraint 3 applies. Setup is fiddly (mail config, spool directories);
budget real time.

**II.6.2 psearchy (pedsort) — Medium.** Parallel text indexer: each core indexes
a portion of a file tree into a per-core hash table, then merges to a shared
B-tree index. The indexing phase is `mmap`-heavy with a large working set —
which is **ebizzy mmap-mode's shape** (+136%) at application scale. That alone
justifies Medium despite the setup cost. The merge phase adds VFS write
traffic.

**II.6.3 apache — Medium.** httpd serving a small static file from many
clients. The bottleneck is the kernel accept/socket path plus (in older MPMs)
the accept mutex. Zero source changes needed for the kernel-side experiment.
Needs a client load generator on the same host, which competes for the same 16
vCPUs — a real methodological problem for any client/server benchmark on a
single CVM, and worth planning for (pin the generator, or accept that you're
measuring a closed system).

**II.6.4 gmake — Low.** A parallel kernel build. See II.7 — this project already
measured **-11.8%** at `-j16`, with a clear mechanistic explanation. Included
here only for completeness of the MOSBench enumeration.

**II.6.5 memcached (kernel role) — Medium.** In MOSBench, memcached is driven
over UDP to stress the **kernel network stack** (socket locks, UDP hash), not
its own item locks. This is genuinely a *different experiment* from I.3.1, on
the same binary: I.3.1 patches memcached's userspace locks and drives it to
contend those; II.6.5 leaves memcached untouched and drives it to contend
kernel socket locks. Run both; report them separately.

**II.6.6 metis — Low.** MapReduce over a large in-memory dataset. As noted in
I.4.3, Metis avoids shared locking by design, so its kernel-side interest is its
**memory allocation / page-fault behaviour** (large `mmap`s, heavy faulting) —
which overlaps psearchy and ebizzy without adding much. Low.

## II.7 Kernel build benchmark — Kernbench and a concrete local plan

**Kernel-space.** This has a completely different shape from everything else in
the survey: heavy **fork/exec churn** (thousands of short-lived `cc1` processes),
page-cache and `mmap` pressure, dcache traffic for header lookups, and make's
own **jobserver pipe** as the only cross-process synchronization point.

**Prior measurement, and why it's negative** (`ivh_benchmark_search_2026-07-20.md`):
vanilla linux-6.6, tinyconfig, timing a clean `make vmlinux`:

| `-j` | off avg | on avg | delta |
|---|---|---|---|
| 16 | 14.61s | 16.33s | **-11.8%** |
| 32 | 14.40s | 14.91s | -3.5% |
| 8 | 16.52s | 15.53s | +6.0% (1 round, not trusted) |

The explanation given there is sound and matches the model: a parallel build is
dominated by **independent CPU-bound compiler processes**, not by shared-lock
contention. The jobserver pipe is cheap and touched once per job, not per
operation. So IVH migrations disrupt compiler cache/TLB locality **without a
lock-holder stall to fix** — the same character as ebizzy malloc-mode (-2%),
scaled up. The `-j8` hint of a win (undersubscribed ⇒ migrations have genuinely
idle cores to land on) is a single noisy round and should not be built on.

That doc also flagged a second problem: **tinyconfig is too small**. A ~15s
build is below this project's reliable-measurement floor, and it disables so
much of the kernel that the build doesn't exercise a representative file/header
mix.

### II.7.1 Recommended local plan

**Tree**: `/root/kernels/linux-6.14-stock` — a clean git repo with **no
`.config`**, per the task brief. Using this rather than
`/root/kernels/linux-6.17-vanilla` keeps the vanilla-6.17 tree free for its
actual role (the IVH-vs-vanilla kernel comparison, e.g. for the LEBench overhead
audit in I.4.6).

**Config choice: `defconfig`, not `tinyconfig` or `allnoconfig`.** Reasoning:
- `tinyconfig` is **already measured and already rejected** — ~15s, below the
  measurement floor, and unrepresentative.
- `allnoconfig` has the same problem and is even less representative.
- `allmodconfig`/`allyesconfig` would be long enough but take 20–40 minutes per
  build at `-j16`, making a paired 8-round design a multi-hour commitment, and
  they shift the workload toward link/modpost phases.
- **`defconfig`** is the standard public config every kernel developer uses, is
  fully reproducible from the tree alone (no local `.config` to drift), builds
  in roughly 4–8 minutes at `-j16` on a 16-vCPU box — comfortably above the
  measurement floor, short enough for 8 interleaved rounds in ~2 hours — and
  exercises a realistic file/header/driver mix.

**Critically: build out-of-tree with `O=`.** `ivh_benchmark_search_2026-07-20.md`
contains a prominent self-flagged mistake where a previous session ran
`make tinyconfig` inside a tree that had uncommitted work and destroyed its
`.config`, then `git checkout --`'d three files with unstaged modifications,
losing them unrecoverably. **`O=` writes every artifact, including `.config`,
into a separate directory and leaves the git tree bit-for-bit pristine.** Given
that history this is not optional hygiene.

### II.7.2 Exact commands

```bash
# One-time: create an out-of-tree build dir OUTSIDE the git repo.
BUILD=/root/kbuild-6.14-defconfig
mkdir -p "$BUILD"
make -C /root/kernels/linux-6.14-stock O="$BUILD" defconfig

# Verify the git tree was not touched (do this once, and after the first run):
git -C /root/kernels/linux-6.14-stock status --porcelain   # must be empty

# Per round (interleave arms to cancel drift, exactly as the other IVH tests do):
for r in 1 2 3 4 5 6 7 8; do
  for arm in 0 1; do
    make -C /root/kernels/linux-6.14-stock O="$BUILD" -j16 clean >/dev/null 2>&1
    echo 3 > /proc/sys/vm/drop_caches            # equalize page-cache state
    echo "$arm" > /proc/sys/kernel/ivh_universal_eligible
    /usr/bin/time -f "arm=$arm round=$r %e" \
      make -C /root/kernels/linux-6.14-stock O="$BUILD" -j16 vmlinux
  done
done
echo 0 > /proc/sys/kernel/ivh_universal_eligible  # restore
```

Notes on the recipe:
- `make clean` rather than a fresh `defconfig` each round keeps the config
  fixed and only rebuilds objects — the thing being timed.
- `drop_caches` matters more here than in any other benchmark in this survey:
  the second arm would otherwise read a fully warm header cache and win for
  reasons unrelated to IVH.
- **Do not use `make mrproper`** — it would delete the config in `$BUILD` and
  change what you are timing between rounds.
- Sweep `-j` at 8, 16, and 32. The `-j8` (undersubscribed) point is the only
  place the prior data hinted at a win, and it was never confirmed; confirming
  or killing it at 8 rounds is a cheap, genuinely open question.

### II.7.3 Kernbench specifically

Kernbench is a wrapper that runs a kernel build at `-j` = {1, N/2, N, 2N,
"optimal", "maximal"} and reports elapsed/user/system time. It adds convenience
and a standard reporting format but **no new mechanism** over the plan above,
and its fixed `-j` ladder is less useful here than a deliberate 8/16/32 sweep
with a paired interleaved design. **Use the plan in II.7.2 rather than
Kernbench**, unless a directly comparable published Kernbench number is wanted.

### II.7.4 Verdict

**Low priority as a place to find a win** — the mechanism argues against it and
the measurement agrees (-11.8%). **Medium priority as a safety check**: a kernel
build is the most universally recognized "normal multi-core Linux workload"
there is, and a credible IVH story needs to be able to say what it costs there.
An honest "-12% on a parallel kernel build, and here is exactly why" is a
stronger paper section than omitting it.

## II.8 Filebench, per personality

**Kernel-space.** Filebench drives configurable multithreaded file workloads via
its own `.f` workload model files. Given dbench's measured split (**-19.3%**
tmpfs / **+33.7%** ext4+fsync), the prediction for Filebench is sharp:

| personality | shape | priority |
|---|---|---|
| `varmail` | mail-server: create/append/fsync/delete in one directory, multithreaded | **Medium-High** — the closest analogue to the dbench `-F` configuration that won +34%, and it adds directory-metadata contention dbench doesn't have |
| `fileserver` | create/write/append/read/delete mix, many threads | **Medium** |
| `webserver` | mostly reads + a shared log append | **Medium** — the log append is a single hot `i_rwsem`, a nice concentrated target |
| `webproxy` | reads + creates in a shared dir | Medium |
| `oltp` | simulated DB I/O with async writes + log writer | Medium |
| `randomrw` | random read/write to a few large files | Low-Medium |
| `singlestreamread/write` | one thread | Unlikely — no contention |

Run all of these on **ext4 on `/dev/vda2`**, not tmpfs, for the reason dbench
demonstrated. Thread counts at and above 16.

## II.9 Confirmed results carried forward (kernel-space)

Restating the measured numbers from `ivh_benchmark_search_2026-07-20.md` so this
document is self-contained. These are the only kernel-space entries here with
real data.

| benchmark | config | metric | off | on | delta | rounds |
|---|---|---|---|---|---|---|
| **ebizzy (mmap)** | `-S 12 -t 16 -m` | records/s | 6,420 | 15,166 | **+136%** | 4 |
| **dbench (fsync, ext4)** | `-F -t 12`, 16 clients | MB/s | 575.3 | 768.9 | **+33.7%** | 3 |
| **hackbench** | `-g 4 -l 30000` | seconds | 21.69 | 15.48 | **+28.6%** | 3 |
| **dbench (fsync, ext4)** | `-F -t 12`, 8 clients | MB/s | 468.2 | 563.0 | **+20.2%** | 4 |
| hackbench | `-g 8 -l 15000` | seconds | 17.34 | 16.48 | +4.9% | 3 |
| ebizzy (malloc) | `-S 12 -t 16` | records/s | 969k | 949k | **-2%** | 2 |
| hackbench | `-g 20 -l 8000` | seconds | 15.20 | 16.98 | **-11.7%** | 3 |
| dbench (tmpfs) | `-t 12`, 16 clients | MB/s | 13,444 | 10,855 | **-19.3%** | 2 |
| kernel build | 6.6 tinyconfig `-j16` | seconds | 14.61 | 16.33 | **-11.8%** | 3 |

Plus, from the brief: `hackbench -T -g1 -f8 -l400000` **+73.9%**.

The within-benchmark sign flips (ebizzy mmap vs malloc; dbench fsync vs tmpfs;
hackbench `-g4` vs `-g20`) are the most valuable thing in this table — each is a
single-variable confirmation of the model, and each tells you which knob to
reach for on a new candidate.

---

# PART III — RECOMMENDED ORDER OF WORK

Ordered by (expected signal) ÷ (engineering hours), not by priority label alone.

**Tier A — days, no new code beyond sysctls.** Everything in Part II with a
High/Medium-High rank and no build problem: will-it-scale `page_fault*`,
`lock1/2`, `mmap1/2`; fxmark's `MRPH`/`MRPM`/`MWCM`/`MWUM` ladder on ext4;
`perf bench sched messaging` and `futex lock-pi` (both already in this tree).
All zero-source-change.

**Tier A′ — locktorture, gated on one config check.** `CONFIG_LOCK_TORTURE_TEST`
is **not set** in this tree's `.config` (II.4), so this may need a kernel
rebuild rather than a `modprobe`. Check first; if it needs a rebuild, fold it
into the next kernel build rather than treating it as a blocker. It is worth the
trouble because it is the **only** way to sweep the CS-length valley on *kernel*
locks (via the hold-time parameters) — something this project has never done,
and which §1(b) flags as resting on a single provisional NHextend measurement.

**Tier B — the cheap userspace ports.** (1) sysbench `mutex` — ~20-line diff,
gives an independent CS-length sweep. (2) will-it-scale `pthread_mutex1-5`
rebuilt against `ivh_adaptive_futex_lock.h` — a controlled AFL-vs-glibc
comparison with an existing scaling harness. (3) The **SPLASH m4 macro layer** —
one file, fourteen benchmarks, and it structurally enforces the `extend()`
ordering that constraint 1 says is the easiest thing to get wrong.

**Tier C — the two best single-lock targets.** vips's `allocate_lock` (one
field, one call site, NHextend topology, already known to lose under migration)
and memcached's `item_lock()`/`item_unlock()` (one function pair, threaded so
PRIVATE futex is fine). Do the contention-*volume* check on vips before the port.

**Tier D — the shared-futex variant, then PostgreSQL.** Constraint 3 blocks
PostgreSQL, Apache, nginx and exim simultaneously. Building and validating a
non-PRIVATE AFL variant is one piece of work that unblocks all four, and
PostgreSQL's `LWLockAcquire`/`s_lock` is the best mechanistic LHP story in the
whole survey. Schedule it deliberately rather than discovering it mid-port.

**Tier E — the staleness-vs-fixed-spin comparison.** LevelDB `db_bench` first
(small, no dependencies), then RocksDB's `WriteThread::AwaitState()`, then
InnoDB if the first two work. This is the line of work with the most *novel*
result in it: every one of those systems already ships an adaptive spin with a
fixed budget, and IVH's claim is specifically that a TSC-heartbeat staleness
signal beats a fixed budget under host preemption.

**Tier F — negative controls and honesty checks, run alongside everything.**
blackscholes and canneal (zero-lock and lock-free, both measured -2.8%),
swaptions (+8.4%, embarrassingly parallel), ebizzy malloc mode (-2%), dbench on
tmpfs (-19.3%), the defconfig kernel build (expected ~-12%), and LEBench as a
pure instrumentation-overhead audit. A survey that only reports the winners is
not usable evidence.

---

# PART IV — OPEN QUESTIONS THIS SURVEY SURFACED

0. **(Highest priority, and cheap to settle.) `ivh_pre_lock()` hooks only the
   four `_raw_spin_lock*` functions — so the +136% ebizzy result and the +33.7%
   dbench result, both rwsem-bottlenecked, cannot have come from it.** See
   II.A. If the scheduler-tick path is doing the work in this project's two
   largest confirmed wins, that is a significant re-attribution, and it is
   settled by one kprobe run on `bpf_sched_pre_lock_migrate()` during an
   `ebizzy -S 12 -t 16 -m` run — the same technique
   `ivh_adaptive_spinning_glock13_findings_2026-09-03.md` §7 already used.
   **Do this before any benchmark work**: it costs minutes and it decides which
   half of Part II is even testing the mechanism you think it is.

1. **The AFL has no condition-variable equivalent.** This disqualifies, by
   construction, every bounded-queue/producer-consumer candidate (dedup, ferret,
   x264, pbzip2, LevelDB's writer queue, RocksDB's write group) — not because
   they don't contend, but because their *blocking* is condvar-shaped and the
   AFL only replaces the mutex. This is the single most consequential structural
   finding here and it was not previously written down in these terms. Whether
   an `ivh_afl_cond` is worth building is a real design question.

2. **Barriers are untouched.** streamcluster, fft, ocean_*, lu_*, freqmine and
   much of SPLASH are barrier-dominated. Barriers are arguably the *strongest*
   LHP amplifier (one laggard stalls N-1) and IVH currently addresses them only
   incidentally, via migration. An `ivh_afl_barrier` (spin, check outstanding
   participants' heartbeats, futex-wait on staleness) is a natural extension —
   but note glibc's `pthread_barrier_wait` already does spin-then-futex, so the
   novel part is only the staleness signal, same as with RocksDB.

3. **`FUTEX_*_PRIVATE` makes the AFL single-process only** — a verified fact
   from `ivh_adaptive_futex_lock.h:584/625/635/657`, and a hard blocker for four
   named candidates. Cost of lifting it is unmeasured.

4. **The 192-byte, three-cacheline `ivh_afl_lock`** makes lock-*array* workloads
   (fluidanimate, water_nsquared, barnes's `ALOCK`, memcached's `item_locks[]`,
   InnoDB's block mutexes) structurally awkward. A footprint-neutral variant
   exists as an option but the struct's own comment explains why single-cacheline
   packing degrades the heartbeat.

5. **The CS-length valley has only ever been measured on NHextend**, at 3-round
   rigor in the 50–234µs region, and the 2026-09-11 doc explicitly flags those
   numbers as provisional. sysbench `mutex --mutex-loops` (userspace) and
   locktorture's hold-time parameters (kernel) are the two cheapest independent
   ways to check whether the valley is a property of LHP or a property of
   NHextend. **This should probably happen before any large port.**

5b. **The AFL's "no valley" property rests on two points, not a sweep.** ~18–20%
   at ~13µs and ~18–20% at ~1.6ms — nothing in between, and in particular
   nothing in the 50–234µs band where migration is worst. Since the entire
   argument for porting the AFL into migration-losing workloads (vips, and by
   extension the mid-CS candidates generally) depends on that flatness, this is
   the **single highest-value cheap measurement in the whole survey** after
   open question 0. Same instrument as item 5, same run.

6. **Every PARSEC regression number in circulation came from the whole-system
   toggle**, and the paired isolated design turned both dedup and vips from large
   losses into neutral. Any candidate re-tested from this survey must use the
   paired design, or it will reproduce a methodology artifact and call it a
   result.

---

## Appendix: count of individual benchmarks evaluated

| group | count |
|---|---|
| PARSEC (individually) | 13 |
| SPLASH-2/2x/3 (individually) | 14 |
| will-it-scale (individually assessed) | ~25 |
| fxmark workloads (individually) | 15 |
| stress-ng stressors (individually) | 17 |
| locktorture torture types (individually, verified in-tree) | 14 |
| perf bench subcommands (individually, verified in-tree) | 18 |
| MOSBench components (individually) | 6 |
| Filebench personalities (individually) | 7 |
| Databases / servers (individually) | 9 |
| Other userspace (sysbench ×2, Phoenix++ ×8, Metis, pbzip2, SPEC ×2, LEBench, TPC-C) | 16 |
| Kernel build (Kernbench + local defconfig plan) | 2 |
| Confirmed prior results carried forward (ebizzy ×2, dbench ×2, hackbench ×3) | 7 |

**Total: ~160 individually evaluated benchmarks/workloads/stressors**, of which
**11 have real measured numbers from this project** (ebizzy mmap/malloc, dbench
fsync/tmpfs, hackbench at three group counts, swaptions, blackscholes, canneal,
dedup, vips, fluidanimate, pbzip2, redis, kernel build — several with both a
whole-system and a paired-isolated number that disagree, which is itself one of
the more important findings carried into this survey).
