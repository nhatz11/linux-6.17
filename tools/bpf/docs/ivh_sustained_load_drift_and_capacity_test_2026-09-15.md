# IVH+AS slows down under sustained load: finding and capacity test plan, 2026-09-15

**Status: finding measured, cause NOT confirmed.** Nothing in this doc has been
tested beyond the three runs in §1. §3 is the plan to find the cause.

## 1. What was measured

Kernel `6.17.0-G-LOCK-28-rotate+`, host sysbench co-runner on, workload
`hackbench -T -g1 -f8 -l400000`, consecutive rounds with no pause between them.
Script: `/root/ivh_tools/cross_kernel_baseline.sh`. Logs in `/root/ivh_tools/`.

| run | mode | idle before | per-round time (s) | mean | sd |
|---|---|---|---|---|---|
| A | IVH+AS | ~1.5 min | 29.99, 42.51, 42.66, 43.58, 44.35 | — | — |
| B | IVH+AS | ~2 min | 35.10, 37.90, 40.19, 41.60, 41.56, 46.64, 49.89, 50.17 | 42.88 | 5.51 |
| C | PV (migration off, `spin_mode 1`) | ~3 min | 53.69, 52.94, 53.46, 53.18, 53.48 | **53.35** | **0.29** |

Logs: run A = `cross_kernel_baseline_G-LOCK-28_run1_rampup.log`, run B =
`cross_kernel_baseline_G-LOCK-28_run2_8rounds.log`, run C =
`cross_kernel_baseline_6.17.0-G-LOCK-28-rotate+_pv.log`. Run A's round 1 is
likely co-runner ramp-up and is excluded. An earlier run with the co-runner off
(`..._NOCORUNNER_discard.log`, ~19.5 s/round) is not comparable.

Three facts:

1. **IVH+AS gets slower every round.** Run B: 35.1 s → 50.2 s, +43% in 8 rounds,
   close to monotone, no plateau.
2. **It recovers during idle.** Run A ended at 44.4 s; after ~2 min idle, run B
   started at 35.1 s. So the slowdown is a state that builds under load and
   decays when the machine idles, not a one-way trend.
3. **PV does not drift.** Run C is flat to 0.5% with the same co-runner and a
   similar idle gap before it.

Consequence: IVH+AS's advantage over PV shrinks from **~34% (round 1) to ~6%
(round 8)** within one run. IVH is still faster in every round, but **the size
of every IVH result depends on how long the machine has been under load.** That
includes the 23.1% three-arm exhaustion result and the 2026-09-14 benchmark
screen.

## 2. What PV-flat does and does not rule out

It rules out the **whole-VM** version of host scheduler credit: if the host
simply favoured this VM's vCPU threads after idle, PV would also have started
fast and slowed down. It didn't.

It does **not** rule out a **per-vCPU** version (H4 below). PV spreads work
evenly across vCPUs, so the host-side credit of each vCPU thread is at
equilibrium from the start. IVH migration concentrates work onto vCPUs it judges
"clean", which were lightly used and had built up host credit. Sustained
migration burns that credit down, and idle restores it. That would produce
exactly this pattern with no bug anywhere in IVH. Do not treat "it's IVH, not
the host" as settled until H4 is tested.

## 3. Hypotheses

| # | hypothesis | predicts | timescale check |
|---|---|---|---|
| **H1** | **In-kernel capacity estimate (`rq->ivh_uc_capacity`) sinks under load**, so fewer vCPUs pass the migration gate (`ivh_capacity_threshold` = 1010), so fewer migrations are accepted | `REJ_CAPACITY_LOW` rises and accepts fall round over round, in step with the slowdown | This is the source actually in use: `ivh_cap_source=3` and `ivh_cfg[0]=3` (set by `goto_mode.sh`). EMA: `ivh_uc_ema_alpha_q16=868` per `ivh_uc_window_ns=200ms` window = time constant ~15 s, half-life ~10.5 s (`kernel/sched/core.c:340-400`). **Too fast on its own** to explain a drift that builds over ~6 min, unless its input keeps worsening |
| H2 | vcap_probe's capacity (`rq->cpu_capacity`) sinks under load | same as H1, but through vcap | `goto_mode.sh` states a ~130 s half-life, which **matches** the timescale. But MY_ivh_atc does not read it with `ivh_cfg[0]=3`. It could still act through the regular CFS load balancer, which does use `cpu_capacity`. vcap_probe itself runs at RT priority at ~800% CPU, in every mode |
| H3 | BPF or per-process state accumulates (e.g. `jit_tgids`, `last_candidate_pid`, mm cpumask spread) | drift that does not reset between hackbench processes | Each round is a new hackbench process, so per-process state resets. Less likely; only system-wide maps could carry over |
| **H4** | **Per-vCPU host credit** (§2): migration concentrates load on "clean" vCPUs, whose host threads lose their credit | migrations keep being accepted at a steady rate, but each is worth less; utilisation stays concentrated on the same vCPUs | Host scheduler timescales are plausible at minutes. Cannot be observed directly from inside the guest (and in-guest steal is untrustworthy, see memory) |
| H5 | Adaptive spinning, not migration, drifts | drift also appears with migration off + `spin_mode 2` | Nothing points at it yet, but it is one cheap run to exclude |

## 4. Test plan

Run all tests with the co-runner on and settled, the same idle gap before each
run (3 min, measured, not guessed), 8 rounds, `dmesg -n 1`. Compare **round by
round**, never mean against mean.

### T0: which half of IVH+AS drifts? (excludes H5, localises everything else)

Three 8-round runs, interleaved order across repeats:
- `ivh` = migration ON, `spin_mode 1`
- `as` = migration OFF, `spin_mode 2`
- `pv` = both off (control, expected flat, run C)

`cross_kernel_baseline.sh` currently supports `MODE=ivhas` and `MODE=pv`; add
`ivh` and `as` (two more `case` arms).

- **Only `ivh` drifts** → migration is the cause. Go to T1.
- **Only `as` drifts** → H5. Capacity and migration are off the hook.
- **Both drift** → go to T1 anyway; they may share an input.

### T1: watch the gate while it drifts (separates H1/H2 from H4)

One 8-round IVH+AS run. Before round 1 and after every round, snapshot:

| what | how |
|---|---|
| hackbench time | the script |
| per-CPU `ivh_uc_capacity`, `ivh_uc_capacity_wall` | `python3 /root/ivh_tools/read_vact_rq.py` (reads struct rq through `/proc/kcore`; offsets are hardcoded, re-check with `pahole -C rq vmlinux` on a new kernel) |
| per-CPU `cpu_capacity` (vcap) | same approach; add the field offset to `read_vact_rq.py` |
| migration accept/reject counts by reason | `bpftool map dump name reject_reasons` (per-CPU array, keys `REJ_*` 0-11 in `MY_ivh_atc.bpf.c:215-230`); take per-round deltas |
| accepted migrations | `bpftool map dump name last_migration` (`count` field) |
| per-vCPU utilisation | `mpstat -P ALL 1` during each round |

Reading the result:
- **Accepts fall and `REJ_CAPACITY_LOW` rises in step with the slowdown** → the
  capacity gate is closing (H1 or H2). Check which capacity field sinks.
- **Accepts stay steady while times rise, and utilisation is concentrated on the
  same vCPUs** → the migrations still happen but are worth less: H4.
- **Neither** → H3, or something not listed. Widen the snapshot.

### T2: break the gate on purpose (confirms H1/H2)

Only if T1 points at the capacity gate. Repeat T1 with the gate effectively off
(`ivh_capacity_threshold` lowered so capacity never rejects; pick the value from
T1's observed minimum). If the drift disappears, the gate is the cause. Note
that this changes migration quality too, so judge it by the drift shape, not by
the absolute time.

### T3: measure how fast it recovers (checks the timescales in §3)

Run one IVH+AS round after idle gaps of 15 s, 60 s, 3 min and 8 min, each
preceded by the same 6 loaded rounds to drive the system into the slow state.
Fit the recovery time constant:
- ~15 s → consistent with the uc EMA (H1)
- ~2 min → consistent with vcap (H2), or host (H4)
- much longer → neither estimate; look elsewhere

### T4: exclude host credit (H4) with host-side data

The one test the guest cannot do alone. Ask the user for host-side per-vCPU-thread
CPU time or scheduler stats for this VM during a drifting run. Do not substitute
in-guest steal readings.

## 5. What to do with measurements until this is resolved

- Always record the idle gap and round position alongside every IVH number.
- Compare IVH arms against each other and against PV **round by round**, with
  matched idle gaps and round counts.
- Interleaved A/B designs are still valid for relative comparisons **only if**
  both arms see the same load history; a design where one arm always follows a
  long loaded stretch is biased by this effect.
- The G-LOCK-29 cross-kernel check uses run B as its reference, compared round by
  round.
