# Measuring spin time without a kernel rebuild, 2026-09-29

## The method that was WRONG

    spin = ivh_slowpath_wait_ns - (ivh_node_halt_cycles.TOTAL + ivh_head_halt_cycles.TOTAL)/2.2

Two independent defects make this invalid:

1. **Gating mismatch.** `ivh_slowpath_wait_begin()` (`qspinlock.c:61`) returns 0 when
   `!ivh_slowpath_wait_measure || in_interrupt()`, so the wall counter excludes
   interrupt context and excludes everything while measurement is off. The halt
   sites -- `ivh_node_halt_record` called at `qspinlock_paravirt.h:2257`, and the
   head block at `:3815` -- carry NO such gate and accumulate on every `pv_wait`.
   The subtrahend therefore contains time the minuend never saw.
2. **Clock mismatch.** The wall counter uses `sched_clock()` (ns, cyc2ns-scaled,
   with `CONFIG_HAVE_UNSTABLE_SCHED_CLOCK` set); the halt sites use
   `ivh_raw_tsc()` (raw cycles). The `/2.2` reconciliation is an approximation.

**Proof rather than inference:** over the 243 runs of the finalcombo sweep the
ratio `halt_cyc/2.2 / wall_ns` spans 0.030 to **1.746**, and three rows exceed
1.0 -- i.e. implied spin time is NEGATIVE. Worst case `fsmark/mig/r1` at 1.746.

Every "spin time" and "on-CPU wait" figure derived this way is withdrawn.
NOT affected, because each comes from a single self-consistent source: `perf`,
`ivh_slowpath_wait_ns` (total wall wait), `ivh_slowpath_wait_events`,
wall-per-acquisition, and every mechanism fire counter.

## The method that WORKS: count iterations, never subtract

### Node spin is exactly counted

    node_spin_iters = ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum

`:2158` records the give-up path; `:1946` records the acquire-during-spin path.
The GLOCK-11 comment at `:1931` states the two pairs exist precisely so they can
be summed for a complete, unbiased total -- an earlier review caught the same
undercount this note is about.

**Verified on the live kernel:** with `tier1=0, tier2=0` the give-up population
must average exactly `ivh_pv_spin_threshold`. Measured 32,768.00 against a
threshold of 32,768. The counters behave as documented.

### Head spin is NOT counted, and cannot be from userspace

`:3589-3591`: a head that acquires mid-spin exits via `goto gotlock` and jumps
past the accounting block at `:3669`. There is no head equivalent of GLOCK-11.

Measured coverage on the live kernel:

| counter | value |
|---|---|
| `ivh_head_spin_enter` (one per tenure, `:3556`) | 84,914,673 |
| `ivh_head_spin_attempts` + `_bail_attempts` (counted) | 335,373 = **0.4%** |
| uncounted | 84,579,300 = **99.6%** |

### How long a head tenure actually spins (survival analysis)

Sweeping `ivh_pv_spin_threshold` makes the uncounted population observable: a
head exceeding the threshold exhausts and becomes counted. hackbench `-l60000`:

| threshold | head tenures | exhausted | % needing more |
|---|---|---|---|
| 64 | 892,374 | 347,135 | 38.90% |
| 256 | 729,796 | 152,695 | 20.92% |
| 1,024 | 1,036,218 | 15,359 | 1.48% |
| 4,096 | 1,000,535 | 5,991 | 0.60% |
| 32,768 | 815,168 | 2,532 | 0.31% |

~61% of head tenures acquire within 64 iterations, ~79% within 256, 98.5%
within 1,024. Integrating the curve gives a mean near **450 iterations**.
This is structurally sensible: the head waits for ONE critical section to end,
whereas a node waiter waits for every waiter ahead of it.

## Calibrating ns per iteration

`ivh_pv_spin_threshold = 1048576` (~37 ms tenure) drives halting to **exactly
zero**, so the wall counter becomes pure spin time with nothing to subtract:

| | |
|---|---|
| slowpath wall wait | 90.772 s |
| halt time / events | **0.000 s / 0** |
| counted iterations | 3,045,948,217 (all node-success) |
| **wall / counted iters** | **29.80 ns** |

29.80 is an UPPER bound: 1,816,942 head tenures contributed 0 counted
iterations, so the denominator is short. Folding in the survival-curve mean
(~450 iters/tenure, ~0.8e9 iterations) gives ~23.5 ns.

    ns/iter = 26 +/- 3

Consistent with the `~26 cycles per cpu_relax` source comment (11.8 ns at
2200 MHz) once the loop's `node->locked` read and its every-256th heartbeat
check are included -- roughly 50-65 cycles per iteration.

## Recommended practice

**For comparing arms -- use iterations directly, and no constant is needed.**
`node_spin_iters` is complete and exact, so the RATIO between two arms is exact
regardless of ns/iter. This is the defensible number for any A/B claim.

**For absolute seconds:**

    node_spin_seconds = (ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum) * 26e-9

carrying +/-13% on the constant.

**State the scope.** This is NODE spin only. Head spin is uninstrumented and is
roughly 20-27% of total spin by the survival estimate. Do not present node spin
as total spin.

**If head spin is required**, it needs a rebuild: add a success-path counter pair
beside `:3591`'s `goto gotlock`, mirroring `:1946`. That is the minimal patch.
Note /boot is at 90% and each kernel is ~64 MB.

## Reproducing

All read-only apart from `ivh_pv_spin_threshold`, which every procedure here
restores and verifies. Counters via
`python3 /root/ivh_tools/read_ivh_counters.py <names>` (reopens /proc/kcore per
sample -- a held-open fd returns frozen values).
