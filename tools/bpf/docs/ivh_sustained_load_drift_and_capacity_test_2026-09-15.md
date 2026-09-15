# IVH+AS slows down under sustained load: finding and capacity test plan, 2026-09-15

**Status: mechanism of the within-run drift identified (§6). A 60 s wait does NOT
make IVH+AS numbers stable (§6.4). Data-collection rules are in §7.** §3-§4 are
the original plan, kept for the record.

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


## 6. Results (investigated the same night, G-LOCK-28, co-runner on)

Tools: `/root/ivh_tools/drift_snap.py` (per-CPU `ivh_uc_capacity` and
`cpu_capacity` via `/proc/kcore`, `reject_reasons` and `last_migration` from
MY_ivh_atc, `/proc/stat`), `drift_t1.sh` (rounds + 5 s sampler + idle tail),
`wait_capacity_settled.sh`, `drift_validate_wait.sh`. Data:
`drift_t1_003400.*`, `drift_pv_004510.*`, `drift_validate_wait_005201.log`.

### 6.1 IVH+AS, 8 rounds with the gate instrumented (T1)

| round | time (s) | capacity mean (min) | migrations/s | CAP_LOW rejects/s | accepted T1/s |
|---|---|---|---|---|---|
| 1 | 23.5 | 739 (650) | 4,861 | 63,781 | 9,330 |
| 2 | 26.7 | 698 (586) | 6,124 | 139,826 | 8,290 |
| 3 | 28.7 | 670 (572) | 4,160 | 181,787 | 5,580 |
| 4 | 30.7 | 660 (579) | 3,894 | 197,784 | 4,950 |
| 5 | 34.7 | 651 (578) | 2,448 | 282,736 | 2,991 |
| 6 | 37.2 | 665 (601) | 2,797 | 350,415 | 3,053 |
| 7 | 37.6 | 666 (596) | 2,816 | 366,778 | 3,007 |
| 8 | 36.7 | 661 (566) | 2,895 | 332,475 | 3,185 |

Capacity at the start (after ~9 min idle) was 848 mean, 807 min.

- **Mechanism of the drift:** as the in-kernel capacity estimate sinks, the
  destination capacity gate in MY_ivh_atc rejects more candidates
  (`REJ_CAPACITY_LOW` ×5.5; the gate is the absolute rail
  `IVH_CAP_HARDFLOOR` = 600 plus the relative `IVH_CAP_TOPBAND` = 50,
  `MY_ivh_atc.bpf.c:600-625`, and the minimum CPU sits right at the rail).
  Accepted migrations fall by about two thirds and hackbench slows by 60% in
  lockstep. Share of evaluations accepted: ~9% in round 1, ~0.8% by round 7.
- **The drift plateaus** once capacity stops falling (rounds 6-8, 36.7-37.6 s,
  within 1.2%). Run B also levelled off by rounds 7-8 (49.9, 50.2 s).
- **H2 (vcap) is out:** `cpu_capacity` read 1024 on every CPU in every sample.
- **H4 (per-vCPU load concentration) not supported by utilisation:** guest busy%
  on CPUs 0-7 vs 8-15 stayed within a few points (60-62% vs 56-62%). This does not
  exclude host-side effects, which the guest cannot see.

### 6.2 PV, 5 rounds with the same instrumentation

| round | time (s) | capacity mean (min) |
|---|---|---|
| start | — | 769 (693) |
| 1 | 52.7 | 601 (562) |
| 2 | 52.3 | 597 (576) |
| 3 | 52.4 | 600 (570) |
| 4 | 52.6 | 587 (568) |
| 5 | 55.6 | 567 (555) |

**Capacity sinks under PV load too, and further** (PV spins more, so the vCPUs
are busier for the host to steal from). So the capacity drop is **not caused by
IVH**. `ivh_uc_capacity` = 1024 × (busy − stolen) / busy per 200 ms window
(`kernel/sched/core.c:540-600`, EMA at `:340-400`): it is measuring host
contention under load. IVH's gate reacts to it; PV has no gate, so PV times stay
flat. PV is stable across runs: mean 52.97 s, sd 0.50 s (0.95%) over 9 rounds
from two runs.

### 6.3 Recovery during idle

Both idle tails show the same shape:
- **fast part, 30-60 s:** capacity climbs back to ~760-780 (IVH tail: 661 →
  770 by +62 s; PV tail: 576 → 770 by +32 s, 783 by +48 s). Consistent with the
  EMA's ~15 s time constant.
- **slow part, many minutes:** after 6 min of idle the IVH tail was still flat at
  ~765-777, below the 848 seen after ~9 min idle. Cause not identified.

### 6.4 Does "wait a minute" fix it? No.

`drift_validate_wait.sh`: 6 cycles of (wait until capacity has held steady for
15 s, minimum 60 s; then ONE IVH+AS round).

| cycle | settled capacity | time (s) |
|---|---|---|
| 1 | 759 | 39.4 |
| 2 | 776 | 22.8 |
| 3 | 774 | 32.1 |
| 4 | 785 | 33.9 |
| 5 | 778 | 28.9 |
| 6 | 769 | 28.6 |

The wait works as designed (settled in 62-63 s every time, at 759-785), but the
round times are **31.0 s mean, sd 5.6 s, CV 18%**, range 22.8-39.4 s, with no
useful correlation to the settled capacity (r = −0.43 over 6 points). Compare PV
at 0.95%.

So there are **two separate problems**:
1. **Within-run drift** (sustained load → capacity sinks → gate rejects →
   IVH slows). A ~60 s idle gap removes this, and it is understood.
2. **Round-to-round IVH+AS variance of ~18% even from the same starting
   capacity.** Not explained by anything measured here. Between-run offsets are
   large too (run B round 1 = 35.1 s, T1 round 1 = 23.5 s; plateaus 50 s vs 37 s)
   while PV repeats to 1%. Leading guess: the host co-runner's placement or
   intensity changes which vCPUs are worth migrating to, which matters to IVH and
   not to PV. That needs host-side data to test (T4).

## 7. Rules for data collection until problem 2 is understood

1. **Never compare IVH numbers across separate sessions, runs or reboots.** Only
   paired, interleaved comparisons within one session are trustworthy.
2. **Interleave arms in short ABBA blocks** so every arm sees the same host state,
   and **run PV as a control inside every block**. Report paired differences and
   medians over many blocks, not means of separate runs.
3. **Put a capacity-settled gap before every measured round:**
   `QUIET=1 /root/ivh_tools/wait_capacity_settled.sh` (typically ~60 s). This
   removes the within-run drift. It does not remove problem 2, so rules 1-2 still
   apply.
4. **Use enough rounds for an 18% CV.** Detecting a 5% difference at 80% power
   with paired rounds needs roughly (2.8 × 18 / 5)² ≈ 100 rounds if pairing
   removes nothing; pairing within ABBA blocks should remove the shared host
   component, so measure the paired-difference SD from the first blocks and size
   the run from that.
5. **Neutrality checks of new kernel code should run in PV mode** (CV ~1%, stable
   across runs), not IVH+AS. For G-LOCK-29 the stamp, the unlock-path gate and the
   head-halt timing are all mode-independent, so PV measures their cost. PV
   reference on G-LOCK-28: 52.97 s, sd 0.50 s.

## 8. Gate-loosening A/B: the strict capacity gate is the cause (same night)

Context from the user: **all 16 vCPUs are contended by the host sysbench
co-runner**, so no destination is genuinely clean and the capacity differences
the gate ranks on are not meaningful here.

Test: `/root/ivh_tools/gate_loose_test.sh`, log `gate_loose_012448.log` (+
`.snaps.jsonl`). Two scratch rebuilds of the running MY_ivh_atc source
(`/root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc.bpf.c`):
- **N (normal):** `IVH_CAP_HARDFLOOR 700`, `IVH_CAP_TOPBAND 50` (the running values;
  note the `/root/linux-6.17/tools/bpf` copy says 600 and is stale)
- **L (loose):** `IVH_CAP_HARDFLOOR 500`, `IVH_CAP_TOPBAND 250` (capacity gate effectively off)

Order N L L N P, 5 consecutive IVH+AS hackbench rounds per arm, capacity-settled
wait before each arm. vcap_probe untouched; daemon swapped per arm.

| arm | round times (s) | median | CV | migrations/s | CAP_LOW rejects/s | accepted/s | accept share |
|---|---|---|---|---|---|---|---|
| N | 35.2 48.2 53.1 52.8 55.2 | **52.8** | 16.5% | 878 | 70,620 | 1,215 | 1.3% |
| L | 32.1 34.1 28.5 31.0 32.1 | **32.1** | 6.5% | 9,361 | 0 | 15,967 | 6.4% |
| L | 31.5 32.5 31.9 29.1 31.3 | **31.5** | 4.1% | 10,576 | 0 | 16,875 | 5.2% |
| N | 30.9 39.7 50.0 50.6 50.4 | **50.0** | 19.9% | 1,527 | 385,858 | 2,122 | 0.5% |
| PV | 54.6 54.7 54.3 54.5 54.2 | **54.5** | 0.4% | 0 | 0 | 0 | — |

- **The loose gate removes the drift.** Normal-gate arms start near 31-35 s and
  sink to PV's level (50-55 s) by round 3; loose-gate arms hold at 28.5-34.1 s
  for all 5 rounds, twice.
- **Loose is ~38% faster than normal at the plateau and ~42% faster than PV**
  (medians 31.8 vs 51.4 vs 54.5 s).
- **Most of the "unexplained ~18% variance" of §6.4 was the gate too:** loose-arm
  CV is 4-6%, normal-arm CV 17-20%.
- With the capacity gate out of the way, `REJ_NOT_BETTER` becomes the main filter
  (170-230k/s) and accepted migrations rise ~10x.
- No soft-lockup / RCU-stall / hung-task warnings in either loose arm; no round
  timed out. The migration-storm concern in the `IVH_CAP_HARDFLOOR` comment did
  not show up on this workload.

**Caveats:** one NLLN block on one workload (hackbench) in one host condition
(full co-runner contention). The loose values were chosen to disable the gate,
not tuned. Not yet checked: dbench/ebizzy, a partly contended host (where the gate
has real clean destinations to prefer), or intermediate settings.

**Implications:**
1. Under full host contention the capacity gate is counterproductive: it keeps
   IVH from migrating and pulls IVH+AS down to PV within ~3 rounds.
2. Every IVH result measured with the strict gate under this co-runner likely
   understates IVH, and its size depended on load history (§1).
3. §7's rules still apply until a gate setting is adopted and re-validated.

Rebuild notes (for making a variant): compile the BPF object with
`clang -g -O2 -target bpf`, generate the skeleton with
`bpftool gen skeleton ... name MY_ivh_atc`, and link the loader against the
**patched libbpf from `/root/linux-6.17/tools/lib/bpf`** (it knows the `sched+`
section). The libbpf under `resolve_btfids` does not, and the program fails to
load with `-EINVAL`.

## 9. Half contention (co-runner moved to vCPUs 0-7), original daemon, normal gate

`/root/ivh_tools/half_contention_check.sh`, order I P P I, 3 consecutive rounds
per arm, capacity-settled wait before each arm. Log `half_contention_*.log`.

| arm | round times (s) | mean | migrations/s | busy% vCPU 0-7 / 8-15 | capacity 0-7 / 8-15 |
|---|---|---|---|---|---|
| IVH+AS | 12.38 12.29 12.24 | 12.30 | 3,859 | 37 / 85 | 353 / 1023 |
| PV | 58.90 59.94 60.02 | 59.62 | 0 | 64 / 76 | 338 / 882 |
| PV | 63.95 65.47 61.95 | 63.79 | 0 | 62 / 77 | 348 / 875 |
| IVH+AS | 12.26 12.25 12.23 | 12.24 | 3,914 | 36 / 85 | 348 / 1022 |

- **IVH+AS 12.27 s (sd 0.06 s) vs PV 61.71 s (sd 2.6 s): 5.0x, 80% less time.**
- **No drift** in either IVH arm, and IVH is far steadier than PV here.
- IVH moves the work onto the clean half (busy 85% on vCPUs 8-15 vs 37% on 0-7).
- With a genuinely clean half, the normal capacity gate does its job: clean vCPUs
  read ~1022 and pass, contended ones read ~350 and are rejected. So the §6-§8 drift
  is specific to **full** contention, where the gate has nothing real to prefer.
- Not yet run here: the loose gate. By arithmetic it should behave the same (the
  contended half at ~350 is below both the 500 floor and best-minus-250), but that
  is unmeasured.

### 9.1 Same, with the NEW (loose) gate

`/root/ivh_tools/half_contention_loose.sh`, I P P I, I = loose daemon (3 rounds),
P = PV (1 round). Log `half_contention_loose_*.log`.

| arm | round times (s) | migrations/s | CAP_LOW rejects/s | busy% 0-7 / 8-15 |
|---|---|---|---|---|
| IVH+AS, new gate | 12.82 12.34 12.24 | 3,779 | 23,486 | 36 / 80 |
| PV | 60.65 | 0 | 0 | 65 / 75 |
| PV | 58.99 | 0 | 0 | 63 / 77 |
| IVH+AS, new gate | 12.28 12.24 12.30 | 3,869 | 24,108 | 36 / 84 |

- **New gate 12.37 s mean (12.28 s without its first round) vs old gate 12.27 s:
  the same within noise.** The one slower round (12.82 s) is the first round
  after the daemon restart.
- As predicted, the new gate still rejects the contended half (~24k capacity
  rejects/s, same migration rate as the old gate).
- No lockup/stall warnings.
- The two gates were measured ~30 min apart, not interleaved; at half contention
  IVH is steady enough (sd ~0.06 s) that this is unlikely to matter.

**Summary across both contention levels:** the loose gate (floor 500, band 250)
matches the old gate at 50% contention and is ~38% faster and far steadier at
100% contention. Still to check before adopting it: dbench and ebizzy.

## 10. Old vs new gate on dbench and ebizzy (half contention, vCPUs 0-7)

`/root/ivh_tools/gate_dbench_ebizzy.sh`, per workload N L P L N (N = original
daemon / old gate, L = loose / new gate, 3 rounds each; P = PV, 2 rounds),
capacity-settled wait before each arm. Commands as in the original wins:
`dbench -F -t 12 16 -D /root/dbench_test`, `ebizzy -S 20 -t 16 -m -s 4194304`.
Log/CSV `gate_dbench_ebizzy_*`. (The script exits 1 from its final dmesg test
when no warning is found; all rounds completed, no warnings, daemon restored.)

| workload | old gate | new gate | PV | old vs PV | new vs PV | new vs old |
|---|---|---|---|---|---|---|
| dbench (MB/s) | 280.2 (sd 2.0) | 283.3 (sd 4.4) | 236.3 | +18.6% | +19.9% | +1.1% |
| ebizzy (records/s) | 1929.5 (sd 10.4) | 1925.7 (sd 11.6) | 897.0 | +115.1% | +114.7% | −0.2% |

Both wins are intact under the new gate; the differences are within round-to-round
noise. The win sizes differ from the historical +33.7% / +136%, which came from a
different host setup, so they should not be compared directly.

**Overall verdict on the loose gate (floor 500, band 250):** equal at 50%
contention on hackbench, dbench and ebizzy; ~38% faster and much steadier on
hackbench at 100% contention. Not yet the default.
