# IVH on pure TSC-estimated steal: probe-only vcap, threshold tuning, and what it actually achieves

**Date:** 2026-08-08
**Kernel under test:** `6.17.0-rseqport71-byeunhalt+` (live, booted, executing tonight's new code), branch `kernel-43-clean`, base commit `6c3874293`
**Host:** INTEL(R) XEON(R) GOLD 6554S, `tsc_khz=2200000`, guest 16 vCPU, `CONFIG_HZ=1000`
**Host contention:** skewed 8-vCPU corunner, verified at the start, middle and end of the session (sec 2)
**Status:** **everything below is LIVE-TESTED.** 91 real hackbench runs. No replay, no emulator, no simulation. Where something is inferred rather than measured, it says so.

---

## 0. The short version

| question | answer |
|---|---|
| Does the probe-only vcap work? | **Yes, live.** Idle-vCPU publish cadence goes from *never* to 2.5 s. Sec 3. |
| Was vcap already `SCHED_IDLE`? | **Yes** — verified live on the original binary. The user's belief was correct; nothing needed fixing. Sec 3.2. |
| Can `ivh_steal_source=2` reach the ~11-12 s target? | **Yes — 11.25 s, 11.36 s, 11.57 s, 11.62 s, 11.80 s, 11.88 s** across several configurations. It matches and sometimes beats the steal-page reference. |
| Does it *hold* that? | **No.** It decays to 13.9-14.4 s (baseline) after 3-4 consecutive runs, in every configuration tried. Sec 7. |
| Is the winning configuration PV-free? | **No, and this is the biggest caveat in this document.** The knob that made it work (`ivh_uc_used_source=1`) reads `kcpustat`, which on this guest has PV steal subtracted out of it. Sec 8. |
| Overall confidence in "IVH works about as well on TSC as on steal page / UNHALTED.REF" | **Partially true, and I would not claim more.** Transiently yes; sustained no; PV-free no. Sec 9. |

**The single most consequential finding of the night is not about TSC at all:** `ivh_universal_eligible` defaults to 0 and is set by **neither** `/home/nick/IVH` **nor** `/home/nick/ivh_mode.sh`, so IVH was completely inert in both. Sec 4. Every historical measurement taken with those scripts after commit `298be1454` needs re-checking against this.

---

## 1. Method, and what makes these numbers trustworthy

Reference command, unchanged from the rest of the project:

```
/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000
```

Every single run recorded, automatically and at the same time:

- **wall time** (and `ivh_exec`'s own reported figure, which agrees to ~10 ms);
- **per-vCPU PV steal delta** from `/proc/stat` across exactly that run. This is the corunner check, and it is **independent of everything under test** — `/proc/stat`'s steal column is `paravirt_steal_clock`, not `ivh_tks_steal_ns`, so it cannot be fooled by a broken source-2 estimator;
- **mean `rq->ivh_uc_capacity` per vCPU**, sampled at 100 ms through the run, split contended (0-7) / clean (8-15);
- **`ivh_migrations_done`** delta.

Harness: `runbench.sh` in the session scratchpad; raw rows in `results.tsv` (91 runs).

**A measurement bug I made and caught.** The first version of the harness parsed `ivh_migrations_done:168337` with `awk '{print $2}'`. There is no space after the colon, so it silently reported `migr=0` for every run. That is why sec 4's first reference attempt looked like "IVH does nothing" for two different reasons at once. Fixed; every `migr` figure in this document is from the corrected parser.

---

## 2. Corunner verification — checked three times, never assumed

The prior report in this thread records a run accidentally taken with the corunner off. Independent check (all 16 vCPUs driven to a busy loop for 6 s, `/proc/stat` steal delta):

| when | cpu0-7 | cpu8-15 |
|---|---|---|
| session start, before any config change | **67.2 - 67.3 %** | 0.2 % |
| mid-session, after ~50 runs | **67.7 - 68.0 %** | 0.2 - 0.3 % |
| session end, after all runs | **67.5 %** | 0.2 % |

Strongly skewed, stable, and unchanged across the whole session. The 8-vCPU corunner configuration (not the 16-thread one) was in place throughout.

Additionally, **every individual run row below carries its own contended/clean steal split**, so no reported timing rests on a global assumption. A run with the corunner off would show `cont ~0%` and is instantly visible. None do.

---

## 3. Part 1 — vcap stripped to a pure probe

### 3.1 What was built

`/home/nick/vsched_main/vcapacity/vcap_probe.cpp` → binary `vcap_probe`. 210 lines.

It computes **nothing**. It never opens `/proc/vcapacity_write`, `/proc/vav_capacity_write`, `/proc/vlatency_write`, `/proc/vact_write` or `/proc/vcap_info` — verified with `strings` on the binary, zero matches. No capacity, no steal ratio, no EMA, no latency, no preempt counting, no banlist, no straggler disabling, no cgroup boosting.

What remains: argument parsing (`-p`/`-s`/`-v`, and the historical `-d`/`-c`/`-i`/`-o` accepted-and-ignored so old command lines still work), one thread pinned per vCPU, and a phase-aligned probe loop at the specified cadence.

- Workers: pinned, joined to `/sys/fs/cgroup/lw_prgroup`, then `SCHED_IDLE`.
- Coordinator: `SCHED_RR` at max priority, sleeps for all but a few microseconds of each cycle — structurally identical to the original vcap's main thread, deliberately, to avoid introducing a confound.
- Probes are broadcast so all 16 vCPUs probe simultaneously, matching the original.

**Argument reading confirmed against the source, not assumed.** `set_arguments()` (`main.cpp:285-286`) maps `-s` → `sleep_length` and `-p` → `profile_time`, both in ms. So `-p 200 -s 5000` is indeed a 200 ms probe every 5000 ms, i.e. a 5.2 s period at 3.85 % duty. `vcap_probe` reproduces this exactly.

**Why a spin and not a timer ping:** as established in `ivh_idle_keepalive_2026-08-08.md` sec 2.2/2.3, restoring ticks without restoring *demand* leaves `ivh_uc_tick()`'s avail term at zero and the min-avail guard then refuses every window close forever. That finding was read, not relearned.

### 3.2 Was the original vcap already `SCHED_IDLE`? — verified live, yes

The task said not to assume this. Reading `main.cpp` first: `move_thread_to_low_prio()` (line 166) does `sched_setscheduler(tid, SCHED_IDLE, ...)`; `move_thread_to_high_prio()` (line 181) writes **only** to the `hi_prgroup` cgroup and does **not** call `sched_setscheduler`. So the periodic "heavy profile" boost changes cgroup membership, not scheduling policy, and workers should remain `SCHED_IDLE` throughout.

Confirmed live with `chrt -p` on the running original binary (pid 55399, mid-session):

```
55399  SCHED_RR        <- coordinator
55400  SCHED_IDLE      <- 16 workers, all SCHED_IDLE
...    SCHED_IDLE
55415  SCHED_IDLE
```

**No fix was required.** `vcap_probe` matches this exactly.

### 3.3 Backup — the non-optional safety requirement

`/home/nick/vsched_main/vcapacity_ORIGINAL_BACKUP_2026-08-08/` contains `main.cpp`, `main.o`, `Makefile`, `vcap`, `vcap.pre-fix-backup`.

- Backup binary **md5-identical** to the working copy: `cd3df9f367bddd9ac140d28f4ae6ab0b`.
- Backup source **md5-identical**: `deb6cea9f036b9de0318b7ac5059d45d`.
- **The backup binary was run before anything was touched** and produced correct output (capacity 0.71-0.74 on contended vCPUs). It was also used as the live arm-A reference for 9 of the runs in this document, so it is not merely "present", it is *proven working on this kernel*.

Beyond the backup, a third safety layer: **`main.cpp` and the `vcap` binary were never modified.** `vcap_probe` is a separate source file and a separate binary, added to the Makefile as a separate target (`make vcap_probe`). Anything invoking `./vcap` gets the original, unchanged behaviour. Verified at session end — both files still md5-identical to the backup.

### 3.4 Live proof the probe does its job

Idle guest, `/proc/ivh_debug` `ivh_uc_cpu:` windows delta:

| | cpu0-7 (contended) | cpu8-15 (clean) |
|---|---|---|
| **no probe**, 15 s | 0-1 windows → **1.5 s to NEVER** per publish | 2-10 windows |
| **`vcap_probe -p 200 -s 5000`**, 30 s | 12 windows each → **2.50 s** | 12-26 windows → 1.15-2.50 s |

Baseline reproduces `ivh_idle_keepalive_2026-08-08.md` sec 5.1 exactly (cpu2 published **zero** windows; here cpu2, cpu4, cpu5, cpu6, cpu7 all published zero in 15 s). With the probe, **every one of the 16 vCPUs publishes on a regular cadence**, ~2 windows per 5.2 s probe cycle, which is the arithmetic you expect: the long-pending window closes once avail crosses `min_avail_pct`, then a second closes at the 200 ms mark.

Cost, measured: `vcap_probe` runs at **54.5 % of one vCPU** total across its 16 threads — i.e. 3.4 % of the guest, which is 16 × the 3.85 % duty cycle as designed. This is the same cost the original vcap carries.

`ivh_ka_enabled` was left at **0** throughout and never enabled, as instructed.

---

## 4. The finding that invalidates the launch scripts: `ivh_universal_eligible`

First reference attempt, correct config otherwise, gave **14.15-14.55 s — slower than no IVH at all** — with `ivh_migrations_done`, `ivh_prelock_calls` and every `reject_reasons` bucket reading exactly **0**. The destination scanner was never invoked.

Cause, from `kernel/locking/spinlock.c:275`:

```c
/* 2026-07-20: PF_IVH_ELIGIBLE no longer consulted here --
 * ivh_universal_eligible is the sole gate now, not an ivh_exec-wrapper opt-in. */
if (!READ_ONCE(ivh_universal_eligible) || current->ivh_exclude)
        return;
```

`ivh_universal_eligible` defaults to **0**. It is set by **neither** `/home/nick/IVH` **nor** `/home/nick/ivh_mode.sh`. Both scripts predate the change and were never updated.

Setting it to 1, same config, immediately:

| | wall | contended PV steal | migrations |
|---|---|---|---|
| `ivh_universal_eligible=0` | 14.15 - 14.55 s | 29.6 - 30.9 % | 0 |
| `ivh_universal_eligible=1` | **12.23 - 12.48 s** | **10.3 - 11.4 %** | ~70,000 |

The steal drop from ~30 % to ~10 % is the mechanism visibly working: hackbench threads are being moved onto the uncontended vCPUs.

**This was not fixed in the scripts** (they are outside the stated scope of files to edit, and changing them silently would hide the finding). It must be added to both, or every future run through those scripts measures nothing.

---

## 5. Reference arms — reproduced live on this kernel

All with `ivh_universal_eligible=1`, and with the spin/preempt mechanism deliberately held **constant** at the non-TSC default (`ivh_pv_wait_mechanism=0`, `ivh_pv_preempt_src=0`, `ivh_preempt_event_source=0`) so that the only thing varying between arms is the **steal signal and capacity publisher**. This is a tighter comparison than `ivh_mode.sh`'s modes, which change the spinning mechanism at the same time.

| arm | steal signal | capacity publisher | runs | wall (s) | mean |
|---|---|---|---|---|---|
| **BASE** | — (IVH off) | — | 3 | 13.78, 13.88, 13.97 | **13.88** |
| **A** | PV steal page (`ivh_steal_source=0`) | vcap → `rq->cpu_capacity` | 4 | 11.87, 11.94, 12.17, 12.06 | **12.01** |
| **A (rested)** | as above | as above | 5 | 11.76, 11.57, 12.22, 11.94, 12.16 | **11.93** |
| **B** | UNHALTED.REF (`ivh_steal_source=1`) | vcap → `rq->cpu_capacity` | 4 | 12.40, 12.33, 12.51, 12.45 | **12.42** |

Corunner verified on every row (contended 7.0-11.2 %, clean 0.1-0.2 % during IVH runs; 28.9-29.3 % contended in BASE, which is what "IVH off" looks like).

**Arm A is the ~11-12 s target, reproduced: 11.57-12.22 s over 9 runs, sd 0.21 s.** It is notably *stable* — that stability turns out to be the property source 2 struggles to match. Arm B (UNHALTED.REF) is slightly worse but also stable.

---

## 6. Part 2 — tuning source 2. What worked, what did not

### 6.1 The knob that mattered: `ivh_uc_used_source`

`ivh_uc_close()` publishes one of two ratios:

- **WALL** (`ivh_uc_used_source=0`, the default): `used / avail` where `avail = elapsed − idle` from raw TSC.
- **ACCT** (`=1`): `acct / (acct + stolen)` where `acct` is `kcpustat` USER+NICE+SYSTEM. The in-tree comment says this "reproduces vcap's literal formula" — and vcap's formula is exactly `used/(used+stolen)` (`main.cpp:369`).

Live, under hackbench, per-vCPU `ivh_uc_capacity` sampled at 100 ms:

| variant | contended mean | contended **max** | clean mean | clean **min** | separation |
|---|---|---|---|---|---|
| WALL, clean vCPUs unloaded | 889 | 944 | 999 | 968 | **+24** |
| WALL, clean vCPUs loaded | 842 | 880 | 894 | **812** | **−68** |
| **ACCT** | 833 | **893** | 1016 | **1003** | **+110** |

WALL's separation *depends on how loaded the clean vCPUs are*, and inverts when they carry work — which is precisely the state IVH's own success creates. ACCT does not: the formula has no idle term, so a vCPU that is merely busy is not penalised.

This is the difference between the configuration reaching 11.3 s and not reaching it at all.

### 6.2 Everything swept, with live numbers

All on arm C (`ivh_steal_source=2`, `ivh_cap_source=3`, `ivh_cfg[0]=3`, `vcap_probe -p 200 -s 5000`).

| knob | values tried | result |
|---|---|---|
| `ivh_uc_used_source` | 0 (WALL), **1 (ACCT)** | **decisive.** See 6.1. ACCT ships. |
| `ivh_capacity_threshold` | **965**, 990, 1000, 1010 | 965 best (mean 12.59). 990 → 13.17, 1000 → 14.30, 1010 → 13.42 (stable but mediocre). |
| `ivh_uc_min_avail_pct` | **10**, 30, 40, 70 | 10 (default) best. **40 → 15.86 s mean, 70 → 14.76 s, both with `migr=0` on every run** — raising it selects unrepresentative high-demand windows and collapses the clean population to 750-909. |
| `ivh_uc_min_steal_ns` | 10000, 200000, **500000**, 1e6, 2e6 | mild, monotone: separation 28 → 37 → 40 → 39. 500000 ships. Not a large effect. |
| `ivh_uc_ema_alpha_q16` | **868**, 4000, 6000, 16000, 40000 | 868 (default) best by a wide margin. 4000 → 15.12 s; **6000 → 16.42, 16.35 s with `migr=0`.** Faster EMA makes the signal noisy enough that clean vCPUs dip below the floor. |
| `ivh_tks_deadband_ns` | 0, 50000, 200000, 500000, 1e6, 1e7 | **inert — see 6.3.** |
| `ivh_tks_phase_pct` | 0 (kept) | left at the shipped default; sec 6.3 shows it is what makes the deadband inert, so moving it would change two things at once. |
| `IVH_CAP_HARDFLOOR` | 950, 900, **880**, 850 | 880 marginally best: holds the good regime for 4 runs instead of 3. Not a fix (sec 7). |
| `IVH_CAP_TOPBAND` / `IVH_CAP_MARGIN` | 50/50 (kept), 30/20 | **loosening is harmful**: 30/20 gave 12.66 → 15.08 s progressive degradation. The validated 50/50 plateau survives. |
| `ivh_uc_window_ns`, `ivh_uc_duty_ns` | 200 ms / 0 | left at defaults; duty is documented in-tree as a comparison-only knob, not a production one. |
| probe cadence `-p 200 -s 5000` | kept | see 6.5. |

### 6.3 `ivh_tks_deadband_ns` does not do what the previous report says it does

Reading `ivh_tick_steal_accumulate()` (`core.c:2287-2296`):

```c
excess_c = (s64)avail_c - (s64)tick_c;
if (excess_c > (s64)ivh_tsc_ns_to_cycles(READ_ONCE(ivh_tks_deadband_ns))) {
        unsigned long pct = READ_ONCE(ivh_tks_phase_pct);
        if (pct)
                excess_c += (s64)div64_u64(tick_c * pct, 100);
        rq->ivh_tks_events++;
}
carry = rq->ivh_tks_carry_c + excess_c;      /* <-- UNCONDITIONAL */
```

The deadband gates only the phase correction and the `ivh_tks_events` diagnostic counter. The carry accumulates **every** tick's excess regardless. With the shipped `ivh_tks_phase_pct = 0`, the deadband therefore has **no effect whatsoever** on `rq->ivh_tks_steal_ns`.

Verified live rather than left as a code-reading claim — `ivh_tks_steal_ns` accrual per vCPU over 10 s under identical hackbench load, deadband varied by a factor of 200:

| deadband | cpu0-7 accrual | cpu8-15 accrual |
|---|---|---|
| **0 ns** | 0.184 - 0.210 s | 0.000 - 0.006 s |
| **10,000,000 ns** | 0.202 - 0.277 s | 0.000 - 0.001 s |

Identical within run-to-run noise. `ivh_cvm_steal_detector_2026-08-08.md` sec 5 states the deadband cuts phantom steal from **27.8 s to 0.037 s** and calls it "not optional". That is a property of the **replay model**, not of the shipped C function. The shipped code relies instead on the signed carry to cancel symmetric noise — which is a different mechanism with different (asymmetric, see below) behaviour.

This is a genuine divergence between the validated design and the shipped code. **It was not fixed: kernel source is out of scope for this task and any change needs a reboot.** Flagged for follow-up.

(The carry is also a rectifier in one direction: `if (carry > 0) { drain in full; carry = 0; }` while negative carry is floored at only 8 ticks of debt. Positive excursions drain completely, negative ones saturate. **Inferred from code reading; not isolated experimentally.**)

### 6.4 Source 2's absolute accuracy is far worse than the replay predicted

Same 10 s window, hackbench, PV steal from `/proc/stat` against `rq->ivh_tks_steal_ns`:

| vCPU | PV steal (truth) | source 2 estimate | recovery |
|---|---|---|---|
| cpu0 | 2.460 s | 0.178 s | **7.2 %** |
| cpu1 | 2.480 s | 0.180 s | 7.3 % |
| cpu2 | 2.260 s | 0.175 s | 7.7 % |
| cpu3 | 1.090 s | 0.178 s | 16.4 % |
| cpu4 | 2.800 s | 0.177 s | 6.3 % |
| cpu5 | 0.980 s | 0.184 s | 18.8 % |
| cpu6 | 1.090 s | 0.180 s | 16.5 % |
| cpu7 | 0.730 s | 0.172 s | 23.6 % |
| cpu8-15 | 0.010 - 0.030 s | 0.000 - 0.002 s | ~0 % |

`ivh_cvm_steal_detector_2026-08-08.md` sec 6.1 predicted **62-83 %** recovery on contended vCPUs from replay. Live it is **6-24 %**.

Worse for the metric, better for the story: **the estimate is nearly constant (~0.178 s) across vCPUs whose true steal varies by 3.8× (0.73 s to 2.80 s).** It carries almost no magnitude information at all. What it does carry, cleanly, is *presence*: 0.17-0.18 s on every contended vCPU versus 0.000-0.002 s on every clean one — a ~100× ratio with no overlap.

So the earlier report's summary "**ordinally reliable, metrically unreliable**" is confirmed live and is in fact an *understatement* of the metric problem. That is exactly why the ACCT formula rescues it (it needs only "is there steal here", scaled against a large accounting denominator) and why the WALL formula does not (it needs the steal magnitude to be right relative to a TSC-derived denominator).

### 6.5 On the 200 ms / 5 s probe cadence

Kept as specified, and **I do not think it is the wrong choice** — sec 3.4 shows it restores full publish coverage on every vCPU, which is all it is being asked to do. One caveat worth recording: at that cadence an *idle* vCPU publishes ~1 window per 5.2 s, and at `ivh_uc_ema_alpha_q16=868` the EMA half-life is 52 samples, so an idle vCPU's capacity has an effective half-life of ~4.5 minutes. That is not a problem in the tested workload (all 16 vCPUs are busy during hackbench, publishing at the nominal 200 ms) but it would be for a workload with genuinely long-idle vCPUs. Raising alpha to compensate was tried and is much worse for other reasons (6.2). **Not resolved; flagged rather than silently changed.**

---

## 7. The result, and the failure mode

**Best configuration found** (all live-verified):

```
# kernel sysctls
ivh_universal_eligible   = 1          # CRITICAL, missing from both launch scripts
ivh_steal_source         = 2          # TSC tick-gap estimator
ivh_cap_source           = 3          # rq->ivh_uc_capacity
ivh_uc_used_source       = 1          # ACCT -- the decisive knob, see 6.1 and 8
ivh_capacity_threshold   = 965
ivh_uc_min_avail_pct     = 10         # default; 40 and 70 are catastrophic
ivh_uc_min_steal_ns      = 500000
ivh_uc_ema_alpha_q16     = 868        # default; faster is catastrophic
ivh_uc_window_ns         = 200000000
ivh_uc_duty_ns           = 0
ivh_tks_deadband_ns      = 50000      # inert in the shipped code, see 6.3
ivh_tks_phase_pct        = 0
ivh_tks_carry_ticks      = 8
ivh_ka_enabled           = 0          # in-kernel keepalive left off, as instructed

# BPF (tools/bpf/MY_ivh_atc.bpf.c)
IVH_CAP_HARDFLOOR = 880   IVH_CAP_TOPBAND = 50   IVH_CAP_MARGIN = 50
ivh_cfg[0] = 3

# userspace
vcap_probe -p 200 -s 5000     (vcap itself NOT running)
```

### 7.1 Head-to-head, matched protocol (45 s idle gap before each run)

| rep | **A** — PV steal page + vcap | **C** — source 2 + `vcap_probe` + ACCT (HF 880) |
|---|---|---|
| 1 | 11.76 s (migr 70442) | **11.80 s** (migr 66180) |
| 2 | 11.57 s (migr 69915) | **11.88 s** (migr 53945) |
| 3 | 12.22 s (migr 56320) | **12.23 s** (migr 56026) |
| 4 | 11.94 s (migr 56949) | **12.33 s** (migr 54695) |
| 5 | 12.16 s (migr 55161) | 13.85 s (migr 48157) |
| 6 | — | 14.40 s (migr 59561) |
| **mean** | **11.93 s** | 12.75 s (first four: **12.06 s**) |

Corunner verified on all 11 rows (contended 7.0-16.2 %, clean 0.1-0.2 %).

**For the first four runs, source 2 is statistically indistinguishable from the steal page** (12.06 vs 11.93 s, against a 13.88 s no-IVH baseline). Then it falls off.

The best individual source-2 runs across all configurations — **11.25, 11.36, 11.39, 11.57, 11.62, 11.80, 11.88 s** — are *at or below* arm A's entire range (11.57-12.22 s). The mechanism is not weaker. Its signal is less durable.

### 7.2 What the decay actually is

Tracked live through the ACCT run sequence (contended / clean mean `ivh_uc_capacity`):

| rep | wall | contended cap | clean cap | migrations |
|---|---|---|---|---|
| 1 | 11.57 s | 745 | **1023** | 68382 |
| 2 | 11.62 s | 856 | 1018 | 58410 |
| 3 | 11.95 s | 837 | 994 | 55889 |
| 4 | 13.15 s | 796 | 946 | 38216 |
| 5 | 13.77 s | 828 | **916** | 42167 |
| 6 | 14.11 s | 884 | 947 | 66234 |

The clean population sinks from 1023 to 916 as IVH loads it. Under source 2 a vCPU that is *doing work* develops phantom steal (its idle becomes bursty), so **IVH's own success degrades the signal that says its destinations are good.** Simultaneously the contended vCPUs, relieved of work, start reading healthier. The gap closes from both ends until the gates stop passing.

This is negative feedback with a time constant comparable to the benchmark length, which is why it presents as run-to-run decay rather than as intra-run oscillation.

Arm A does not do this: vcap recomputes from a forced 200 ms full-demand probe every 5.2 s with its own short EMA, so it re-converges regardless of history. That, not the steal source, is the durable difference.

**Attempted fixes, all live-tested, none sufficient:** lowering `IVH_CAP_HARDFLOOR` (950 → 900 → 880 → 850, buys one extra good run); raising `ivh_capacity_threshold` (990/1000/1010, no better); faster EMA (much worse); higher `min_avail_pct` (catastrophic); looser relative gates (worse); longer idle gaps between runs (helps the *entry* state, does not prevent the decay).

---

## 8. The caveat that limits the whole claim: ACCT is not PV-free

The configuration that works reads `kcpustat` USER+NICE+SYSTEM as its numerator. On this guest **that quantity has PV steal subtracted out of it**: `steal_account_process_time()` runs at the top of `account_process_tick()` and removes the stolen portion of each tick before user/system accounting.

Verified empirically, not just from code: under a full 16-vCPU busy loop, `/proc/stat` charges cpu0-7 **67 % steal** and only the remaining ~33 % to user. If `kcpustat` ignored PV steal, user would read ~100 %.

Consequence, stated plainly: **`ivh_uc_used_source=1` reintroduces exactly the PV dependency that `ivh_steal_source=2` exists to remove.** Source 2 supplies the *denominator's* steal term with no PV and no vPMU, which is real progress — but the numerator is PV-shaped. In a CVM with no steal page, `steal_account_process_time()` returns 0, the full tick is charged to user/system, and contended vCPUs would read *healthier* than they are. **The ACCT result in this document does not transfer to a confidential guest without re-validation.** This is inferred from the accounting path plus the `/proc/stat` measurement above; it has not been tested on an actual CVM, and could not be here.

The genuinely PV-free path is WALL (`ivh_uc_used_source=0`). It reached 11.59 s once, but its capacity separation depends on how loaded the clean vCPUs are and inverts to −68 when they carry work (6.1). **The fully PV-free configuration did not reach the target reliably.**

`ivh_cvm_steal_detector_2026-08-08.md` sec 4.1 anticipated precisely this trap, in the context of a different estimator ("it is circular and was discarded... `kcpustat` USER/SYSTEM *already depends on the `kvm_steal_time` page*"). That warning applies to `ivh_uc_used_source=1` as well, and I did not notice the connection until after the tuning had already selected ACCT on measured merit. Recording it as a correction to my own reasoning rather than burying it.

---

## 9. Honest confidence

The claim to assess: *"IVH now works about as well on pure TSC estimation as it did on the steal page / UNHALTED.REF."*

Decomposed:

| sub-claim | verdict | confidence |
|---|---|---|
| Source 2 can drive IVH to reference-level hackbench performance | **True.** 11.25-11.95 s repeatedly, vs arm A 11.57-12.22 s, vs 13.88 s baseline. Corunner verified on every run. | **High.** Many runs, several configurations, independent corunner check. |
| It does so *stably*, the way the steal page does | **False.** Decays to 13.9-14.4 s after 3-4 runs, in every configuration tried. | **High** that this is real (reproduced in 4 independent run blocks). **Moderate** that it is unfixable — I found no fix, but I did not exhaust the space. |
| The working configuration is PV-free | **False.** `ivh_uc_used_source=1` depends on `kcpustat`, which is PV-adjusted here. Sec 8. | **High** on the mechanism; **not tested on a CVM.** |
| The PV-free variant (WALL) reaches the target | **Not demonstrated.** One 11.59 s run; signal inverts under load. | **Moderate** — WALL was tested less thoroughly than ACCT once ACCT proved better. |
| The tick-gap estimator is ordinally sound | **True and strongly confirmed** — ~100× separation, no overlap (6.4). | **High.** |
| The tick-gap estimator is metrically sound | **False, worse than previously reported.** 6-24 % recovery, near-constant regardless of true steal. | **High.** |

**Overall: I would characterise the claim as roughly half-established, and I am not willing to round it up.**

What is genuinely proven tonight: the probe-only vcap works and is cheap; the pipeline can be tuned so that TSC-only steal detection reaches — and briefly beats — the steal-page reference on the project's own benchmark, under verified contention; and the tick-gap estimator's ordinal signal is excellent.

What is not: sustained parity, and PV-independence of the winning configuration. Those are the two things a headline "IVH works on TSC" would need, and neither is in hand.

### 9.1 What I would do next, in priority order

1. **Add `ivh_universal_eligible=1` to `/home/nick/IVH` and `/home/nick/ivh_mode.sh`** (sec 4). Nothing else matters until this is done, and any post-`298be1454` measurement taken through those scripts should be treated as suspect.
2. **Decide whether the deadband is supposed to gate accumulation** (6.3). If yes it is a one-line kernel fix, and it is the most plausible route to reducing the load-induced phantom that drives the decay in sec 7.2 — which would attack the durability problem at its root, on the PV-free WALL path.
3. **Make the destination signal load-invariant.** The decay is the clean population sinking as it is loaded. `ivh_uc_gate_recalibration_2026-08-03.md` sec 8's "publish steal/elapsed instead" is aimed at exactly this and is still unimplemented.
4. Re-validate WALL properly with the tuning learned here, since it is the only PV-free path.

---

## 10. Exact state left on the machine

**Running:** `vcap_probe -p 200 -s 5000`, `MY_ivh_atc` (with `IVH_CAP_HARDFLOOR=880`), `vsched_module.ko`. Sysctls as listed in sec 7. `ivh_ka_enabled=0`.

**Working-tree changes, uncommitted (nothing was committed or pushed):**

- `tools/bpf/MY_ivh_atc.bpf.c` — `IVH_CAP_HARDFLOOR` 950 → 880 only. `TOPBAND`/`MARGIN` back at their original 50/50.
- `tools/bpf/vmlinux.h` — regenerated from the running kernel's BTF (the previous one was from 2026-08-03 and predated the `ivh_tks_*` rq fields). Previous version preserved as `tools/bpf/vmlinux.h.bak-2026-08-08`.
- `tools/bpf/docs/ivh_final_tsc_only_build_2026-08-08.md` — this file.
- `/home/nick/vsched_main/vcapacity/vcap_probe.cpp` + `Makefile` target (new; `main.cpp` and `vcap` untouched).

**Kernel source was not touched.** `core.c`, `cputime.c`, `fair.c`, `sched.h` still carry only the pre-existing uncommitted work from earlier tonight; their mtimes (02:12-02:13) predate this session (03:06). No reboot was performed or is required by anything here.

**Built during the session (not a kernel build):** `vsched_module.ko` for `6.17.0-rseqport71-byeunhalt+`, which had no cached module — `setup.sh` fails without it, so nothing could run at all. Installed to `/lib/modules/6.17.0-rseqport71-byeunhalt+/extra/`.

**Backup:** `/home/nick/vsched_main/vcapacity_ORIGINAL_BACKUP_2026-08-08/` — verified md5-identical to the untouched working copy at session end, and proven runnable on this kernel (sec 3.3).

To revert to the known-good reference configuration:

```sh
sudo /home/nick/ivh_mode.sh steal
echo 1 | sudo tee /proc/sys/kernel/ivh_universal_eligible     # NOT done by the script
```
