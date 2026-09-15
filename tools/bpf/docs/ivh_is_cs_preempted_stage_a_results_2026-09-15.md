# `is_cs_preempted()` Stage A results, 2026-09-15: kill criterion fires on coverage

Kernel `6.17.0-G-LOCK-29-cspreempt+` (kernel commit `5c459d20d099`), MY_ivh_atc
with the loosened capacity gate (`931d6c933250`). Host co-runner on vCPUs 0-7
(half contention, confirmed from `ivh_uc_capacity` ~460 vs 1023). Plan:
`ivh_is_cs_preempted_build_plan_2026-09-14.md`. Tools: `/root/ivh_tools/
glock29_neutral.sh`, `cs_detect_run.sh`, `cs_stage_a.py`, `phase0b_dump.py`.

**Verdict: the detector works as designed, but the population it can see holds
~0.006% of vCPU time. Kill criterion (>= 1% recoverable) fires by ~160x. Stage B
was not run: no action on this detector can recover a measurable amount.**

## 1. Preflight and neutrality

- Tick period 2,200,000 cycles (HZ=1000, tsc_khz 2,200,000), `nohz_full` (null),
  `nohz=off`. No `#VE`, lockup, RCU-stall or WARNING lines at any point.
- Sysctl interlocks: `ivh_cs_head_probe=1` refused without `owner_enable`;
  `ivh_cs_head_bail=1` refused without probe; `ivh_pv_rot_enable=1` refused while
  probing.
- **Neutrality, all switches off**, IVH+AS hackbench `-T -g1 -f8 -l400000`,
  settled wait before each round: 12.283 12.306 12.326 12.424 12.323 12.328 s,
  mean **12.33 s**, vs G-LOCK-28 same gate and contention **12.28 s** (§9.1 of
  the drift doc): **+0.4%** across a reboot, within noise.

## 2. Smoke test (threshold 32768, 9 s hackbench each)

| check | A-gate (clear 0) | A-clr (clear 1) |
|---|---|---|
| partition 1 deviation | 0 | 0 |
| partition 2 deviation | 0 | 0 |
| avg head spin per exhaustion vs threshold | 32768.0 / 32768 | 32768.0 / 32768 |
| `abstain_nohz`, `abstain_rot`, `abstain_skew` | 0 / 0 / 0 | 0 / 0 / 0 |
| `long_hold` | 0 | 0 |

At the default threshold heads exhaust at ~45 us, so no hold ever reaches the
2-tick bar: expected.

## 3. Detect-only runs at `ivh_pv_spin_threshold = 16777216`, A-clr, ~62 s

| | migration ON (IVH+AS) | migration OFF (PV+AS) |
|---|---|---|
| hackbench rounds | 12.27 12.42 12.41 12.56 12.64 s | 62.99 s |
| check samples (every 256 iters) | 16,202,790 | 10,226,659 |
| **abstain_noprev** | **97.11%** | **97.72%** |
| abstain_tag / young | 1.54% / 1.35% | 1.45% / 0.63% |
| long_hold | 0 | 19,808 |
| healthy_long / fired | 0 / 0 | 3,798 / 16,008 |
| **distinct episodes** | **0** | **41 (0.64/s)** |
| episode mean (all ACQUIRED) | — | 1.54 ms |
| fires per episode | — | 390 |
| head halts | 0 | 0 |

Migration ON: at half contention migration keeps holders off the contended
vCPUs, so there is no long hold to detect and the raised threshold costs nothing.

Migration OFF, against the §5.7 kill criteria:

| item | criterion | measured | result |
|---|---|---|---|
| recoverable fraction | >= 1% of vCPU time | 41 x 1.54 ms / (64 s x 16) = **0.006%** | **FAIL (~160x short)** |
| episode rate | >= 100/s | 0.64/s | **FAIL** |
| episode vs halt round trip | median > ~14,166 cycles (6.4 us) | p50 ~2^21 cycles (~0.95 ms) | pass |
| K4 detection informative | detected tenure p50 >= 2x undetected | ~1.9 ms vs ~0.1 us | pass |
| K5 liveness term works | healthy_long / long_hold >= 0.05 | 0.19 | pass |
| K6 coverage | (tag+noprev) <= 0.9 x (checks - young) | 99.2% | **FIX, do not conclude** |

Over-count check: 390 sampled fires per distinct stall. Counting raw fires would
have overstated the opportunity ~400x, which is the handoff-rotation trap the
episode keying was built to avoid.

## 4. Why it is small: the design covers the wrong heads

Holder identity comes from `prev`, which exists only for a head that queued behind
someone. **97-98% of all head spin samples come from heads with no predecessor**:
under PV every contended arrival that finds an empty queue goes straight to head
(`pv_hybrid_queued_unfair_trylock()` breaks out when the tail is empty), and its
holder took the lock on the uncontended fastpath, which is never stamped.
`ivh_head_waiter_adaptive_spinning_design_2026-09-14.md` §4 called this role
"structurally the shortest wait"; the data says it is where nearly all head spin
time goes.

Ceilings, using the measured ~26.3 cycles per spin iteration, in the migration-OFF,
threshold-max run:

| population | vCPU time spent spinning |
|---|---|
| no-predecessor heads (invisible to this design) | ~30.6 vCPU-s = **~3.0%** |
| heads with a predecessor (all verdicts) | ~0.76 vCPU-s = 0.07% |
| detected long holds with silent holder | ~0.05 vCPU-s = 0.005% |

The 3.0% is an upper bound on what covering the no-predecessor heads could ever
recover, in the least favourable configuration (migration off, exhaustion
disabled). Most of it is healthy short waiting, not preemption.

## 5. Options

1. **Stop here** (plan §5.7 discipline). The detector is correct; its reachable
   opportunity is ~0.006%, and with migration on it is zero at half contention.
2. **Cover the no-predecessor heads.** Needs a holder stamp on the uncontended
   `queued_spin_lock()` fastpath, i.e. every spinlock acquisition in the kernel.
   The July 29 holder table (`include/linux/ivh_lock_holder.h`,
   `ivh_lock_holder_enabled`) already stamps `{lock, holder_cpu}` there but has no
   sysctl and no acquisition TSC. Before building it, measure the ceiling cheaply:
   add a per-tenure spin-duration histogram for no-prev heads (no identity needed)
   and check how much of the ~3% sits in tenures longer than 2 ticks.
3. **Full contention**, where migration cannot help, is the other place a larger
   opportunity could exist. Not measured here.

## 6. Machine state after the runs

All `ivh_cs_*` switches back to 0, `ivh_pv_spin_threshold` 32768, IVH+AS mode,
new-gate daemon running. No kernel warnings.

## 7. Re-run at full contention (co-runner on all 16 vCPUs), shorter

`cs_detect_run.sh`, A-clr, threshold 16777216, DUR 30 s (one hackbench round each).

| | migration ON | migration OFF |
|---|---|---|
| hackbench | 32.84 s | 55.12 s |
| head spin samples with no predecessor | 98.0% | 97.9% |
| distinct stalls caught | 2 (mean 0.51 ms) | 20 (mean 1.65 ms) |
| stall time / all vCPU time | ~0.0001% | 20 x 1.65 ms / (56 s x 16) = **~0.004%** |
| no-predecessor head spinning / all vCPU time | ~2.4% | ~2.9% |
| healthy_long / long_hold | 0.01 | 0.38 |

Same picture as half contention: the detector fires correctly, but the heads it
can see hold almost no spin time. Kill criterion still fires.

## 8. G-LOCK-30: fast-path owner stamp, and the replace-exhaustion A/B

Kernel `6.17.0-G-LOCK-30-csfast+` (kernel commit `9be8425a00d2`): new sysctl
`ivh_cs_owner_fast` stamps the per-CPU `{lock, tsc}` slot on the uncontended
acquire paths (A1/A2) and PV steals (A6); a head with no predecessor scans the
16 slots for its lock. Co-runner on all 16 vCPUs, migration ON, IVH+AS, new gate.
Neutrality with switches off was not re-measured on this boot.

Script `/root/ivh_tools/replace_exhaust.sh`, data `replace_exhaust_042212`. Order
D F X B B X F D, 2 rounds per arm, capacity-settled wait before each arm. The
host was much slower than earlier today (~70 s per round vs ~32 s).

| arm | what | rounds (s) | mean | vs D |
|---|---|---|---|---|
| D | threshold 32768, everything off (today's config) | 67.5 73.4 73.8 75.9 | 72.7 | — |
| F | threshold 32768, fast stamp on, no detection | 74.2 76.0 81.8 88.5 | 80.1 | **+10.3%** |
| X | threshold max, no replacement | 71.4 76.6 77.5 83.4 | 77.2 | +6.3% |
| B | threshold max, is_cs_preempted bail on every head | 91.5 82.6 74.2 80.3 | 82.1 | **+13.0%** |

(Lower is better. One block; within-arm spread ~5-10%, so treat single-digit
differences as indicative.)

B-arm counters (two arms):

| | arm 4 | arm 5 |
|---|---|---|
| no-predecessor lookups hit / miss | 5.55M / 14.26M | 4.60M / 12.70M |
| checks still abstaining on noprev | 68.9% | 70.3% |
| long holds (> 2 ticks) as share of checks | 12.3% | 11.8% |
| of those, holder still ticking (no fire) | **96.3%** | **94.9%** |
| bails -> head halts | 93,605 | 108,201 |
| bail halt duration mean / p50 | 102 us / ~32 cycles | 75 us / ~32 cycles |

D arms, for comparison: 41,679 and 43,561 exhaustion halts, mean ~720 us.

**Result: it does not replace exhaustion.**
1. **Stamping every uncontended lock costs ~10%** on this workload (F vs D).
2. **Coverage is still poor:** a single per-CPU slot is overwritten by nested
   acquisitions, so ~70% of no-predecessor checks still cannot find the holder.
3. **Most long waits are behind a holder that is running.** 95-96% of holds longer
   than 2 ticks had a holder that was still ticking (a slow critical section under
   heavy steal, or a holder itself waiting on an inner lock). A preemption detector
   correctly stays quiet there, but exhaustion used to cut those waits off. That is
   the part of exhaustion's job this design cannot do.
4. **When it fires, it is usually late:** most bail halts return almost immediately
   (p50 ~32 cycles). A holder that has just resumed from host preemption keeps a
   stale beat until its next tick, so the detector fires as the holder is about to
   release.

Machine state after the run: all `ivh_cs_*` off, threshold 32768, IVH+AS.
