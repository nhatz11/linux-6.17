# IVH — final conclusion (2026-07-02)

## Verdict

**Proactive, pre-lock, guest-side thread migration is the wrong strategy for
preventing lock-holder preemption (LHP) on kernel spinlocks.** This isn't an
implementation-quality problem — every implementation bug found this session
was real and got fixed, and each fix that made the *mechanism cheaper*
helped; every fix that made the *decision more accurate* made results worse.
The strategy itself has a cost floor that exceeds what it can ever protect,
for the class of workload it has to operate on.

## The core arithmetic

- Kernel critical sections, measured live (kprobe on `current->last_cs_ns`,
  both `hackbench` and the project's own `NHextend` benchmark): **16ns–2µs**,
  mode 128–256ns.
- Cheapest possible correct cross-core thread relocation in this kernel,
  after rebuilding the mechanism around `stop_one_cpu()` (the same primitive
  the in-tree `sched_exec()` uses): **low microseconds** — two runqueue
  locks, one cross-core stopper-thread round trip, a cache-cold landing.

The relocation costs more than the entire event it exists to protect, by
1–2 orders of magnitude. Even a migration to a destination that stays
perfectly healthy forever is already a net loss on arrival. And because a
real hypervisor steal only recurs every few *milliseconds* while a CS lasts
a few hundred *nanoseconds*, the odds any single acquisition was ever
actually at risk are roughly 1-in-10,000 — so nearly every relocation
protects an event that was never in danger.

## The most important single data point

Iteratively made `process_cpu()`'s destination selection *more correct*
this session (fixed a threshold mismatch, a fake trylock, a tickless-idle
false-positive, inverted the tier preference to favor genuinely idle
destinations) — confirmed via instrumentation that selection really did
improve. Result: hackbench regression went from +2.7% to **+9.7% — worse**.
More accurate selection stopped getting vetoed and started reliably
executing the same structurally-losing trade, more often. The only
intervention that ever helped was cutting how often IVH fires at all
(a `last_cs_ns` prefilter, cutting eval volume 98.6%, brought hackbench to
+0.9% — neutral, not a win). **If the bottleneck were decision quality,
better decisions would have helped. They didn't.** The bottleneck is that
the mechanism runs at all, not what it decides when it runs.

## Why PLE / pv-spinlock work and this doesn't — not cost, strategy

PLE and pv-spinlock are **reactive**: they act on a realized, already-
happening symptom (real spinning, an actually-held lock past a threshold),
and neither one relocates a thread — PLE nudges host-level vCPU scheduling,
pv-spinlock parks a waiter and wakes it on unlock. Both have a 100% hit
rate by construction (they only fire once something is actually wrong) and
near-zero fixed cost on the common case.

IVH is **predictive**: it spends effort on every eligible lock acquisition,
trying to guess which ones are at risk before anything has happened. Given
the ~10⁻⁴ hit-rate above, it overwhelmingly pays its cost on acquisitions
that were never going to have a problem. This asymmetry is structural — it
does not shrink with better implementation.

**Important nuance:** this is not an argument that guest-only solutions are
impossible, or that host/hypervisor access is required. PLE and pv-spinlock
work because of *what they do* (react, don't predict; don't relocate
anything), not *where they run*. A guest-side, reactive, non-relocating
mechanism — e.g. a spin-waiter that checks whether the lock-holder's vCPU
looks unhealthy and backs off/parks instead of continuing to spin — is
still a legitimate, structurally different research direction, using the
same vcap/vact capacity signals already built. It just isn't IVH, and it
isn't a fix to IVH.

## Every workload tested, and what actually predicts the outcome

Not "mixed results" — reads cleanly once sorted by *confirmed engagement*
(how much IVH actually ran, not gate config):

| Workload | Result | Engagement confirmed? |
|---|---|---|
| `hackbench -T -g1 -f8 -l400000` | +0.9% (neutral) | Only after cutting engagement 98.6% |
| NHextend (`-n -l`, project's own long-CS benchmark) | **+25% worse, 6/6 rounds** | Yes — confirmed heavy, real engagement |
| dbench (`-t 8 4`) | ~neutral / mild latency improvement | Not measured |
| PARSEC (dedup, vips, canneal, streamcluster; 2 rounds each) | near-neutral, mixed | Not measured; sample size too small to trust either way |
| PARSEC raytrace | untested — native input exceeded 800s/run | — |

**Zero cases exist where confirmed-heavy IVH engagement correlates with a
win.** The one case with confirmed heavy engagement — NHextend, deliberately
chosen as IVH's best-case (long critical sections) — is the clear, decisive
loss. NHextend's real measured CS length turned out to be 1-2µs, not the
~100µs the "shift to long-CS validation" plan had assumed (that number was
never actually measured before this session — it was aspirational). Kernel
spinlocks are, by design contract, meant to guard only short critical
sections; code that holds one long enough for proactive migration to pay
for itself is, by convention, either a bug or a rare pathological exception
— which is why a genuinely favorable workload was hard to find on purpose,
not for lack of searching.

## Real, valid engineering work done this session (not wasted)

Five genuine kernel bugs found and fixed, independent of whether the
overall strategy works:
1. `is_cpu_preempted()` threshold mismatch between kernel-side (`cputime.c`,
   1.5ms) and BPF-side (`MY_ivh_atc.bpf.c`, was 300µs) despite a comment
   claiming they matched.
2. `my_spinlock` in `bpf_sched_pre_lock_migrate()` was a blocking
   `raw_spin_lock_irqsave()` despite its own comment describing trylock
   behavior — fixed to a real `raw_spin_trylock_irqsave()`.
3. Migration mechanism replaced: `set_cpus_allowed_ptr()` (general-purpose
   API, `set_affinity_pending` bookkeeping) + a redundant second
   `schedule()`, replaced with a direct `stop_one_cpu()` dispatch via new
   `ivh_migrate_self()` in `kernel/sched/core.c`, modeled on the existing
   in-tree `sched_exec()`.
4. `clock_preempt` (the heartbeat field `is_cpu_preempted()` reads) only
   refreshes on an active scheduler tick — any tickless-idle CPU
   (`NO_HZ_IDLE`, default) stops refreshing it and gets misread as "stolen."
   Fixed both implementations to use `max(clock_preempt, last_idle_tp)`.
5. `ivh_trylock_misses` counter was declared, exported, and printed in
   `/proc/ivh_debug` but never actually incremented anywhere — wired up,
   revealed ~1:1 trylock-miss-to-migration ratio under real load.

The methodology also matured in ways worth keeping for any future kernel
scheduling work on this VM: co-tenant contention drifts significantly even
within single ~90s test batches, so only interleaved paired sampling
(alternating baseline/treatment runs) produces trustworthy deltas — every
unpaired comparison this project ever ran was later shown to be unreliable.

## Later same-day work: new design direction (see `post_acquisition_reactive_migration_2026-07-02.md` for full detail)

After the verdict above, the session continued into designing a genuinely
different mechanism rather than closing out. Summary — **read the other
doc for the actual content, this is a pointer, not a substitute**:

- **PARM (Post-Acquisition Reactive Migration):** migrate a thread only
  *after* it's confirmed holding a lock, not before — eliminates the
  "will this thread ever hold a lock" guesswork that doomed pre-lock IVH.
- **Ruled out for kernel locks specifically**, via a real correctness
  review: migrating a confirmed kernel lock-holder mid-critical-section can
  silently corrupt per-CPU data the critical section body assumes is
  stable (`this_cpu_ptr()` invariant, actively checked elsewhere in the
  kernel via `__this_cpu_preempt_check()`). This does NOT trip the
  "scheduling while atomic" bug check — it's legal but not correct.
- **Remains viable for userspace-held locks** (pthread mutex/futex) —
  no kernel per-CPU invariant applies there.
- **Professor feedback corrected the expected-value math:** the flat
  `C_LHP ≈ 1ms` model this whole verdict is based on undercounts the true
  cost under real contention — every other thread waiting on the same
  lock also pays for a stall, not just the holder. With real contention
  (~10 waiters, ~60ms aggregate cost), the trade becomes narrowly
  *positive* (~+1000ns), but only when gated on lock *hotness*
  (waiter count) — an uncontended lock's math stays negative regardless.
  This is why "Hotlock" (a waiter-count gate) is not optional polish, it's
  the precondition for PARM being worth building at all.
- **qspinlock/MCS exposes no O(1) waiter count today** — verified against
  the real structs. Would need new state (a side-table, `pv_hash`-shaped)
  to build Hotlock for kernel locks; a self-contained atomic counter shim
  is the practical path for userspace locks (glibc doesn't expose a
  usable waiter count either).
- **Recommended scope given all of the above:** capacity/time-left signal
  cleanup → pthread waiter counter → PARM scoped to userspace locks only.
  Kernel-qspinlock PARM and MCS-position patching are cut to future work,
  not because they're unbuildable in the available time, but because they
  can't be made *correct* in it.

## Later same-day work: asymmetric-contention NHextend re-test

Motivation: a running theory all session was that migration destinations
picked by capacity alone don't stay good for long once the whole VM is
uniformly contended (the "residency window" problem) — so the natural
next test was an environment with a genuinely *stable* pool of healthy
destination vCPUs, to see if IVH does better when that specific problem is
controlled for.

**Setup process, worth keeping for any future controlled-contention test
on this host:**
1. Initial attempt — reduced the co-tenant sysbench VM (`bench-18c`) from
   16 threads to 8 (`taskset -c 0-7 sysbench cpu --threads=8 --time=0
   run`, confining sysbench to `bench-18c`'s own vCPUs 0-7) — did **not**
   produce a clean split. Live `bpftrace` capacity sampling (kprobe on
   `update_rq_clock`, same method used throughout this session) showed
   every one of this VM's (`prototype`) 16 vCPUs fluctuating together,
   with no stable healthy/unhealthy grouping, and which specific vCPU
   looked "healthiest" changed between readings taken seconds apart.
2. **Root cause, found via `virsh vcpupin`:** `bench-18c` has each vCPU
   pinned 1:1 to a specific host physical core (vCPU 0→host core 0, ...,
   vCPU 7→host core 7, vCPU 8→host core 8, vCPU 9→host core 36, ...,
   vCPU 15→host core 42). But `prototype` (this VM) had **every vCPU
   pinned to the same shared 16-core pool** (`0-8,36-42`, no per-vCPU
   restriction) — so any given `prototype` vCPU could land on a busy or
   free host core at any moment, smearing contention evenly across all 16
   instead of splitting it.
3. **Fix:** pinned `prototype`'s vCPUs 1:1, mirroring `bench-18c`'s own
   scheme exactly (`virsh vcpupin prototype <N> <same host core as
   bench-18c's vCPU N>` for N=0..15) — so `prototype`'s vCPUs 0-7 now
   directly compete with sysbench for host cores 0-7, and vCPUs 8-15 land
   on host cores sysbench never touches. This produced a genuinely stable
   split, confirmed via two consecutive `bpftrace` readings: cpu8-15 held
   at 1020-1022 both times (reliably healthy), cpu0-7 fluctuated but
   stayed contended both times (519-1009 range).

**Result, paired NHextend (`-n -l`), 10 rounds, under this confirmed
stable asymmetric split:**
- `total_wait`: 5/10 rounds better, avg delta **+0.12s (slightly worse)**
  — but removing the single largest-magnitude round **flips the sign** to
  -0.01s (essentially exactly flat). Same fragility pattern as the
  threshold-sweep result earlier this session — not an outlier-robust
  effect in either direction.
- `max_wait`: 4/10 better, 6/10 worse, avg **~+9.8µs worse** — a real, if
  noisy, lean toward IVH being worse on this specific tail-latency metric
  under this setup.
- `ivh_migrations_done` moved by 349 across the whole batch (~35/run) —
  real, non-trivial engagement, structurally correct (Gate 1 / capacity
  threshold naturally never fires for threads already on the healthy
  cpu8-15 set, since they're never below threshold — IVH only evaluates
  threads already on the contended set, as intended).

**Conclusion: even in the cleanest, most favorable environment
constructed all session — a real, confirmed, stable pool of healthy
migration destinations, directly controlling for the "residency window"
concern — NHextend still does not show a robust win.** It lands at
essentially neutral (total_wait) to mildly negative (max_wait), and the
total_wait result specifically fails the same outlier-robustness check
that invalidated the earlier threshold-sweep "improvement." This is a
meaningful negative result: it suggests the unstable-destination-pool
theory, while real and worth ruling out, was not the primary thing holding
pre-lock IVH back — consistent with, and reinforcing, the core arithmetic
verdict at the top of this document (migration cost vs. critical-section
length), which this experiment does not change.

## If picking this back up

Don't re-tune `process_cpu()`, gate combinations, or cooldown values on
**pre-lock** IVH — that space is exhausted and every result in it is now
explained, including under the most favorable contention setup that could
be constructed (see above). The open, legitimately different direction is
**PARM scoped to userspace-held locks, gated by a real waiter-count
signal** — see `post_acquisition_reactive_migration_2026-07-02.md` for
the full design, the correctness reasoning for why kernel-side PARM is
cut, and the recommended build order. Adaptive spinning (separately
designed by the user, not detailed in these docs) is the other standing,
architecturally-sound direction, complementary to PARM, not competing
with it.
