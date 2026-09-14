# Re-rank on the SPINLOCK criterion, 2026-09-14

**Supersedes `ivh_benchmark_reranking_2026-09-14.md`, which ranked on the wrong
criterion** (it scored "hot lock" generally and included pure `pthread_mutex`
candidates). It also corrects two claims in
`ivh_benchmark_survey_2026-09-13.md`.

## 0. The criterion

The thesis is lock-holder preemption in **spinlocks**. A mutex waiter blocks and
yields its vCPU: a preempted holder costs it latency, not burned cycles. LHP is
a spinlock pathology. And the kernel mechanisms are spinlock-only by
construction -- `ivh_pre_lock()` lives only on `_raw_spin_lock*` paths, never in
`mutex.c`, `rwsem.c`, `rtmutex.c`, `percpu-rwsem.c`.

The userspace AFL is itself spin-then-block, so swapping it in for a pure
sleeping mutex converts a mutex workload into a spinning one -- a different
claim than the thesis makes.

## 1. Do PARSEC/SPLASH use spinlocks?

### SPLASH -- NO. Definitively.

All **11** `*.m4` macro files across Splash-2 / Splash-3 / Splash-4 / PARSEC's
bundled `ext/splash2`, `ext/splash2x`, `pkgs/libs/parmacs` were enumerated.
`LOCK()` expands to `pthread_mutex_lock` in every multiprocessor variant; the
rest are uniprocessor no-ops (`{;}`) or the Graphite simulator
(`CarbonMutexLock`). `ALOCK`/`AULOCK` likewise. Zero `pthread_spin`, zero
`__sync_lock_test_and_set`, zero hand-rolled test-and-set across all 14 codes.

Two near-misses, neither a lock:
- `splash2x/apps/radiosity/src/taskman.C:117` -- a hand-rolled spin-wait
  *barrier*. Spins, but has no holder, so it is not LHP.
- Splash-4 replaces ~9 CSes with `do{...}while(!CAS(...))` retry loops. Lock
  free, no holder to preempt -- it deletes the thing under test.

**Consequence: all 14 SPLASH codes are dropped**, including the "one m4 file
covers everything" leverage argument from the previous doc. The leverage was
real; the criterion is not met.

### PARSEC -- YES, two sites, both enabled by default.

**(a) `dedup` -- a genuine `pthread_spinlock_t`.**
`pkgs/kernels/dedup/src/mbuffer.c:24` defines `ENABLE_SPIN_LOCKS`
**unconditionally**, so `PTHREAD_LOCK` (:30) is `pthread_spin_lock` over
`pthread_lock_t *locks`, an array of `NUMBER_OF_LOCKS`=1021 hashed by mcb
address. 11 call sites: `mbuffer_clone()` :140, `mbuffer_free()` :197,
`mbuffer_realloc()` :230-256, `mbuffer_split()` :280.
`netapps/netdedup/.../mbuffer.c` is identical.

**`mbuffer_realloc()` holds the raw spinlock across `realloc()`** -- a possible
`brk`/`mmap` inside the CS. Textbook LHP shape.

Honest caveat: the other three CSes are a single `m->mcb->i++` (tens of ns), and
1021 locks spread 16 threads thin, so the holder-preemption *rate* is low even
though the *shape* is ideal.

**(b) `streamcluster` and `fluidanimate` -- a hand-rolled unbounded spinlock.
This corrects the survey.**
`parsec_barrier.hpp:29` defines `ENABLE_SPIN_BARRIER`; `:24` defines
`ENABLE_AUTOMATIC_DROPIN`, which `#define pthread_barrier_wait(b)
parsec_barrier_wait(b)`. So *every* `pthread_barrier_wait` in these two apps is
really `parsec_barrier_wait()`, which contains -- twice, in both branches taken
by every non-last thread at every barrier:

```c
volatile spin_counter_t i=0;
while(barrier->is_arrival_phase && i<SPIN_COUNTER_MAX) i++;   /* ~0.1ms flag spin */
while((rv=pthread_mutex_trylock(&barrier->mutex)) == EBUSY);  /* unbounded, no pause, no yield */
```

The second line is a spinlock with no backoff, and the holder's CS contains
`pthread_cond_broadcast()` -- a `FUTEX_WAKE` **syscall held under the spinlock**.
Preempt that holder and all N-1 peers burn full timeslices in a tight trylock
loop.

**This overturns survey §I.1.7**, which said "streamcluster is sync-saturated,
but the sync is **barriers**, not mutexes -- see the entry for why that changes
which primitive applies." The barrier *is* a spinlock.

**No other PARSEC pthreads app spins**: bodytrack, raytrace, vips (GLib GMutex,
no spin phase), blackscholes, canneal, ferret, x264, swaptions, facesim,
freqmine -- zero `pthread_spin`, zero `__sync_*` spin loops, zero `_mm_pause`.

**TBB route**: only `fluidanimate/src/tbb.cpp` uses `tbb::spin_mutex`
(`atomic_backoff` -> `__TBB_Pause` then `__TBB_Yield`). Genuine, but the lock
array is `numCells x MUTEXES_PER_CELL` -- a 192-byte AFL is a footprint blowout,
and TBB spins internally in its task scheduler where you cannot cleanly bracket.
An observation target, not an AFL port.

## 2. Two corrections to the survey's hook coverage (§II.A)

Checked directly against this tree:

1. **There are THREE hooked call sites, not four.** `ivh_pre_lock(lock)` appears
   at `kernel/locking/spinlock.c:279` (`_raw_spin_lock`), `:298`
   (`_raw_spin_lock_irqsave`) and `:316` (`_raw_spin_lock_irq`).
   **`_raw_spin_lock_bh` is NOT hooked.** The survey's table lists it at `:831`
   with the others; that row is wrong.

2. **The hook self-gates far harder than "it is on the four spin_lock paths"
   suggests.** `ivh_pre_lock()` returns early unless ALL of:
   `bpf_sched_enabled()`, `ivh_universal_eligible`, `!current->ivh_exclude`,
   `in_task()`, `preemptible()`, `current->lock_depth == 0`,
   `__state == TASK_RUNNING`, and `ivh_eval_cooldown_ok()`.
   So it fires only for the **outermost** spinlock acquisition, in task context,
   while preemptible. Any nested acquisition is invisible to it.

**Good news on open question 0**: no `CONFIG_INLINE_SPIN_LOCK*` is set in this
build (only `CONFIG_UNINLINE_SPIN_UNLOCK=y`), so the out-of-line hooked versions
ARE the live ones and all three are `noinline`. The hook is reachable. Whether
it *fires* at a useful rate is still unmeasured -- `bpftool map dump name
last_migration` is the check.

## 3. Correction to the claim that ebizzy/dbench are not spinlock evidence

It was argued that ebizzy (+136%) and dbench (+34%) are `mmap_lock`/`i_rwsem`
bound, hence rwsems, hence never touch `ivh_pre_lock()`. **That is wrong, though
the underlying caution is right.**

`struct rw_semaphore` (`include/linux/rwsem.h`) contains
**`raw_spinlock_t wait_lock`**, and `kernel/locking/rwsem.c` takes it with
`raw_spin_lock_irq(&sem->wait_lock)` at :1021, :1037, :1079, :1131, :1145, :1189
-- which is `_raw_spin_lock_irq()`, a hooked entry point.

So rwsem-bound workloads DO reach the hook. The correct statement is narrower:
the contention those workloads *experience* is at the rwsem level (the
`count`/`owner` atomics plus the `osq` optimistic spin queue), while the hook
fires on the rwsem's **inner, briefly-held** `wait_lock` -- taken only on the
slowpath, guarding list manipulation. So ebizzy/dbench are weak spinlock
evidence, not zero evidence. Do not cite them as clean spinlock results, and do
not discard them either.

## 4. Re-ranked shortlist

| # | Candidate | The SPINLOCK | space | mechanism | source change |
|---|---|---|---|---|---|
| 1 | **Kernel-spinlock set**: will-it-scale `lock1`/`lock2`/`futex1-4`, hackbench, fxmark `MRPH`/`MRPM` | `blocked_lock_lock` (`fs/locks.c:170`), `ctx->flc_lock`; AF_UNIX `unix_state_lock`, `sk_receive_queue.lock`; futex `hb->lock` (`kernel/futex/futex.h:136`); dcache `d_lockref` | kernel | migration + tier1/tier2 | **none** |
| 2 | **PARSEC `dedup`** | `pthread_spin_lock` on `locks[1021]`, `mbuffer.c` | user | glibc-patch instrumentation | **none** (patched glibc already instruments `pthread_spin_lock`) |
| 3 | **PARSEC `streamcluster`/`fluidanimate`** | `while(pthread_mutex_trylock(&barrier->mutex)==EBUSY);`, `parsec_barrier.cpp` x2 | user | `extend()`/`sys_ivh_cs_enter()` | small |
| 4 | **PostgreSQL `pgbench -c 64`** | `s_lock()` -> `perform_spin_delay()`, `s_lock.c:98/126`; `MIN_DELAY_USEC`=1000 doubling to 1e6 | user, cross-process shm | staleness test in `perform_spin_delay()` | medium |
| 5 | **nginx shared-zone mutex** | `ngx_shmtx_lock()`, `ngx_shmtx.c:70` -- `ngx_cpu_pause()` to `spin`=2048, then `sem_wait` | user, cross-process | AFL (`init_shared`) | medium |
| 6 | **RocksDB / InnoDB** | RocksDB `WriteThread::AwaitState` (`write_thread.cc:64`, 200x `AsmVolatilePause`); InnoDB `TTASEventMutex::spin_and_try_lock()` | user | comparison baseline, not a port | n/a |

**Dropped for failing the criterion** (contention is a pure sleeping mutex):
all 14 SPLASH codes; libvips/`vips` (GLib `GMutex`); memcached
(`item_lock` is `pthread_mutex_t`); sysbench `mutex` (retained only as the
CS-length-sweep *instrument*, a different role); LevelDB (mutex + 2 condvars,
already blocked); PARSEC raytrace, bodytrack, facesim, x264, ferret.

## 5. Integration notes for the userspace survivors

**dedup** -- preferred path is **no swap at all**: run the stock pthreads binary
against the patched glibc, which already instruments `pthread_spin_lock/unlock/
trylock` with `cr_counter`, `wait_counter`, `last_cs_overall_ns`. The only
candidate needing zero application edits. If swapping anyway, the single macro
pair at `:26-38` covers all 11 sites. **Unsafe**: `mbuffer_realloc()` has
**three** exit paths (early `return -1` at :236 and :243, plus :256) -- all need
`unextend()`. `locks` is `malloc`'d once, never realloc'd, so address-as-futex-
identity is safe. 1021 x 192 B = 196 KB, footprint is a non-issue.

**streamcluster/fluidanimate** -- **do not swap the AFL in**: `barrier->cond` is
bound to `barrier->mutex` via `pthread_cond_wait` (:158, :189), and the AFL has
no condvar. Correct edit is `sys_ivh_cs_enter()` before
`pthread_mutex_lock(&barrier->mutex)` (:131) and `extend()`/`unextend()` around
each mutex-held region. **Unsafe**: the CS contains `pthread_cond_broadcast()`
(:172, :200) -- a syscall under the lock, so the CS can exceed
`IVH_AFL_STALE_NS`; and `parsec_barrier_wait()` has **six** unlock exit paths,
so pairing is not structurally guaranteed.

**PostgreSQL** -- staleness test inside `perform_spin_delay()` (:126), serving
both `SpinLockAcquire` and `LWLockWaitListLock()`. **Unsafe**:
`finish_spin_delay()` (:191-197) mutates the process-global `spins_per_delay`
(+-100/-1) -- touching it confounds the baseline.

**nginx** -- `ngx_shmtx_lock()`/`unlock()`; `ivh_afl_init_shared()` mandatory.
**Unsafe**: the lock word stores `ngx_pid` (:78, :91) and
`ngx_shmtx_force_unlock()` uses it for crashed-worker recovery -- the AFL has no
owner identity, so that is lost. `ngx_shmtx_trylock()` has live call sites and
the AFL has no trylock. Ensure `NGX_HAVE_ATOMIC_OPS` or the `:275` `fcntl`
fallback is what you measure. Keep the swap off `accept_mutex` (held across
`accept()`).

## 6. Sequencing

1. **Answer open question 0 empirically** -- run one kernel-spinlock workload
   and check `bpftool map dump name last_migration` for real events. Everything
   in row 1 depends on it and it is minutes of work.
2. **dedup**, because it needs zero source changes and has a real
   `pthread_spinlock_t` with a syscall inside one CS.
3. **streamcluster** -- the corrected finding, and a spinlock holding a
   `FUTEX_WAKE`.
4. Build `ivh_afl_cond` + `ivh_afl_trylock` (still the binding API gap) before
   nginx or any condvar-bound candidate.
5. PostgreSQL last -- highest value, highest lift.
