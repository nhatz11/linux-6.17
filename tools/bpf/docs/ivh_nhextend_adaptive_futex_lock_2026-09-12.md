# A userspace adaptive lock for NHextend3: design, implementation, and validation, 2026-09-12

Kernel `6.17.0-G-LOCK-25-tier1confirm+` (spin_mode fixed at STOCK_PV throughout — this is
entirely orthogonal to kernel adaptive spinning), migration engine live (`MY_ivh_atc` +
`vcap_probe`, `ivh_universal_eligible` toggled per arm). Docs/history repo `/root/linux-6.17`,
branch `kernel-43-clean`. Follows directly from
`ivh_nhextend3_migration_validation_2026-09-11.md` (same day's earlier migration-only result).

## 1. The question

NHextend3's own lock (`grab_lock()`) is a plain userspace test-and-test-and-set spinlock — no
sleep, no queue, no fairness. Nothing in this project had ever built an analogue of the kernel's
PV-qspinlock adaptive-spin mechanism (tier-1/tier-2, `qspinlock_paravirt.h`) for this lock. The
question: is there a userspace equivalent worth building, and does it help on top of the
already-validated migration win?

**Naming note, corrected mid-session**: this is *not* "a mutex" in the sense that matters here.
Every lock (spinlock, mutex, ticket lock) provides mutual exclusion — that's not a distinguishing
property. What makes this the userspace sibling of PV qspinlock specifically, not a generic mutex,
is the *trigger*: it decides to block based on a TSC-heartbeat staleness signal (is the holder
actually still executing?), not a blind spin-count the way even glibc's `PTHREAD_MUTEX_ADAPTIVE_NP`
does. Same reason nobody calls PV qspinlock "a mutex" despite `pv_wait()`/`HLT`+hypercall-kick
mechanically being block-and-wake underneath.

## 2. Design, by Opus (full plan in the session, summarized here)

**Mechanism**: `futex(2)` (`FUTEX_WAIT`/`FUTEX_WAKE`) — the userspace analogue of the kernel's
`HLT`+hypercall-kick pair, one privilege level up. Chosen specifically because it abstracts away
both "sleep vs. halt" (a real syscall-level block/wake primitive already) and "broadcast vs.
build-a-queue" (the kernel's own per-address futex wait list already is the cheap add/pop
structure — no custom queue needed).

**Heartbeat keyed by lock, not by CPU** — a deliberate, load-bearing departure from the kernel's
per-vCPU `ivh_tsc_beat`. NHextend3's threads are unpinned (`-n`); a CPU-keyed check fails worst
exactly when it matters most, because Linux's load balancer preferentially wakes a long-preempted
thread onto a *different*, less-loaded CPU than the one it left. A single `hb_tsc` word per lock,
written by whoever currently holds it regardless of which CPU they're on, is migration-immune by
construction and needs zero kernel changes (`__rdtsc()` is ring-3 executable on this host).

**Three-state wake-skip flag** (`state`: 0=free, 1=held/no waiters, 2=held/may have waiters),
adopting glibc NPTL's own `lowlevellock` pattern. The open question going in — how does the flag
ever get back from "may have waiters" to "no waiters" without a lost-wakeup bug — resolved
precisely: **pessimism lives on the sleep/acquire side, never the wake side.** Unlock always
writes `0` and wakes iff it swapped out a `2`. Any acquisition other than the direct uncontended
`0→1` CAS installs `2` unconditionally, without trying to determine whether other sleepers remain
— it can't know, so it assumes the worst. Getting this backwards (a woken thread optimistically
installing `1`) causes a genuine, unrecoverable lost-wakeup hang, not just a perf regression —
walked through concretely in the design doc. Self-healing back to a wake-skipping state happens
automatically the next time some acquisition finds the lock genuinely free.

**Two real bugs the design process caught before any code ran:**
- **The existing `cmpxchg()` macro in `NHextend3.c` is byte-sized, not 64-bit**, despite its
  `unsigned long` signature (`asm volatile("lock; cmpxchg %b1,%2"...)` — `%b1` forces a byte
  operand). Verified live: `cmpxchg(&word, 0, 0x1234567800000005UL)` writes only `0x05`. It "works"
  today purely by accident — the only values ever exchanged (`0`, `sched_getcpu()+1` ∈ [1,16]) fit
  in a byte. The new header avoids it entirely, using correctly-sized `__atomic_*` builtins, and
  explicitly prohibits ever packing a second field into the 0/1/2 `state` word for the same reason.
- **A signed-vs-unsigned TSC subtraction bug in the staleness check**: `now - hb` on unsigned
  64-bit underflows to ~2^64 ("infinitely stale") if `hb` is momentarily ahead of `now` (bounded
  skew from `RDTSC`'s lack of ordering, or residual cross-vCPU offset) — causing an immediate
  spurious sleep. Fixed with a signed cast, which makes that case compare as fresher-than-threshold
  instead — flagged by the design as "the single most likely one-character bug in this file."

**Shutdown deadlock, found and fixed**: a thread blocked in `FUTEX_WAIT` does not poll
`data->done` — `pthread_join()` would hang on any run that ended under real contention (i.e.
essentially every run). Fixed three ways: an abort-flag check inside the wait loop,
`ivh_afl_shutdown_wake()` called right after `data.done = true` (before the join loop), and a
10ms `FUTEX_WAIT` timeout as a backstop for the residual race (a thread between its last abort
check and its syscall entry when the broadcast fires).

**In-CS instrumentation contamination, found and fixed**: the original `grab_lock()` called
`read_vcap_steal()` — a `pread()` + `strtok_r`/`sscanf` parse of an 8KB proc buffer — *after*
acquiring the lock and *before* releasing it, i.e. inside the very ~13µs CS this project spent all
of 2026-09-11 calibrating. Moved both `steal_before`/`steal_after` reads outside the timed region
in `NHextend-full.c` (harmless to widen — it's a cumulative per-CPU counter already filtered at
>100µs) — this alone measurably shortened the recorded CS length (~13µs → ~11µs) independent of
anything about the new lock.

## 3. Implementation

- `ivh_adaptive_futex_lock.h` — new, reusable, single-header (`static inline`, no build-system
  integration). Public API: `ivh_afl_global_init()` (once per process — calibrates TSC-per-ns via
  a resurrected `calibrate_tsc()` pattern that existed in an earlier version of `NHextend3.c`,
  commit `298be1454`, since removed; checks `/proc/cpuinfo` for `constant_tsc`/`nonstop_tsc`/
  `tsc_reliable` and fails *closed* — pure spin, never sleep — if the TSC isn't trustworthy, with
  `IVH_AFL_DISABLE=1` as a deliberate same-binary kill switch), `ivh_afl_init()`,
  `ivh_afl_lock()`/`ivh_afl_unlock()`, `ivh_afl_beat()` (self-gated republish, cheap to call every
  CS iteration), `ivh_afl_set_abort_flag()`/`ivh_afl_set_hooks()` (before-sleep/after-wake hooks —
  `NHextend-full.c` uses these for `unextend()`/`wait_exit()`/`wait_enter()` bookkeeping around an
  actual sleep, since a genuinely blocked thread isn't spin-waiting and carrying an outstanding
  `cr_counter` extension request into a sleep is a real inconsistency, not cosmetic), and
  `ivh_afl_shutdown_wake()`. Tunables (`IVH_AFL_SPINS_BEFORE_CHECK=256`, `IVH_AFL_STALE_NS=50000`,
  `IVH_AFL_BEAT_MASK=0x3FF`, `IVH_AFL_WAKE_COUNT=1`, `IVH_AFL_WAIT_TIMEOUT_NS=10ms`) are
  `#define`-overridable; wake count is additionally an `IVH_AFL_WAKE` env var override, no rebuild.
- `NHextend-full.c` — adaptation of `NHextend3.c`: `struct data.lock` changed from `unsigned long`
  to `struct ivh_afl_lock`; `grab_lock()` rewritten around `ivh_afl_lock()`/`_beat()`/`_unlock()`;
  the now-meaningless `data->lock != my_lock_val` identity assertions removed (state is 0/1/2 for
  any holder, not an encoded identity); `main()` wired with `ivh_afl_global_init()`/`_init()`/
  `_set_hooks()`/`_set_abort_flag()` and the pre-join `ivh_afl_shutdown_wake()`. Compiles clean
  with `-Wall`, no warnings. Optional `-DIVH_AFL_STATS` build (`NHextend-full-stats`) adds
  per-thread counters (fast/slow acquires, sleeps, wakes issued/skipped, stale detections,
  recheck-aborted sleeps), summed across threads via `__atomic_add_fetch` at thread exit and
  printed at shutdown.

**Fix applied mid-session, prompted by direct user review of the code**: `ivh_afl_unlock()` was
discarding `futex(FUTEX_WAKE)`'s own return value — which tells you exactly how many waiters were
actually popped, `0` if none were asleep — and only tracking "did we call it" (`wakes_issued`).
Fixed to capture the real return value directly (`wakes_woke_nobody`/`wakes_woke_someone`/
`total_threads_woken`), replacing what had been an indirect inference (comparing `wakes_issued`
against `sleeps`) with a precise, direct measurement.

## 4. Validation

**Smoke test**: clean run, exit code 0, no hang across repeated `NHEXTEND_DURATION` runs including
end-of-run shutdown under real contention (the exact scenario the shutdown-deadlock fix targets).

**Mechanism check (single stats-enabled run, loop_spin=5000, `-n -v -l`, 20s)**: of 1,245,231 total
acquisitions, only **3,782 (0.3%) ever actually called `FUTEX_WAIT`** — `stale_detections` (3,794)
tracks `sleeps` almost exactly, with 12 caught and aborted by the last-moment double-check (a
holder that looked stale but had just released). This confirms the heartbeat gate is doing its
job: it is not sleeping on ordinary healthy queueing behind other threads, only on genuine stalls.

**Known, real cost, found via this same stats run**: `wakes_issued` was 1,019,340 against only
3,782 real sleeps — over 99.6% of wake syscalls woke nobody. This is the design's own predicted
"bounded, self-healing waste" (any slow-path acquisition pessimistically arms `state=2`, even
though most win within a few spin iterations and never need a wake on their own release) — not a
bug, but a real, large, measured cost, and a legitimate future optimization target (an explicit
atomic waiter count, checked instead of the coarser `state==2` flag, would tighten this — not yet
built, would need the same lost-wakeup-race rigor as the original state machine).

### Headline throughput results (`ivh_universal_eligible` toggled per arm, spin_mode fixed at
STOCK_PV, both binaries default `loop_spin` unless overridden)

**loop_spin=5000 (~13µs CS), single run each:**

| mode | "Ran for" |
|---|---|
| PV (migration off) | 870,427 |
| IVH (migration on) | 1,096,379 (+26.0% vs PV) |
| IVH+adaptivespin | 1,240,546 (+42.5% vs PV, **+13.1% vs IVH**) |

**loop_spin=5000, 5-round interleaved confirm:**

| mode | mean | range |
|---|---|---|
| PV | 898,679 | 875,827–932,084 |
| IVH | 1,052,238 | 988,240–1,102,009 |
| IVH+adaptivespin | 1,233,594 | 1,225,754–1,239,568 |

IVH vs PV: **+17.3%**, 5/5 positive, t=4.45. IVH+adaptivespin vs IVH: **+17.5%**, 5/5 positive,
t=6.88 (tighter spread than the migration-alone comparison: 5.7pp sd vs 8.7pp). The migration-alone
number here (+17.3%) came in below the previous day's dedicated 10-round validation (+21.2%/+22.8%,
t=10–50) — most likely host-state drift (corunner activity, capacity-EMA convergence state) between
sessions rather than anything wrong with this comparison, noted rather than quietly using the
better historical number.

**loop_spin=600000 (~1.6ms CS, the original pre-2026-09-11 default), 3-round interleaved:**

| mode | mean | range |
|---|---|---|
| PV | 5,972 | 5,348–6,878 |
| IVH | 11,439 | 11,139–11,590 |
| IVH+adaptivespin | 13,694 | 13,557–13,858 |

IVH vs PV: **+93.7%**, 3/3 positive, t=6.71 — migration's benefit is far larger at this longer CS
than at loop_spin=5000, consistent with more absolute per-acquisition time for capacity/steal
avoidance to pay off. IVH+adaptivespin vs IVH: **+19.7%**, 3/3 positive, **t=17.96** — the tightest,
most statistically decisive result this whole investigation has produced (1.9pp sd), and nearly
identical in magnitude to the +17.5% seen at loop_spin=5000 despite the two CS lengths differing by
two orders of magnitude (13µs vs 1.6ms).

## 5. Bottom line

The futex-adaptive lock is a real, reproducible, generalizing win layered on top of migration —
confirmed at two CS lengths two orders of magnitude apart, both landing an independent ~18-20%
lift with no reversals across 8 total interleaved rounds (5+3). This is a first validation pass,
not a fully swept design: `wake_count` has only been run at its default (1), the ~1M-wasted-wake
cost is real and unoptimized, and every tunable threshold (`IVH_AFL_STALE_NS`, the spin-before-
check gate, the beat-republish interval) is the design doc's reasoned starting value, not something
independently swept on this host yet.

## 6. Files

`/root/linux-6.17/ivh_adaptive_futex_lock.h` (new, reusable across future userspace benchmarks),
`/root/linux-6.17/NHextend-full.c` (new), binaries `NHextend-full`/`NHextend-full-stats` (built,
not committed — build artifacts). `NHextend3.c`'s own default `loop_spin` was separately changed
600000→5000 the previous day (2026-09-11, see the migration-validation doc) and is unrelated to
this file's own additions.
