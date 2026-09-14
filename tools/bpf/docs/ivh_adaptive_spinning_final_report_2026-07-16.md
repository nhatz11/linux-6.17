# IVH + adaptive spinning: final report for this userspace workload

## Setup (unchanged across everything below)

Kernel `6.17.0-rseqport44+` (branch `kernel-43-clean`, `84f1e5fcc` + `CONFIG_HZ=1000`
+ BPF gate fixes). Host deliberately keeps vCPUs 0-7 stolen (~40-60%), 8-15 clean.
Sysctls: `ivh_capacity_threshold=1010`, `ivh_time_left_threshold_ns=4000000`,
`ivh_max_concurrent=8`, `ivh_time_left_source=1` (`last_active_time` formula),
`ivh_selection_trylock=0` (blocking lock) — the best-known combination from
tonight's sweep. BPF gates: `LOCKHOLDER=1, SPINNER=0, CAPACITY_LOW=1,
NOT_BETTER=1, PREEMPTED=1, BURST_ORDER=0, BURST_BUDGET=0`, `is_cpu_preempted()`
staleness threshold 6ms. `loop_spin=600000` (~1ms critical section),
`NHEXTEND_DURATION=5`, 16 threads, unpinned (`-n`).

## IVH alone: the real, reliable result

Across every test tonight (`NHextend2`, `NHextend3`, `NHextend`), IVH-alone
consistently delivers:

- **Iteration throughput: +11% to +18%** over no-opt, every round positive.
- **Wait time: -13% to -15%** (avg wait per acquisition).
- **Host-preempted-during-hold: 8-13% → 0.2-0.8%** — a >10x reduction, the
  mechanism doing exactly what it's designed to do.

This is the headline, publishable result: pre-lock migration reliably converts
most of a lock holder's host-steal exposure into a healthy-CPU acquisition,
with a real (not just theoretical) throughput and latency benefit.

## Adaptive spinning: real, but smaller than it first looked, and the recheck matters

Two designs were tested, both using the live KVM `steal_time.preempted` bit
(`/proc/vcap_preempted`) to detect when a lock holder's vCPU is currently
stolen, backing off in `tpause` slices (never leaving `TASK_RUNNING`, no
syscall, can't itself create a steal window) rather than hot-spinning:

1. **Backoff only** (`NHextend3.c`'s first version): waiter backs off, does
   nothing else. Modest, inconsistent benefit over IVH-alone in interleaved
   testing (Opus's rigorous re-test found the +1.2pp edge from an earlier,
   less-careful comparison was within the noise floor).
2. **Backoff + re-check** (ported from the original `NHextend.c`, now also in
   `NHextend3.c`): after backing off, the waiter re-triggers `ivh_cs_enter()`
   for *itself* before re-attempting the lock — the insight being that a
   `tpause` backoff can last long enough that the pre-wait migration check is
   stale, and the thread about to become the *next* holder deserves a fresh
   chance to land on a healthy CPU right before its critical section starts.

**The re-check is the mechanism that matters.** Without it, adaptive spinning
is a wash. With it, `NHextend3` (6 tightly-clustered rounds, no bimodal
instability) showed a consistent **+3 to +4 percentage points beyond
IVH-alone** — e.g. +17.7% vs IVH-alone's +13.9% in one matched comparison —
plus a further wait-time reduction on top of IVH-alone's own gain. Small, but
real and repeatable.

## The `is_cpu_preempted()` question — direct answer

Flipping `vsched_module`'s `preempted_src` (0=KVM steal bit, 1=`is_cpu_preempted()`
heartbeat) with the re-check mechanism present:

- `is_cpu_preempted()` generates **~2x the backoff volume** (tick-granularity
  false positives — a busy-but-healthy CPU reads "preempted" for most of every
  tick).
- Each of those extra backoffs now also risks an extra real migration syscall
  (the re-check), so the noise has a real cost: measured throughput fell
  *below no-opt* in one comparison.
- **Conclusion: the KVM steal bit is clearly the better signal once the
  re-check exists.** Before the re-check, the two signals performed the same
  (just different backoff counts); the re-check is what makes signal quality
  matter. If a kernel-side design needs `is_cpu_preempted()` specifically
  (e.g. it's what's already available without a new export), expect it to
  cost real throughput unless paired with something that filters out its
  tick-granularity false positives (e.g. requiring N consecutive stale reads,
  or widening the polling period — untested here).

## An important noise source, identified — read this before trusting any single short test

`vcap`'s capacity/steal data (`/proc/vcap_info`) updates in **discrete steps
roughly every 5-6 seconds** (its `-s 5000` sleep-then-profile duty cycle),
sitting completely static in between. A 5-second test run has close to a
coin-flip chance of landing entirely inside one stale window versus straddling
a refresh boundary. This produced a genuinely **bimodal** result on `NHextend.c`
(3 rounds clustered at ~2748 iterations, 3 rounds clustered at ~3971 — not
scatter around one mean, two distinct clusters) that a small sample could
easily read as "consistently 20%+" or "no better than no-opt" depending on
which few rounds you happened to catch. **Any single 4-6-round test at this
duration should be treated with real skepticism; the true picture only
emerged by running enough rounds to see both clusters.** This is very likely
why an earlier informal read of `NHextend.c`'s numbers suggested a bigger,
more consistent win than the fuller data supports — not a measurement error,
just an unlucky/lucky small sample relative to a real, external ~5-6s cycle.
Longer test durations (multiple `vcap` cycles per run) would average this out,
but conflict with the project's preferred 5s test duration — worth knowing
this tradeoff exists rather than assuming any short-run number is fully stable.

## Bottom line

- **IVH alone**: the reliable, reproducible, headline result (+11-18%
  iterations, -13-15% wait, >10x host-preempted reduction).
- **Adaptive spinning with the backoff-recheck**: a real, modest, repeatable
  addition (+3-4 additional percentage points, further wait-time reduction) —
  worth keeping, not worth overselling.
- **`is_cpu_preempted()` as the adaptive-spin signal**: worse than the KVM bit
  once the recheck exists; don't use it without a debounce.
- **Any short (5s) A/B result should be run in enough rounds to see past the
  ~5-6s `vcap`-cycle alignment effect** before being trusted as "the" number.
