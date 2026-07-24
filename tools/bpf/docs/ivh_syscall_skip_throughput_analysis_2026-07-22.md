# IVH syscall-skip optimization: throughput analysis

Date: 2026-07-22
Kernel: `6.17.0-rseqport55-trimsys+` (currently booted)
Sysctls: `ivh_capacity_threshold=1010`, `ivh_time_left_threshold_ns=4000000`,
`ivh_max_concurrent=8`, `ivh_time_left_source=1`, `ivh_selection_trylock=0`,
`ivh_migrate_mechanism=0`, `ivh_eval_cooldown_ns=50000`. IVH on = `ivh_universal_eligible=1`.

## Question

The syscall-skip optimization (`NHextend3_new`, using `RSEQ_SCHED_STATE_FLAG_IVH_DANGER`)
removes ~88% of `ivh_cs_enter` syscalls but did NOT improve throughput over the
original always-syscall binary. The user expected removing that many syscalls to
show up as an additional throughput win. Is there an easy fix, or is the
expectation miscalibrated?

## Verdict (up front)

**No throughput fix, because there is no throughput bug.** The pre-lock syscall is
*structurally off the throughput-critical path* in this benchmark, at every CS
length and contention level tested. Removing it cannot raise throughput. The
optimization does exactly what it should — it cuts syscalls and system-CPU time —
and this closed spin-loop benchmark cannot convert that saving into throughput by
construction. The staleness hypothesis does not hold up: the checked binary shows
**no** HP% penalty. The expectation was miscalibrated.

## Method: clean matched pair

The two shipped binaries are drifted (`/home/nick/NHextend3` is Jul 16, predates
the feature; `/home/nick/NHextend3_new` is Jul 22). To make the ONLY difference the
syscall-skip logic, both variants were compiled from the *current* source
(`/home/nick/kernels/linux-6.17-rseqport/NHextend3.c`, Jul 21) with one compile
guard added to `ivh_danger()`:

- `NH_checked`   — default build (skip when advisory danger bit clear).
- `NH_unchecked` — `-DIVH_FORCE_SYSCALL`, forces `ivh_danger()` to always return
  true → always syscall, reproducing the original pre-optimization behavior.

Build: `gcc -O2 -pthread -mwaitpkg [-DIVH_FORCE_SYSCALL]`. Both 35400 bytes,
identical to `NHextend3_new`'s flags. Source copy + binaries live in the session
scratchpad. All runs `NHEXTEND_DURATION=20 ... -n -l 16`, 3 interleaved rounds per
condition (interleaving averages out the background co-runner's steal drift).

Note: `/proc/ivh_debug` kernel counters (`ivh_prelock_calls` ~100k/run,
`ivh_migrations_done` ~4k/run) are **contaminated** — with `ivh_universal_eligible=1`
every task system-wide is IVH-eligible, so in-kernel lock paths (qspinlock,
mutex-spin) dominate those counters. The harness made only ~1800 syscalls/run.
Primary signals are therefore the harness's own stats (throughput, HP% via
`/proc/vcap_info` ground truth), which are clean.

## Experiment A — loop_spin=600000 (~1.5 ms CS), the regime the user asked about

| round | CHECKED ran | CHECKED HP% | UNCHECKED ran | UNCHECKED HP% |
|---|---|---|---|---|
| 1 | 15201 | 1.276% | 15281 | 1.728% |
| 2 | 15322 | 1.305% | 15121 | 1.574% |
| 3 | 15255 | 1.665% | 15611 | 1.390% |
| **avg** | **15259** | **1.42%** | **15338** | **1.56%** |

Checked skipped 87.2–87.8% of syscalls. Throughput difference between conditions:
**79 iters (0.5%)**, with unchecked nominally *higher* — the opposite of "skip
helps." Within-condition spread is up to 490 iters (3.2%), so the 79-iter gap is
deep in the noise. **No throughput effect.** Checked HP% is if anything *lower*
than unchecked — no staleness penalty.

## Experiment B — loop_spin=10000 (~25 µs CS), where syscall cost is proportionally huge

Here unchecked makes ~780k syscalls/20s vs checked ~52–90k (skip 88–90%).

| round | CHECKED ran | CHECKED HP% | UNCHECKED ran | UNCHECKED HP% |
|---|---|---|---|---|
| 1 | 776367 | 0.103% | 781684 | 0.138% |
| 2 | 772458 | 0.103% | 761966 | 0.129% |
| 3 | 773505 | 0.113% | 778737 | 0.091% |
| **avg** | **774110** | **0.106%** | **774129** | **0.119%** |

Throughput difference: **19 iters out of 774,000 = 0.002%.** Within-condition
spread up to 19,700. Even removing ~730,000 syscalls/20s produces **no** throughput
change. Priority #3's hypothesis (short CS would reveal the win) is **falsified**.

## Experiment C — low contention (-l 2, loop_spin=10000), skip ~99.7%

| round | CHECKED ran | UNCHECKED ran |
|---|---|---|
| 1 | 195765 | 196209 |
| 2 | 197768 | 198662 |
| avg | 196766 | 197435 |

Even at 99.7% skip and minimal contention, unchecked is nominally higher (0.3%,
noise). This is the decisive test: it rules out "contention from 16 threads is
masking the win."

## Why throughput cannot move — the structural reason

Throughput is bounded by the **serialized critical section** (one holder at a time).
Each thread's control flow is:

    ivh_cs_enter_checked()  →  spin-wait for lock  →  acquire  →  CS  →  release  →  loop

A waiting thread issues its pre-lock syscall **while another thread holds the
lock** — the syscall overlaps the current holder's CS and is never on the path that
gates when the lock next becomes free. With a contended lock there is always
another thread already spinning, ready to take it, whose syscall completed long
ago. Removing that syscall frees the waiter's CPU but does not hand the lock over
any sooner. This holds at -l 2 as well (A holds while B pre-issues its next
syscall), which is why even 99.7% skip shows nothing. It is **structural, not
CS-length-dependent** — a stronger statement than "wrong CS length."

## The arithmetic (priority #1), confirmed

At loop_spin=600000: ~13,264 syscalls saved / 20 s. Bare trap ≈ 193 ns (per
`ivh_state_of_the_art_2026-07-20.md` §3.3) → **2.56 ms aggregate saved**. Even in
the impossible worst case where 100% of that were on the critical path:
2.56 ms / 20,000 ms = **0.0128%** → ~2 iterations out of 15,080. The measured
round-to-round noise band is ~490 iterations (~3%). The theoretical maximum effect
is **~240× below the noise floor.** "No measurable improvement" is the arithmetically
correct outcome even before the structural argument.

## The optimization's *real*, measurable win: system-CPU time

`/usr/bin/time -v`, loop_spin=10000, -l 16 (high syscall volume):

| | CHECKED | UNCHECKED | delta |
|---|---|---|---|
| User time (s)   | 52.14 | 52.15 | ~0 |
| System time (s) | 6.87  | 7.42  | **-0.55 s (-7.4%)** |
| Total CPU (s)   | 59.01 | 59.57 | -0.56 s |
| %CPU            | 295%  | 297%  | - |

Removing ~730k syscalls cuts **system time by 0.55 s (7.4%)** — this is real and
consistent with ~750 ns/syscall for the full gated path. But **total CPU is
essentially unchanged**: the saved kernel time is reabsorbed as more user-space
spin-waiting, because the bottleneck is the serialized CS, not CPU availability.
In this closed spin-loop the freed cycles have nowhere to go but idle spin. In a
real deployment where those vCPU cycles could do other work (or be yielded to the
host), the ~7% sys-time / ~1% total-CPU reduction is the deployable benefit — but
it is a *CPU-efficiency* win, not a throughput win, and this benchmark cannot
express it as throughput.

## Staleness hypothesis (priority #2) — not supported

The advisory bit can be up to ~1 tick (~4 ms) stale, and the advisory gate
(`ivh_rq_capacity_and_timeleft_ok`, fair.c) additionally (a) omits the
`ivh_eval_cooldown_ok()` check the real syscall path has, and (b) reads a
possibly-stale `t->last_cs_ns` (the syscall path refreshes it from
`rseq->last_cs_overall_ns` first; the advisory path does not). Direction of these:

- Omitted cooldown → advisory gate says "danger" slightly *more* often than the
  real syscall would act → checked makes a few syscalls that then get
  cooldown-rejected. Harmless (skip% merely a bit lower than theoretical max).
- Stale-small `last_cs_ns` → advisory gate computes time_left slightly *larger* →
  could occasionally say "safe" when a fresh eval would say "danger" → a genuine
  potential *missed* migration.

The second is the real staleness risk. Empirically it does not bite: across all
three experiments the checked binary's HP% is **equal to or lower than** unchecked
(A: 1.42% vs 1.56%; B: 0.106% vs 0.119%). If staleness were causing missed danger
windows, checked HP% would be systematically *higher*. It is not. At these settings
`last_cs_ns` is stable enough (CS length is regular) and the tick refresh is
frequent enough relative to the migration-relevant timescale that the miss rate is
negligible. No fix warranted.

## Is there an easy fix? (priority #4)

**No.** Nothing found that costs throughput. The two code asymmetries above are
benign (one fails toward making a syscall = safe; the other has no measured HP
impact). No sysctl retune, conditional, or code change would produce a throughput
gain, because the gain does not exist to be unlocked — the syscall is off the
critical path.

The honest recalibration: the syscall-skip optimization should be claimed and
measured as a **system-CPU / syscall-rate reduction** (88–99.7% fewer syscalls,
~7% less sys time under high syscall volume), verified here to preserve the
mechanism's HP benefit and throughput exactly. It should **not** be expected to
raise throughput on this benchmark — that expectation is structurally miscalibrated,
not a bug.

## Housekeeping

`ivh_universal_eligible` restored to 0 after all comparisons.
