# The WALL-path phantom is un-ticked busy time, the bad basin has a findable trigger, and the dominant uncontrolled variable is the host — no reproduced fix

**Date:** 2026-08-09 (session ~01:50–03:10 wall, following `ivh_wall_path_calibration_2026-08-09.md`)
**Kernel under test:** `6.17.0-rseqport71-byeunhalt+` (live), branch `kernel-43-clean`, base commit `6c3874293`
**Status:** ~110 real hackbench runs, all live, per-run PV-steal capture. No replay, no simulation. Everything labelled measured was executed on this machine tonight. Kernel source untouched; BPF source untouched; nothing committed or pushed.

---

## 0. The answer, up front

The task asked for one of three verdicts. The honest answer is a **fourth** one, and it is more specific than any of the three:

> **I did not find a reproduced fix.** But the failure is no longer a mystery: I found the *exact* mechanism that produces the phantom steal, confirmed it by three independent routes, and confirmed by direct experiment that the estimator is **not** resolution-limited — with the right load shape it is *exact*. The condition under which it fails is specific and nameable: **non-idle time that never receives a scheduler tick.** Two live interventions that remove that condition were built and measured; both work perfectly on the signal and both are **prohibitively expensive on throughput**, for reasons I measured rather than guessed. And the single largest source of block-to-block variance is not in the guest at all: **the host's contention character moved by ~75 % in benchmark terms during this session while the project's corunner check moved by less than 5 points.**

Explicitly against the three offered verdicts:

| offered verdict | applies? |
|---|---|
| "I found a reproduced, working fix" | **No.** One candidate (a 100 %-duty probe prime) produced the equal-best block of the night, 11.557 s, and then failed to reproduce at 13.012 s from a near-identical entry state. Recorded as a failure, sec 7. |
| "A kernel reboot is genuinely required, and here is exactly what change" | **Partly, and I can now be exact about it** — sec 8. But it is *not* the honest headline, because sec 9 shows a live experiment that must be run first, and it needs host-side access rather than a reboot. |
| "Something is genuinely wrong with the measurement" | **The user's correction is right and my evidence supports it.** It is *not* a resolution ceiling. Under a continuous load the estimator reads clean vCPUs at **raw 1024 exactly** and contended vCPUs at **490**, i.e. their true steal, under full hackbench. The signal is excellent. It is destroyed by a specific, identifiable property of the *workload*, not by noise. Sec 3. |

---

## 1. Method and starting state

Reference command, unchanged: `/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000`.

Harness `runbench.sh` (session scratchpad) records per run: wall time from `ivh_exec`'s own `Time:` line; per-vCPU PV steal delta from `/proc/stat` across exactly that run (the corunner check, independent of everything under test); pre- and post-run `ivh_uc_capacity` split contended (0-7) / clean (8-15) with contMAX/cleanMIN; and the `ivh_migrations_done` delta. Raw rows in `results.tsv`.

**Starting state verified before anything was touched** and matched `ivh_wall_path_calibration_2026-08-09.md` sec 9 exactly: all 15 documented sysctls (`ivh_universal_eligible=1`, `ivh_steal_source=2`, `ivh_cap_source=3`, `ivh_uc_used_source=0` (WALL), `ivh_capacity_threshold=1010`, `ivh_uc_min_steal_ns=500000`, `ivh_uc_ema_alpha_q16=868`, `ivh_uc_window_ns=200000000`, `ivh_uc_min_avail_pct=10`, `ivh_tks_deadband_ns=50000`, `ivh_tks_phase_pct=0`, `ivh_tks_carry_ticks=8`, `ivh_ka_enabled=0`, `ivh_ref_steal_enabled=0`, `ivh_uc_enabled=1`); BPF `IVH_CAP_HARDFLOOR 880`, `TOPBAND 50`, `MARGIN 20`, `MARGIN_REL 0`; one `MY_ivh_atc`, one `vcap_probe -p 200 -s 5000`, `ivh_cfg=3`. The machine was sitting in the **good** state the calibration session left it in: `cont≈868, clean=1023` with all eight clean vCPUs pinned at 1023.

**Corunner measured three times** (all 16 vCPUs to a busy loop, `/proc/stat` steal delta): 62.8–63.2 % / 0.2 % at session start; 57–60 % mid-session; 61 % / 0.1 % at session end. Present and stable throughout by this metric — which sec 9 shows is the wrong metric.

---

## 2. First result: the "perfect" entry state is not sufficient

The very first block, shipped config, 5 s gap, entered from `cont 877 / clean 1021 / cleanMIN 1020` — a textbook good entry state:

| run | wall | pre cont / clean | migr | cont PV steal | post cont / clean |
|---|---|---|---|---|---|
| 1 | 14.018 | 877 / 1021 | 45 609 | 18.1 % | 905 / **927** |
| 2 | 15.881 | 905 / 929 | 22 782 | 23.5 % | 861 / 844 |
| 3 | 15.789 | 861 / 849 | 24 380 | 23.6 % | 830 / 795 |
| 4 | 15.287 | 832 / 802 | 22 381 | 23.9 % | 794 / 775 |
| 5 | 15.488 | 789 / 782 | 22 185 | 24.1 % | 750 / 755 |
| 6 | 16.593 | 751 / 761 | 20 412 | 23.8 % | 720 / 711 |
| 7 | 16.151 | 725 / 721 | 22 941 | 23.5 % | 730 / 704 |
| 8 | 16.042 | 733 / 715 | 23 052 | 23.3 % | 733 / 713 |

**n=8, mean 15.656 s, sd 0.724.** IVH-off baseline measured 20 minutes later: **14.715 s (n=3, sd 0.078)**. So IVH was *net harmful*.

The headline is run 1: **the destination population fell 1021 → 927 in a single 14 s run.** Nothing in the previous reports describes a collapse that fast. Arithmetic on the EMA (α=868/65536, 200 ms window, ~70 samples) says the clean vCPUs must have published a raw ratio of ≈866 throughout that run, against the 1024 they publish at idle. That number — *what raw value do clean vCPUs publish while carrying load* — became the whole investigation.

---

## 3. The mechanism: phantom steal is un-ticked busy time

### 3.1 A four-point load-shape ladder

`raw_wall` (`/proc/ivh_debug` `ivh_uc_cpu:` column 8, the un-EMA'd published ratio) sampled at 100 ms on the eight **clean** vCPUs (0.2 % genuine PV steal), under four different load shapes:

| load on cpu8-15 | mean raw_wall | % of samples at 1024 |
|---|---|---|
| **A.** eight 100 %-busy spinners, never block | **1020.6 – 1022.7** | 97–98 % |
| **B.** hackbench pinned to cpu8-15 (16 threads on 8 CPUs — oversubscribed, so mostly continuously busy) | 982.7 – 995.6 | 43–69 % |
| **C.** 50 % duty in **5 ms** continuous chunks | 1009.2 – 1014.1 | 95 % |
| **D.** full unpinned hackbench (≈1 thread/CPU, blocking on pipes constantly) | **666.7 – 709.2** | **0 %** |

Same CPUs, same corunner, same estimator, same 0.2 % real steal. The published capacity swings from 1022 to 690 purely on the **shape** of the guest's own load. Coarse busy time → no phantom. Fine-grained blocking → 33 % phantom.

For reference, the contended vCPUs in run D published 528.7–591.1, so the clean/contended separation collapses to ~155 points even in the natural, IVH-off case.

### 3.2 Why: the estimator's model is "one tick = one TICK_NSEC of runnable time", and NO_HZ breaks it

`ivh_tick_steal_accumulate()` (`core.c:2230-2315`) computes, between consecutive scheduler ticks on a CPU:

```
avail_c  = (raw TSC delta) − (get_cpu_idle_time_us + iowait delta)
excess_c = avail_c − tick_c            /* tick_c = TICK_NSEC in cycles */
```

with positive carry drained in full and negative carry floored at `−carry_ticks` ticks. The model is exact only if the CPU receives one tick per millisecond of non-idle time. Under `CONFIG_NO_HZ_IDLE` it does not: **busy time chopped into fragments shorter than a tick period frequently misses the jiffy boundary entirely, so the CPU accrues `avail` without accruing a matching tick.** The estimator has no way to distinguish that from host preemption — in both cases it sees "more runnable time elapsed than ticks arrived".

### 3.3 Confirmed directly, by two more independent routes

**Route 2 — local timer interrupt counts.** `/proc/interrupts` `LOC` deltas per CPU over a 10.025 s window inside a full hackbench run:

| | LOC ticks | idle (ms) | avail = elapsed − idle | ticks × 1 ms | implied phantom | real PV steal |
|---|---|---|---|---|---|---|
| cpu8 | 3 195 | 3 660 | 6 365 ms | 3 195 ms | **3 170 ms** | **20 ms** |
| cpu12 | 2 768 | 4 620 | 5 405 ms | 2 768 ms | 2 637 ms | 10 ms |
| cpu0 (contended) | 2 018 | 4 100 | 5 925 ms | 2 018 ms | 3 907 ms | 3 520 ms |

On cpu8 the estimator books **3.17 seconds of "steal" in ten seconds on a vCPU with 20 milliseconds of real steal.** On cpu0 the same arithmetic gives 3.9 s against 3.5 s of genuine steal — i.e. the contended reading is *mostly real*, the clean reading is *almost entirely artefact*.

**Route 3 — the `/proc/stat` accounting hole.** Over that same window, cpu8's own `/proc/stat` columns sum to only **7 200 ms of the 10 025 ms of wall time** (busy 3 520 + idle 3 660 + steal 20). At idle, the same sum is exact (600 units over 6.00 s, checked). The missing **2 825 ms** is, by construction, non-idle time that received no tick — because tick-driven accounting is the only thing that charges busy time. That hole matches the 3 170 ms phantom of route 2 to within the sampling error of the method.

Three routes — load-shape ladder, interrupt counts, and the guest's own cputime accounting — give the same answer with the same magnitude. **The phantom steal on a clean vCPU is, essentially exactly, the guest's own un-ticked busy time.**

### 3.4 This reframes `ivh_wall_path_calibration` sec 5.1 rather than contradicting it

That report measured per-window `win_stolen_c` distributions on clean (1.4–11.4 ms median) and contended (3.2–13.0 ms) vCPUs and found them almost completely overlapping, concluding no `min_steal_ns` threshold can separate them. That measurement is correct and I reproduce it. What is new is that **the overlap is not noise** — the clean side of it is a deterministic function of how fragmented the destination's load is, and it goes to zero when the load is coarse. Which is exactly the user's framing: this is a findable condition, not a resolution floor.

---

## 4. Two live interventions that make the measurement exact — and what they cost

### 4.1 Continuous SCHED_IDLE filler on all 16 vCPUs: measurement perfect, throughput ruined

`vcap_probe -p 1000 -s 0` makes its 16 already-`SCHED_IDLE` workers spin continuously instead of 200 ms in 5 200 ms. No rebuild — `-s 0` was already supported. The vCPU then never idles, `avail == elapsed`, and the tick-gap excess *is* the host preemption.

Measured, idle guest:

| | contended cpu0-7 raw | clean cpu8-15 raw |
|---|---|---|
| `-p 200 -s 5000` (shipped) | 580 – 600 | 1024 |
| `-p 1000 -s 0` (continuous) | **443 – 456** (= the true ~56 % steal) | **1024** |

Measured, **during a full unpinned hackbench** (the load shape that produced 690 in sec 3.1):

| | contended raw | clean raw | separation |
|---|---|---|---|
| shipped probe | 528 – 591 | 667 – 709 | ~155 |
| **continuous filler** | **486 – 494** | **1018 – 1021** | **~530** |

That is the estimator working *perfectly*: clean vCPUs at the rail, contended vCPUs at their true steal ratio, under real load. **This is the direct disproof of "the measurement is fundamentally limited".**

**The cost, measured:** IVH-off hackbench went from **14.715 s** (n=3, sd 0.078) to **39.477 s** (n=3, sd 0.547) — 2.7×. Contended PV steal read 66.5–66.9 % instead of 32.5–32.9 %, because a vCPU that never halts is *always* eligible to be stolen from. The guest loses the host's sleeper bonus on the eight contended pCPUs.

### 4.2 Filler restricted to the uncontended vCPUs: still ruinous, and the reason is guest-side

If the cost were purely host-side, restricting the filler to cpu8-15 (whose pCPUs have no competition) should be free. Eight `SCHED_IDLE` spinners pinned to cpu8-15, verified `IDL` policy at 100 % each:

- clean raw under hackbench: **1011.9 – 1016.0** (still fixed);
- IVH-off hackbench: **24.010 s** (n=3, sd 0.225) vs 14.715 s. Still 63 % worse.

So the dominant cost is **inside the guest**: a running `SCHED_IDLE` task makes `idle_cpu()` / `available_idle_cpu()` false, so `select_idle_sibling()` loses the idle-CPU fast path that hackbench's wakeup pattern depends on. Priority does not help — the task yields instantly, but the CPU is no longer *idle*, and that is what the wakeup path tests.

**Conclusion: "never idle" fixes the signal and cannot be shipped.** Both variants measured, both rejected.

### 4.3 A 1 ms per-CPU timer (restore ticks without adding demand): breaks the other end

If the problem is missing ticks, arm a timer that keeps `tick_nohz_stop_tick()` from stopping the tick. `tickalive.c` (16 threads, one pinned per vCPU, `SCHED_IDLE`, absolute-deadline `clock_nanosleep` at 1 ms) — the CPU still halts between wakeups, so `idle_cpu()` stays true.

It works on the clean side and destroys the contended side:

| config | contended raw | clean raw | separation |
|---|---|---|---|
| baseline | 528 – 591 | 667 – 709 | 155 |
| tickalive, `carry_ticks=8` | **990** | 1023 | **33** |
| tickalive, `carry_ticks=1` | 788 | 977 | 189 |

Local timer interrupts rose to 1 190–1 744/s. Because the estimator's expectation is a *fixed* `TICK_NSEC` per tick, delivering ticks faster than HZ makes every interval's excess systematically negative; the bounded negative carry then absorbs genuine steal, and the contended signal reads 990 on vCPUs that are 60 % stolen. **Rejected.** (It also explains why `nohz=off` cannot simply be bolted on — see sec 8.)

---

## 5. Things the previous reports flagged as open, now answered

### 5.1 The IVH-off warm-up lead (`calibration` sec 7.1 / sec 8 item 0) — **refuted**

That report's top-priority next experiment was: three hackbench rounds with `ivh_universal_eligible=0` converge the destination population to 1023 in tens of seconds, so use it as a warm-up. Run deliberately, from a collapsed state (`cont 690 / clean 869`):

| run | wall | pre cont / clean | post clean |
|---|---|---|---|
| 1 | 14.738 | 690 / 869 | 848 |
| 2 | 14.611 | 770 / 850 | 845 |
| 3 | 14.797 | 802 / 850 | 844 |

**The clean population went down, not up** (869 → 844), and stuck. It does not converge the destinations. The sec 7.1 observation was a coincidence of that machine state; sec 3 of this document explains why it cannot be relied on — with IVH off, hackbench spreads ~1 thread per CPU, which is precisely the *maximally fragmented* shape (row D of the ladder), so IVH-off running is the worst possible warm-up, not the best. **Do not put this in `/home/nick/IVH`.**

(Useful by-product: tonight's IVH-off baseline, **14.715 s**, against the calibration session's 15.87 s.)

### 5.2 "Is vcap interrupting migrations?" — **no, measured**

Interleaved A/B with IVH on, 5 s gap, `vcap_probe` alternately running and `SIGSTOP`ped, three pairs:

| | wall (3 runs) | mean |
|---|---|---|
| vcap_probe **running** | 11.878, 12.194, 12.555 | **12.209 s** |
| vcap_probe **SIGSTOPped** | 12.081, 12.056, 13.169 | **12.435 s** |

Migrations were in the same 49–57 k band in both arms. Pausing the probe does not speed anything up; if anything it is marginally worse. Neither lock contention nor accounting interference from the probe is detectable. **Ruled out.**

On the related concern: the `Voluntary context switch within RCU read-side critical section!` warning (`rcu_note_context_switch+0x532`, from `bpf_sched_pre_lock_migrate` → `set_cpus_allowed_ptr`) appears **exactly once** in `dmesg`, at uptime 3073 s, and has not recurred across this session's ~110 runs and several million further migrations (`ivh_migrations_done` advanced from 4.30 M to 8.11 M). It is real but rare, and it is not correlated with the probe. It remains worth fixing on its own merits; it is not tonight's failure.

On sec 3.1's accounting question — is the probe's own execution counted differently from genuine idle? Yes, and *deliberately*: the probe's 200 ms spin is counted as fully-available, tick-complete time, which is exactly why idle clean vCPUs publish raw 1024. The probe is not causing the clean/contended overlap; it is the only thing currently suppressing it.

### 5.3 "Maybe the half-tick / deadband / carry machinery is over-compensating" — **the carry is load-bearing**

`ivh_tks_phase_pct` is already 0, and with it at 0 the deadband is provably inert (`calibration` sec 6.3, code-confirmed). So the only live question is the bounded carry. A/B, shipped vs bare-bones, with a repeat of the shipped arm as a reproduction check:

| arm | `deadband` / `carry_ticks` | n | mean | sd | population behaviour |
|---|---|---|---|---|---|
| **S** shipped | 50 000 / 8 | 4 | **11.556 s** | 0.087 | clean pinned 1023 all four runs, migr 66–67 k |
| **B** bare-bones | 0 / 1 | 4 | 12.399 s | 0.396 | clean sagged 1023 → 998, migration storms to 79.6 k |
| **S2** shipped again | 50 000 / 8 | 4 | **11.634 s** | 0.307 | **recovered** the population 999 → 1022 |

Removing the bounded carry is measurably worse and visibly destabilising, and restoring it repairs the damage within four runs. **It is genuine noise suppression, not complexity papering over a bad measurement.** (The deadband can be deleted with no effect at `phase_pct=0`; that is a tidiness change, not a fix.)

### 5.4 Genuinely untested calibration axes, tried

- **`ivh_uc_window_ns` swept downward** with `ivh_uc_ema_alpha_q16` scaled to preserve the wall-clock half-life: 200 ms/868 → 50 ms/217 → 20 ms/86 → 10 ms/43. Motivation was real: phantom scales with window length while the 500 µs `min_steal_ns` guard does not, so a shorter window should make the guard operative again. Measured separation (contended vs clean raw, under load) went **48 → 14 → 31 → 30**; published contended EMA rose monotonically 907 → 947 → 960 → 968, i.e. shorter windows *reduce* the separation. No help. Reverted.
- **`ivh_tks_carry_ticks` 1 / 2 / 8**, with and without the tick timer of sec 4.3 — see 4.3 and 5.3.
- **A fresh `/home/nick/IVH` relaunch immediately before a block**, tested as a candidate trigger since every reported failure followed one: n=6, mean **11.710 s**, clean pinned at 1022 throughout. **Refuted.**

---

## 6. The trigger, reproduced: the first run after a long guest idle craters the destinations

This is the "specific findable condition" and it does reproduce.

Intervention: from a warm, good state, idle the guest (a single `sleep`, no polling) for 5 minutes, then run a measured block. Done twice.

| | entry after idle | run 1 pre → post clean | block mean |
|---|---|---|---|
| **IDLE_ENTRY1** | cont 875 / clean 1014 | **1013 → 912** | 14.514 s (n=6, sd 0.840) — migrations hit **0** by run 4 |
| **IDLE_ENTRY2** | cont 871 / clean 1023 (cleanMIN 1023) | **1023 → 941** | 12.422 s (n=6, sd 0.148) — stabilised at ~910, no shutdown |

Contrast with runs from a benchmark-warm state, same config, same gap:

| block | run 1 pre → post clean | block mean |
|---|---|---|
| BARE_S | 1023 → 1023 | 11.556 s |
| BARE_S2 | 999 → 1012 (climbing) | 11.634 s |
| RELAUNCH1 | 1022 → 1022 | 11.710 s |
| HF880_A | 1021 → 1022 | 11.557 s |

**The signature is unambiguous and reproduced: the first hackbench run after ≥5 minutes of guest idle costs the destination population ~100 points; the same run from a warm state costs it nothing.** Whether that ~100-point hit then turns into a full shutdown (IDLE_ENTRY1) or merely a degraded plateau (IDLE_ENTRY2) is the part that is *not* deterministic.

Mechanistically this is sec 3 running in the bad direction: after an idle period the destinations' first loaded windows carry the cold, maximally-fragmented, thread-spread-wide phase of hackbench, which is load shape D; from a warm state IVH is already feeding them coarse, continuously-runnable lock-waiters, which is load shape A/B. Once the EMA has taken the hit, the 10.4 s half-life plus the ~2-windows-per-5.2 s idle publish rate means it cannot climb back inside a run.

**What this does *not* explain:** the entry-capacity hypothesis I built on top of it is wrong, see next section.

---

## 7. The candidate fix that failed to reproduce — recorded as a failure

Sec 3 predicts a cheap primer: run the probe at 100 % duty for ~60 s before benchmarking, so every vCPU publishes tick-complete windows and the destination population is pinned at the rail, then revert to `-p 200 -s 5000`. It costs no throughput during the measured runs. It has a second attraction: it also drives the *contended* population down to its true value (~480), which the "wide separation is dangerous" reading of the gate would predict is bad — so the experiment discriminates between two hypotheses at once.

| block | primer | entry cont / clean / cleanMIN | n | mean | sd |
|---|---|---|---|---|---|
| **HF880_A** | 60 s of sixteen 100 % `SCHED_IDLE` spinners | **506 / 1021 / 1020** | 6 | **11.557 s** | 0.136 |
| **PRIME2** | 60 s of `vcap_probe -p 1000 -s 0` | **477 / 1014 / 1012** | 6 | **13.012 s** | 0.395 |

HF880_A is the equal-best block of the night: 11.584, 11.317, 11.497, 11.534, 11.663, 11.749, with the clean population pinned at 1022 through all six runs and migrations in the canonical 64–69 k band, entered from the *lowest source reading of the session*. PRIME2, from a near-identical entry state with the same 100 %-duty primer, degraded monotonically 12.56 → 13.53 with the clean population falling 1014 → 858.

**Two blocks, one procedure, near-identical entry states, 11.56 s and 13.01 s.** Exactly the shape of `calibration` sec 6's `IVH_CAP_MARGIN_REL` result. Had I stopped after HF880_A I would have reported a fix that does not exist. **Not shipped. `/home/nick/IVH` and `ivh_verify.sh` were not edited.**

HF880_A also kills the hypothesis I had built from sec 6 — that a low contended reading widens the gate (`dest ≥ src + MARGIN`) and gives the destinations too much room to degrade before the gate closes. Entry at `cont 506` gives the destinations *517 points* of room and produced the best block of the night. **Entry capacity, on either side, does not determine the basin.**

---

## 8. What a reboot would buy, exactly — and why it is not simple

The mechanism of sec 3 has one clean structural cure: guarantee a tick every millisecond regardless of idleness. That is `nohz=off`, a **boot parameter, not a kernel source change**. With a periodic tick, consecutive ticks are exactly `TICK_NSEC` apart in the absence of steal, so `avail_i = TICK − idle_i ≤ TICK` and `excess_i ≤ 0` for any vCPU that is not being preempted; steal appears only as genuine tick delay. The phantom becomes structurally impossible, and sec 4.1 is the empirical proof of the "ticks present ⇒ no phantom" half of that. Unlike the filler, the vCPU still halts between ticks, so it keeps the host sleeper bonus and `idle_cpu()` stays true.

**But `nohz=off` alone will silently disable the estimator entirely**, and this is the specific thing worth knowing before spending the reboot. `ivh_tick_steal_accumulate()` (`core.c:2241-2251`) bails on every tick when `get_cpu_idle_time_us()` returns `-1`:

```c
	idle_us   = get_cpu_idle_time_us(cpu, NULL);
	iowait_us = get_cpu_iowait_time_us(cpu, NULL);
	if (unlikely(idle_us == (u64)-1 || iowait_us == (u64)-1)) {
		rq->ivh_tks_skipped++;
		return;
	}
```

and `get_cpu_sleep_time_us()` returns `-1` precisely when `!tick_nohz_active`, which is what `nohz=off` sets. So the reboot must carry a **matching one-block source change**: on that path, fall back to `kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE] + [CPUTIME_IOWAIT]` instead of returning. Under a periodic tick that fallback is *better* than the NO_HZ source, because with no skipped ticks there is no accounting hole (verified tonight: the hole is zero at idle, 28 % of wall under fragmented load) — the two quantities the estimator needs become consistent for the first time.

Sec 4.3 is the reason not to try to approximate this from userspace: any scheme that delivers ticks at a rate *other than* HZ makes the fixed `TICK_NSEC` expectation systematically wrong in the negative direction and erases the contended signal. Only an exactly-periodic HZ tick has the right property, and that is a boot-time decision.

**Second, independent kernel-side option**, if `nohz=off`'s cost (1 000 timer interrupts/s per idle vCPU) turns out to be unacceptable: make the estimator refuse the intervals where its own model does not hold, i.e. accumulate only across tick intervals with `d_idle_c == 0`, and restrict `ivh_uc_tick()`'s `avail` accumulation to the same intervals so the published ratio stays consistent. That is a real change of maybe 15 lines across `ivh_tick_steal_accumulate()` and `ivh_uc_tick()`, and it also needs a reboot. It is strictly more work than `nohz=off` and should only be attempted if `nohz=off` validates the mechanism first.

**Neither can be tested live**, and I checked: `nohz` has no runtime toggle, `tick_nohz_active` is not writable, and the sysctl surface (`ivh_tks_deadband_ns`, `ivh_tks_phase_pct`, `ivh_tks_carry_ticks`, `ivh_uc_*`) contains nothing that changes what the estimator *expects* per tick.

---

## 9. The finding that should be acted on before any reboot: the corunner check is blind

Late in the session the IVH-off baseline had moved from **14.715 s** to **25.092 s** (n=3, sd 0.528), and an interleaved on/off pair block confirmed it: PAIR_OFF 25.996 s, PAIR_ON 26.421 s. The corunner check read **61 % / 0.1 %** — statistically identical to the 62.8–63.2 % measured at session start. No stray guest load (`ps` clean, single `MY_ivh_atc`, single `vcap_probe`, no D-state tasks, all sysctls verified unchanged).

The decisive control:

| | 02:15 | session end |
|---|---|---|
| hackbench confined to the **eight uncontended** vCPUs (`taskset -c 8-15`) | **10.877 s** | **10.854 s** |
| hackbench across all 16, IVH off | **14.715 s** | **25.996 s** |

**The guest is byte-for-byte as fast as it was.** Everything that changed is on the eight contended pCPUs, and it changed the benchmark by 77 % while moving the steal percentage by less than 5 points. A corunner that switches from a few long-running threads to many short ones — or a host that acquires any other latency-generating load — is invisible to a steal-percentage check but dominates hackbench, which is latency-bound, not throughput-bound.

Two consequences, and I think they are the most important practical output of tonight:

1. **Every absolute wall-time comparison in this project between blocks taken more than a few minutes apart is unsafe.** That includes `calibration` arm E (11.41 s) vs arm G (14.05 s), and tonight's HF880_A (11.557 s) vs PRIME2 (13.012 s). A large fraction of what has been called "bistability" may be the host moving under a fixed measuring stick. I cannot quantify what fraction, because I have no host-side visibility — but the effect is at least as large as the entire phenomenon under study.
2. **The bad basin becomes self-sealing when the host is bad.** At session end the *whole* population, contended and clean alike, had fallen to 452–588 — every vCPU below `IVH_CAP_HARDFLOOR = 880` — so the destination scan rejects everything and `ivh_migrations_done` is exactly **0** even with `ivh_universal_eligible=1`. IVH switches itself off precisely when the host is worst. That is a genuine and reachable failure mode of the absolute hard floor, and it is BPF-side and reboot-free to address (`calibration` sec 8 item 3's volume regulator, or a population-relative floor), though I did not get a reproduced result for it tonight.

**Concrete recommendation, and it is cheap: make `ivh_verify.sh` take three IVH-off runs inside the same batch and assert a *ratio*, and add `taskset -c 8-15` hackbench as a guest-health control.** `calibration` sec 8 item 1 proposed the first half; tonight's data makes it a requirement rather than a preference, and adds the second half. I did not apply it — it changes a pass/fail criterion, which is the user's call.

---

## 10. Every block run tonight

Corunner verified per run. IVH-off baselines: **14.715 s** early, **25.092 s** late (sec 9).

| block | config | gap | entry cont / clean | n | mean | sd |
|---|---|---|---|---|---|---|
| CTRL1 | shipped | 5 s | 877 / 1021 | 8 | 15.656 | 0.724 |
| WARM_OFF | IVH **off** | 5 s | 690 / 869 | 3 | **14.715** | 0.078 |
| FILL_OFF | IVH off, 100 % filler ×16 | 5 s | 470 / 1019 | 3 | 39.477 | 0.547 |
| FILL8_OFF | IVH off, 100 % filler on cpu8-15 | 5 s | 718 / 1019 | 3 | 24.010 | 0.225 |
| VCAP_RUN / VCAP_STOP | shipped, probe A/B interleaved | 5 s | 733 / 1023 | 3+3 | 12.209 / 12.435 | 0.277 / 0.519 |
| BARE_S | shipped (deadband 50 k, carry 8) | 5 s | 941 / 1023 | 4 | **11.556** | 0.087 |
| BARE_B | bare-bones (deadband 0, carry 1) | 5 s | 961 / 1023 | 4 | 12.399 | 0.396 |
| BARE_S2 | shipped, repeat | 5 s | 962 / 999 | 4 | **11.634** | 0.307 |
| RELAUNCH1 | shipped, straight after `/home/nick/IVH` | 5 s | 945 / 1022 | 6 | 11.710 | 0.290 |
| IDLE_ENTRY1 | shipped, after 5 min idle | 5 s | 874 / 1013 | 6 | 14.514 | 0.840 |
| HF880_A | shipped, after 60 s filler primer | 5 s | 506 / 1021 | 6 | **11.557** | 0.136 |
| IDLE_ENTRY2 | shipped, after 5 min idle | 5 s | 871 / 1023 | 6 | 12.422 | 0.148 |
| PRIME2 | shipped, after 60 s 100 %-duty probe primer | 5 s | 477 / 1014 | 6 | 13.012 | 0.395 |
| BASE_LATE | IVH **off** | 5 s | 699 / 865 | 3 | **25.092** | 0.528 |
| PAIR_ON / PAIR_OFF | interleaved on/off, population below hard floor | 5 s | 542 / 656 | 3+3 | 26.421 / 25.996 | 0.324 / 0.711 |

Best single run: 11.317 s. Worst (excluding the deliberate filler arms): 26.9 s.

---

## 11. Honest confidence

| claim | verdict | confidence |
|---|---|---|
| Phantom steal on a clean vCPU is un-ticked busy time | **True** | **High** — three independent routes (load-shape ladder, `/proc/interrupts` LOC deficit, `/proc/stat` accounting hole) agree on mechanism *and* magnitude |
| The estimator is resolution-limited / fundamentally broken | **False** | **High** — under continuous load it reads clean vCPUs at raw 1024 and contended at their true 490, under real hackbench |
| Making the vCPU never idle fixes the signal | **True** | **High** — measured on all-16 and clean-only variants |
| …and is unusable | **True** — 39.5 s and 24.0 s vs 14.7 s | **High** — n=3 each, tight sd; guest-side cause identified (`idle_cpu()` / `select_idle_sibling`) |
| A 1 ms per-CPU timer is a viable substitute | **False** — separation 155 → 33 | **Moderate-high** — measured at two carry settings |
| The IVH-off warm-up (`calibration` sec 7.1) converges the destinations | **False** — 869 → 844 | **High** for this machine state; mechanism in sec 3 explains why it cannot be relied on generally |
| `vcap_probe` interferes with migrations or capacity accounting | **False** | **Moderate-high** — interleaved SIGSTOP A/B, n=3 pairs; not a large-n result but the sign is wrong for interference |
| The bounded carry is load-bearing | **True** — 11.556 / 12.399 / 11.634 | **Moderate-high** — n=4 arms with a reproduction of the control |
| A long guest idle before a block craters the destination population | **True** — 1013 → 912 and 1023 → 941, vs no change from warm | **High** — reproduced, n=2 interventions, clean contrast against four warm blocks |
| …and that reliably produces the full collapse | **False** — 14.51 s once, 12.42 s once | **High** that it is not reliable |
| The 100 %-duty primer is a fix | **Not demonstrated** — 11.557 then 13.012 | **High** that it is not demonstrated |
| Entry capacity (either population) determines the basin | **False** | **High** — the lowest source entry of the night (506) gave the best block |
| The host's contention character changed materially mid-session | **True** — 14.7 → 26.0 s IVH-off with the uncontended-only control flat at 10.87 → 10.85 s | **High** — the control is decisive |
| …and it is invisible to the project's corunner check | **True** — 63 % → 61 % across that change | **High** |
| `nohz=off` + the `kcpustat` idle fallback would fix the estimator | **Predicted, untested** | **Moderate** — the mechanism is established and sec 4.1 proves the "ticks ⇒ no phantom" half, but the composite has never been run |

**Overall.** The user's instinct was right on both counts. The estimator is not broken and it is not resolution-limited — it is exact when the load is coarse and catastrophically wrong when it is fine-grained, and I can now say precisely why, in one sentence: *it charges the guest's own un-ticked busy time as host preemption.* And there is a findable trigger, which reproduces: a long guest idle before a block. What I could not do is turn either into a fix without a reboot, because every live lever that restores ticks either destroys the wakeup fast path or breaks the estimator's fixed per-tick expectation. And I would not spend the reboot yet: sec 9 says the measuring stick moved by more than the effect under study, and that is worth a night of host-side instrumentation before it is worth a boot parameter.

---

## 12. Exact state left on the machine

Restored and verified by read-back.

- **Sysctls:** all 15 read back identical to sec 1. `ivh_universal_eligible` was toggled for baselines and is back at **1**. `ivh_uc_window_ns` 200000000, `ivh_uc_ema_alpha_q16` 868, `ivh_tks_carry_ticks` 8, `ivh_tks_deadband_ns` 50000 — all restored after their sweeps.
- **BPF:** `tools/bpf/MY_ivh_atc.bpf.c` is **byte-identical to the session-start file** (`diff` clean against a copy taken before anything ran). `IVH_CAP_HARDFLOOR` is still 880, `MARGIN` 20, `MARGIN_REL` 0. No rebuild or reload was performed at any point tonight. One `MY_ivh_atc`, `ivh_cfg` = 3.
- **`vcap_probe`:** exactly one instance, back at `-p 200 -s 5000`. The `-p 1000 -s 0` primer and its zombie were cleaned up.
- **Userspace load:** all `filler.sh` spinners and `tickalive` killed and verified gone (`ps` count 0).
- **Kernel source:** **not touched.** `core.c`, `cputime.c`, `fair.c`, `sched.h` carry only the pre-existing uncommitted work from earlier sessions.
- **`/home/nick/IVH` and `/home/nick/ivh_verify.sh`: not edited.** No confirmed fix exists to ship; sec 9's `ivh_verify.sh` change is a pass-criterion change and is left as a recommendation.
- **No reboot performed. Nothing committed or pushed.**
- New files: this document; `tickalive.c`, `runbench.sh`, `rawsamp.sh`, `results.tsv` in the session scratchpad.

**Caveat on the machine's current condition:** the guest was left with the whole capacity population below `IVH_CAP_HARDFLOOR` (452–588) and IVH consequently making zero migrations, because the host is in the heavy state of sec 9. This is not a configuration problem — the config is correct and verified — and it will clear when either the host load drops or the population is re-primed. Check with:

```sh
grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '{n=$2+0; if(n<8){a+=$5;c++}else{b+=$5;d++}} END{printf "cap_cont=%.0f cap_clean=%.0f\n", a/c, b/d}'
taskset -c 8-15 /home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000   # guest-health control, expect ~10.9 s
```

If the second command is ~10.9 s and the all-16 IVH-off run is well above 15 s, the host is the problem, not the guest.
