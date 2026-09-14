# Lifting constraint 3: the adaptive lock now works across processes, 2026-09-14

`ivh_benchmark_survey_2026-09-13.md` called the PRIVATE-futex limitation "the
single biggest structural filter in this survey". This lifts it.

## 1. The bug, and why it was invisible

`ivh_adaptive_futex_lock.h` used `FUTEX_WAIT_PRIVATE` / `FUTEX_WAKE_PRIVATE` at
all four call sites. Those key the futex on `(mm, addr)`. Two processes mapping
the same shared page have different `mm`, so they derive **different keys** and
never see each other's waits or wakes.

Critically, this is **not** a correctness failure and will not show up as one:

- Mutual exclusion comes from the atomic cmpxchg on `l->state`, which works
  perfectly well in shared memory. The futex is only the blocking/waking path.
- Every `FUTEX_WAIT` carries `IVH_AFL_WAIT_TIMEOUT_NS` = **10ms**, documented in
  the header as "a correctness backstop... NOT a tuning knob for
  responsiveness". So a missed wake does not hang -- the waiter simply sleeps
  the full 10ms and retries.

So the lock keeps working, keeps passing any correctness test, and quietly adds
up to 10ms of latency to every block. Measured directly (`afl_mp_test`, 8
processes, lock in `mmap(MAP_SHARED|MAP_ANONYMOUS)`, 6s):

| | shared key (fixed) | private key (bug) |
|---|---|---|
| `FUTEX_WAIT` entered | 6,115 | 2,809 |
| ...ended in a real wake | **5,498 (90%)** | **0** |
| ...had to TIME OUT | 346 | **2,665 (95%)** |
| mutual exclusion | PASS | PASS |
| throughput | 954k acq/s | 1,022k acq/s |

**Zero cross-process wakes ever succeeded** under the private key, yet both arms
pass a mutual-exclusion test and the broken one is nominally *faster*. Nothing
short of instrumenting the wake return value would have caught this.

## 2. The fix

Per-lock `uint32_t shared` in the config cacheline (which had 16 spare bytes),
plus:

```c
static inline int ivh_afl_op(const struct ivh_afl_lock *l, int base)
{
        return l->shared ? base : (base | FUTEX_PRIVATE_FLAG);
}
```
`FUTEX_WAIT_PRIVATE` is literally `FUTEX_WAIT | FUTEX_PRIVATE_FLAG`, so the
choice is one OR. `l->shared` sits in the same cacheline as `l->wake_count` and
`l->stale_tsc`, both of which every one of these paths already touches, so the
read is free.

- `ivh_afl_init(l)` -- unchanged default, private.
- `ivh_afl_init_shared(l)` -- **new**; call for any lock in `MAP_SHARED`/shm.
  Must be called once by the creating process before others map the segment.
- `IVH_AFL_SHARED=1` -- validation-only env override forcing every lock shared,
  so the key cost can be measured on an existing single-process benchmark.

**Per-lock, not global, on purpose**: a process can legitimately hold private
per-thread locks AND a shared mutex in an mmap'd segment; paying the shared-key
cost on the private ones would be pure loss.

Only the futex ops were process-scoped. `extend()`/`unextend()` use rseq
(per-thread, fine per-process), `sys_ivh_cs_enter()` likewise, and the TSC
heartbeat is a plain `rdtsc` store into the shared page that already worked
across processes.

## 3. The shared key costs nothing

Fear was that `(inode, offset)` key derivation -- which runs
`get_user_pages_fast()` -- would land on the exact syscall the wake-skip
machinery exists to avoid. It does not. NHextend-full, same binary all three
arms so the CS-fix confound is fixed, 3 rounds x 8s, `-n`:

| loop_spin | ~CS | spin-only | private | shared | AFL benefit | shared vs private |
|---|---|---|---|---|---|---|
| 5,000 | 11us | 216,269 | 361,678 | 354,478 | +64% | **-2.0%** |
| 25,000 | 55us | 104,136 | 131,789 | 132,057 | +27% | **+0.2%** |
| 50,000 | 110us | 30,212 | 56,375 | 56,712 | +88% | **+0.6%** |
| 150,000 | 330us | 10,305 | 20,693 | 20,652 | +100% | **-0.2%** |
| 300,000 | 660us | 5,248 | 10,376 | 10,462 | +99% | **+0.8%** |
| 600,000 | 1.3ms | 1,791 | 4,881 | 4,922 | **+175%** | **+0.8%** |

Mean shared-vs-private over all six: **+0.03%**. Adaptive spinning's benefit is
preserved at every CS length studied, from 11us to 1.3ms.

**Caveat on this table**: NHextend is single-process, so with `IVH_AFL_SHARED=1`
the kernel's `get_futex_key()` takes the shared path but, for anonymous pages,
still resolves to an mm-based key after doing the page-pin work. So this
measures the extra key-derivation cost but not a genuinely inode-backed mapping.
It is a lower bound on the shared cost. The cross-process correctness proof in
§1 is the part that required `afl_mp_test`.

## 4. What this unblocks

Per the survey, one shared-futex variant unblocks all of these at once:
**PostgreSQL / pgbench** (process-per-backend, LWLocks in shared memory --
`LWLockAcquire()`/`LWLockRelease()` are single functions and ideal integration
points), **Apache prefork/worker MPM**, **nginx `ngx_shmtx`**, **MOSBench
exim**, and the `fork()`+`MAP_SHARED` PARSEC/SPLASH build. memcached was already
fine (threaded).

## 5. Files

- `ivh_adaptive_futex_lock.h` -- the fix.
- `afl_mp_test.c` -- cross-process proof. Build with `-DIVH_AFL_STATS`, since
  the discriminator is the wake return value, not mutual exclusion.
  `-p` forces the private key to demonstrate the failure mode.
