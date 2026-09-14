# Benchmark re-ranking after the shared-futex fix, 2026-09-14

Supersedes the priority column of `ivh_benchmark_survey_2026-09-13.md`.
Constraint 3 (PRIVATE futex => single-process only), which that survey called
"the single biggest structural filter", was lifted today -- see
`ivh_afl_shared_futex_2026-09-14.md`. This is the re-rank, with live source
checked for every row.

## 0. The headline: the binding constraint moved

With the futex fixed, what now disqualifies candidates is **the AFL's missing
API surface**, verified against `ivh_adaptive_futex_lock.h`:

| gap | present? | blocks |
|---|---|---|
| condition variable | **no** | LevelDB, RocksDB, dedup, ferret, x264, pbzip2, and PG's `LWLockAcquire` |
| reader/writer mode | **no** | `LWLockAcquire` (pgbench -S takes `ProcArrayLock` SHARED) |
| trylock | **no** | memcached `item_trylock()`, nginx `ngx_shmtx_trylock()` |

The whole public API is `init` / `init_shared` / `set_abort_flag` / `set_hooks`
/ `publish_heartbeat` / `beat` / `lock` / `unlock` / `shutdown_wake`. **Building
`ivh_afl_cond` + a trylock is now higher value than another port.**

## 1. Shortlist

| # | Candidate | The lock, and why the HOLDER gets descheduled while holding | AFL mode | Difficulty |
|---|---|---|---|---|
| 1 | **PostgreSQL `pgbench -S -c 64`** | NOT `LWLockAcquire`. The real victim is the `LW_FLAG_LOCKED` bit spun in `LWLockWaitListLock()` and every `SpinLockAcquire`, both via `perform_spin_delay()` -- whose first backoff sleep is `MIN_DELAY_USEC` = **1000us**, doubling to 1s. Holder descheduled => every backend burns `spins_per_delay` then sleeps >=1ms on a linked-list splice. `s_lock.c`'s own comment names the pathology. | **shared** | Med-High |
| 2 | **libvips / PARSEC `vips`** | `pool->allocate_lock`, one pool-wide `GMutex`, every worker, serialising the allocate callback. Already a measured *migration loser* -- the regime the AFL's flat-across-CS-length property targets. | private | Low-Med |
| 3 | **SPLASH-3 (`radiosity`, `raytrace`)** | One m4 file backs `LOCK()`/`UNLOCK()` for all 14 codes. radiosity: work stealing means contention rises exactly when host preemption occurs. raytrace: global ray-counter lock, NHextend's topology. | private | **Low** (one file) |
| 4 | **memcached `-t 16`** | `item_lock(hv)`/`item_unlock(hv)` in `thread.c` -- one function pair covering every call site. | private | Low-Med |
| 5 | **sysbench `mutex`** | Not an LHP story -- the *instrument* for settling whether the AFL really has no CS-length valley, on which rows 2, 3 and 6 all rest. | private | **Very Low** |
| 6 | **nginx shared-zone mutex** | `ngx_slab_pool_t.mutex` at the head of every shared zone: all 16 worker PROCESSES, every request. `ngx_shmtx_lock()` is exponential spin then `sem_wait` on a pshared semaphore -- **the AFL's exact architecture minus the staleness signal**, so the cleanest head-to-head available. | **shared** (exists only because of today's fix) | Med |
| 7 | **LevelDB** | Listed to record that it is BLOCKED -- by the missing condvar, not by constraint 3. | -- | blocked |

## 2. Demotions that today's fix does NOT void

Checked against live source, not assumed:

- **Apache httpd** -- stays Low. Constraint 3 was never the binding reason: the
  `mpm-accept` mutex engages only with multiple listening sockets, and the
  default event MPM removes it. No hot userspace lock remains.
- **MOSBench exim** -- stays kernel-only. Multi-process, but the bottlenecks are
  `mmap_lock`/`tasklist_lock`/dcache/fsync. No userspace mutex to port; the
  constraint-3 note was a red herring.
- **SPLASH builds** -- **were never blocked.** The survey hedged "*if* the build
  uses fork()+MAP_SHARED". Both Splash-3's `c.m4.null.POSIX` and Splash-4's
  `pthread.m4` use `pthread_create` + `valloc`/`malloc` with
  `pthread_mutex_init(..., NULL)`. Private futex was always correct.
- **nginx `accept_mutex`** -- stays Low (off by default). Row 6 is promoted via a
  *different* lock.

## 3. Two survey errors found

1. **sysbench `--mutex-loops` is documented and implemented as loops OUTSIDE the
   lock** -- the CS is literally `global_var++`. It cannot sweep CS length as
   shipped; the loop must be moved inside (~5 lines). Also needs
   `--mutex-num=1`; the default 4096 spreads contention to nothing.
2. **memcached's constraint-4 footprint is worse than estimated**: `-t 16` gives
   `item_lock_count = hashsize(15)` = **32,768 locks** = 6.3 MB at 192 B/lock,
   not 1.25 MB. There is no CLI knob (power is derived from thread count), so
   the default config is *designed* not to contend and must be patched down or
   it yields a null result for uninteresting reasons.

## 4. Integration points (user-level survivors)

**PostgreSQL** -- `src/backend/storage/lmgr/{s_lock.c,lwlock.c}`
- **Do NOT swap `LWLockAcquire`.** It is rwlock-mode and blocks in
  `PGSemaphoreLock()`, not a futex. Correct edit: add the staleness test inside
  `perform_spin_delay()` -- one function, used by both `LWLockWaitListLock()`
  and every `SpinLockAcquire`.
- **Heartbeat slot free of charge**: `union LWLockPadded` pads `LWLock` (~16 B)
  to `PG_CACHE_LINE_SIZE`, leaving ~48 unused bytes per lock. An 8-byte
  `hb_tsc` costs zero footprint -- dodges constraint 4 entirely.
- `extend()`/`unextend()`: bracket `LWLockWaitListLock()` ...
  `LWLockWaitListUnlock()` in `LWLockQueueSelf()`, `LWLockWakeup()`,
  `LWLockDequeueSelf()`. Pure proclist work, no syscalls, well under 50us.
- `sys_ivh_cs_enter()`: top of `LWLockAcquire()`.
- Hazards: `HOLD_INTERRUPTS()` internally; `MAX_SIMUL_LWLOCKS`=200 nesting;
  `finish_spin_delay()` mutates a GLOBAL `spins_per_delay` -- leave it alone or
  the baseline is confounded.

**libvips** -- `libvips/iofuncs/threadpool.c`
- Upstream: `vips__worker_lock(GMutex*)` is a single chokepoint; field is a value
  `GMutex allocate_lock`. PARSEC fork: **no wrapper, two unlock exit paths** in
  `vips_thread_work_unit()` (normal + `vips_thread_allocate()` error path) --
  both must be bracketed or `extend()` leaks.
- `ivh_afl_init()`; pool is `IM_NEW()`'d once, never copied -- address identity
  safe.
- Hazard: the CS runs `pool->allocate(...)`, an opaque callback that can do file
  I/O -- lock held across a syscall, can exceed `IVH_AFL_STALE_NS` (50us).
  Raise it or drive `ivh_afl_publish_heartbeat()` from the callback. Pin
  `VIPS_CONCURRENCY=16`: libvips resizes the pool from `pool->n_waiting`, which
  counts workers blocked on this very lock, so IVH changes thread count => confound.

**SPLASH-3** -- `codes/null_macros/c.m4.null.POSIX`
- Redefine four macros once, and constraint 1 is structurally satisfied across
  all 14 codes:
  `LOCK` -> `{ ivh_cs_enter_checked(); ivh_afl_lock(&($1)); extend(); }`,
  `UNLOCK` -> `{ unextend(); ivh_afl_unlock(&($1)); }`.
- `ivh_afl_init()` -- `CREATE` is `pthread_create`, `G_MALLOC` is `valloc()`.
- Hazards: `ALOCKDEC`/`ALOCK` are lock ARRAYS (barnes per-cell,
  water_nsquared `MolLock[]`) -- constraint 4 at 192 B/lock; swap `LOCK` first
  and leave `ALOCK` on pthreads. Audit anything that `memcpy`s a struct
  containing a `LOCKDEC`: the AFL's address IS its futex identity.
- **Do not use Splash-4** as the subject: its stated contribution is replacing
  locks with lock-free atomics, i.e. it deletes the thing under test. It is the
  ideal *paired control* against Splash-3. Stock SPLASH-2 has real data races
  that would be blamed on IVH.

**memcached** -- `thread.c`
- `item_lock(uint32_t hv)` / `item_unlock(uint32_t hv)`, one-liners over
  `static pthread_mutex_t *item_locks` (`calloc` in `memcached_thread_init()`,
  never realloc'd -- hash expansion does not resize it).
- `extend()`/`unextend()` inside those two functions => pairing structurally
  guaranteed. `sys_ivh_cs_enter()` at top of `item_lock()`.
- Hazards: `item_trylock()` returns the RAW `pthread_mutex_t *` and
  `item_trylock_unlock(void *)` unlocks by pointer -- **needs an AFL trylock,
  which does not exist**. `lru_locks[]` is taken NESTED under an item lock: two
  AFLs held at once is legal, but `extend()` must not be armed twice.

**sysbench** -- `src/tests/mutex/sb_mutex.c`
- `mutex_execute_event()`; `thread_locks[i].mutex`, `malloc`'d in
  `mutex_init()`, never realloc'd. `ivh_afl_init()`. Nothing unsafe -- but move
  the `nloops` barrier inside the lock first (see §3).

**nginx** -- `src/core/ngx_shmtx.c`, `src/core/ngx_slab.h`
- `ngx_shmtx_lock()`/`ngx_shmtx_unlock()`; lock word is `ngx_shmtx_sh_t lock` at
  the head of `ngx_slab_pool_t`.
- **`ivh_afl_init_shared()` mandatory** -- zone is `mmap(MAP_SHARED)` created by
  the master before fork, so workers have distinct `mm`.
- Hazards: the current protocol stores `ngx_pid` in the lock word and
  `ngx_shmtx_force_unlock()` lets the master recover a zone from a crashed
  worker -- **the AFL has no owner identity, so crash recovery is lost**
  (acceptable for a benchmark, not a deployment). `ngx_shmtx_trylock()` has call
  sites and needs an AFL equivalent. `accept_mutex` is held across `accept()` --
  scope the swap to the slab/zone mutex only.

**LevelDB -- blocked, do not port.** `db/db_impl.cc`: two condition variables
are bound to `mutex_` (`Writer::cv`, `background_work_finished_signal_`).
`CondVar::Wait()` needs atomic unlock-and-sleep, which `ivh_afl_unlock()` +
`FUTEX_WAIT` cannot provide. Also note `DBImpl::Write` already drops `mutex_`
around `log_->AddRecord()`/`Sync()`, so the fsync is OUTSIDE the CS -- a weaker
LHP target than the survey implied.

## 5. Sequencing

1. **Patched sysbench (row 5)** -- a few hours, and it either confirms the AFL
   is flat across CS length or overturns rows 2, 3 and 6. Rows 2/3/6 all rest on
   that property, which is currently inferred from two points 100x apart with
   nothing measured in the 50-234us band where migration is worst.
2. **Build `ivh_afl_cond` + `ivh_afl_trylock`** -- now the binding constraint,
   and worth more than another port.
3. Then SPLASH-3 (cheapest real port), then libvips, then nginx, then PG.

Survey open question 0 -- whether `ivh_pre_lock()` fires at all, given it hooks
only `_raw_spin_lock*` -- still gates every kernel-side attribution here and is
unaffected by today's change.
