# Is there a post-boot calibration transient? No. The fast failure is the known bistability, entered from the wrong side — and the host is slower tonight

**Date:** 2026-08-09 (overnight session, ~23:55–02:05, following `ivh_script_reproduction_audit_2026-08-08.md`)
**Kernel under test:** `6.17.0-rseqport71-byeunhalt+` (live, booted 2026-08-08 23:47), branch `kernel-43-clean`, base commit `6c3874293`
**Host contention:** skewed 8-vCPU corunner, verified independently at session start and session end (sec 1.2)
**Status:** **75 real hackbench runs, all live**, per-run PV-steal corunner verification. No replay, no simulation. One arm was invalidated by an error of mine and is discarded, with the cause recorded (sec 4.3). Everything labelled "live-tested" was executed on this machine tonight.

---

## 0. Headline, up front

**The calibration / warm-up hypothesis is RULED OUT.** There is no post-boot warm-up requirement in either the `ivh_tks_*` tick-gap steal estimator or the `ivh_uc_capacity` EMA. Both are explicitly built to have none, and I verified that in the source and against live behaviour:

- `ivh_tick_steal_accumulate()` carries at most **`ivh_tks_carry_ticks` = 8 ticks = 8 ms** of state (`core.c:2305-2308`), plus a one-tick `prev_tsc`/`prev_idle_ns` pair that is seeded on the first tick it ever runs (`core.c:2255-2258`). There is nothing in it that could still be miscalibrated seconds — let alone minutes — after boot.
- `rq->ivh_uc_capacity` is **seeded at `SCHED_CAPACITY_SCALE` (1024) at rq init** (`core.c:11722`), and the first window **assigns rather than blends** (`core.c:2500-2506`), with an in-tree comment saying that is there precisely to remove "the multi-half-life cold-start transient a zero-seeded blend would otherwise cost".

So the specific mechanism proposed — boot-time tick noise seeding `ivh_tks_carry_c` with garbage that then takes EMA half-lives to wash out — cannot happen. The carry is bounded at 8 ms and is drained to zero on every positive excursion.

**What is actually going on is three things at once, and only the second is new:**

| # | finding | status |
|---|---|---|
| 1 | **The host is genuinely slower tonight.** IVH-off baseline **15.87 s** (n=3, sd 0.19) against 14.98 s the previous morning and 13.88 s the night before. The ~11.5 s figure was validated against a 14.98 s baseline. | live-measured, sec 2 |
| 2 | **The zero-gap protocol is worth ~2.3 s on its own.** Same machine, same state region: n=8 at 0 s gap → **14.70 s**; n=8 at 5 s gap → **12.36 s**. The orchestrator's batch used no gap; every validated block used 5 s. | live-measured, sec 4.1 |
| 3 | **The rest is `ivh_wall_path_validation` sec 6's bistability, and tonight the machine sits on the wrong side of it.** The destination population would not converge to 1023 at idle at any point in a 2-hour session — it plateaus at 1000–1015 — whereas all three prior sessions had it pinned at 1022–1023. | live-measured, sec 3, sec 5 |

**The failure reproduces exactly.** n=8, zero gap, immediately after a fresh `/home/nick/IVH`: **12.18, 14.70, 16.05, 20.81, 15.92, 13.53, 12.24, 12.16 — mean 14.70 s.** Compare the orchestrator's **14.39, 13.79, 15.84, 18.29, 15.32, 12.98, 11.50, 13.43 — mean 14.4 s.** Same mean, same shape: degrade to a worst run at #4, recover by #7. This is a limit cycle with a period of roughly six runs, not a monotone decay and not a broken configuration.

**Also ruled out tonight, each with a measurement rather than an argument:** the corunner (sec 1.2), the launch script's own load (sec 4.2), agent/background guest activity (sec 3.2), and raising `ivh_uc_min_steal_ns` (sec 5.2).

**One thing came very close to being a fix and then failed to reproduce, and I am recording the failure rather than the success:** `IVH_CAP_MARGIN_REL=1` produced the best block of the session — **11.41 s (n=7, sd 0.11)** with the destination population climbing to 1023 and *pinning* — and then a second block with the identical configuration and a near-identical starting state gave **14.05 s**. Sec 6.

---

## 1. Method

### 1.1 Harness

Reference command, unchanged from the rest of the project:

```
/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000
```

Every run records, automatically and simultaneously: wall time from `ivh_exec`'s own `Time:` line; **per-vCPU PV steal delta from `/proc/stat` across exactly that run** (the corunner check — `paravirt_steal_clock`, wholly independent of everything under test, so a broken source-2 estimator cannot fool it); `ivh_uc_capacity` per vCPU sampled at 100 ms through the run and split contended (0-7) / clean (8-15), plus the pre-run and post-run population means, maxima and minima; and the `ivh_migrations_done` delta.

Harness `runbench.sh` and raw rows in the session scratchpad. Protocols: **gap 0** (the orchestrator's), **gap 5 s** (the validated "back-to-back"), **gap 20 s**.

### 1.2 Corunner verification — measured, not taken on trust

I do not have host-side visibility, so I did not rely on the user's `top` check. All 16 vCPUs driven to a busy loop for 6 s, `/proc/stat` steal delta:

| when | cpu0-7 | cpu8-15 |
|---|---|---|
| session start (00:04) | **66.1 – 67.2 %** | 0.2 % |
| session end (02:02) | **65.7 – 66.8 %** | 0.2 % |

Prior sessions measured 66.5–67.7 % / 0.2–0.4 %. **Unchanged, and within noise of all three.** The skewed 8-vCPU corunner is genuinely in place and genuinely on the first 8 pCPUs. Additionally every one of the 75 run rows carries its own contended/clean steal split; a run with the corunner off shows `cont ~0 %` and is instantly visible. None do.

**So the corunner is conclusively not the explanation, and this is established independently of the host-side check.**

### 1.3 Starting state confirmed before anything was touched

All 16 documented sysctls matched `ivh_script_reproduction_audit_2026-08-08.md` sec 2.1 exactly (`ivh_universal_eligible=1`, `ivh_steal_source=2`, `ivh_cap_source=3`, `ivh_uc_used_source=0`, `ivh_capacity_threshold=1010`, `ivh_uc_min_steal_ns=500000`, `ivh_uc_ema_alpha_q16=868`, `ivh_uc_min_avail_pct=10`, `ivh_tks_deadband_ns=50000`, `ivh_tks_phase_pct=0`, `ivh_tks_carry_ticks=8`, `ivh_ka_enabled=0`, `ivh_ref_steal_enabled=0`, `ivh_uc_enabled=1`). BPF constants `IVH_CAP_HARDFLOOR 880`, `TOPBAND 50`, `MARGIN 20`, `MARGIN_REL 0`. Exactly one `MY_ivh_atc` (pid 4121) and one `vcap_probe -p 200 -s 5000` (pid 4128), both started 23:52:19; `vsched_module` loaded; `ivh_cfg` = 3.

**Nothing was misconfigured.** The `ivh_uc_min_steal_ns=500000` fix that the audit session added to `/home/nick/IVH` did survive the reboot and was applied correctly — that is the first thing I checked, and it is worth recording as a positive: the audit session's script fix works.

---

## 2. The host is slower tonight, and this reframes the target

Measured tonight, `ivh_universal_eligible=0`, everything else identical, 5 s gap:

| session | IVH-off baseline | n |
|---|---|---|
| overnight 2026-08-08 (`final_tsc_only`) | 13.88 s | 3 |
| morning 2026-08-08 (`wall_path`) | 14.98 s | 3 |
| **tonight** | **15.87 s** (sd 0.19, range 15.60–16.01) | 3 |

Contended PV steal during those baseline runs was 27.2–27.9 %, against 25.3–26.1 % in the morning session. The corunner is the same; the host as a whole is carrying more.

This matters for reading the anomaly. Against tonight's baseline:

- the orchestrator's 14.4 s is a **9 % improvement**, not "IVH doing nothing";
- tonight's best block, 11.41 s, is a **28 % improvement** — the largest relative margin measured anywhere in this project.

**The ~11.5 s number is not a fixed property of the mechanism; it is a property of the mechanism *on the host as it was on 2026-08-08 morning*.** A pass/fail threshold written as an absolute wall time — which is what `ivh_verify.sh` sec 6 currently does (`mean < 12.5 and max < 13.5`) — will produce false failures whenever the host load moves. Sec 8 recommends making it relative to a same-session IVH-off baseline.

The target is still *reachable* tonight (11.30 s appears repeatedly in sec 6), so this is not the whole story. It is roughly the first third of it.

---

## 3. The calibration hypothesis, tested directly

### 3.1 What the code says, and it is unambiguous

Both candidate sites are explicitly designed to have no warm-up:

```c
/* core.c:2255 -- tick-gap estimator, first tick ever */
if (unlikely(!rq->ivh_tks_prev_tsc)) {
        rq->ivh_tks_skipped++;
        goto seed;                      /* first tick: no delta yet */
}
```

```c
/* core.c:2500 -- capacity EMA, first window ever */
if (unlikely(!rq->ivh_uc_windows)) {
        /* First window: ASSIGN rather than blend, ... removing the
         * multi-half-life cold-start transient a zero-seeded blend
         * would otherwise cost (sec 3.3). */
        rq->ivh_uc_ema_wall_q = x_wall << 16;
```

and `rq->ivh_uc_capacity` is seeded to 1024 at `sched_init()` (`core.c:11722`). The proposed contamination path — boot noise poisoning `ivh_tks_carry_c` — is additionally blocked by the carry's own structure: it is drained to **zero** on every tick where it is positive (`core.c:2291-2293`) and floored at −8 ticks when negative. **Its memory is at most 8 ms.**

I also checked the one runtime discontinuity that could have looked like a calibration transient: `/home/nick/IVH` flips `ivh_steal_source` from the kernel default 0 (PV steal page) to 2 (tick-gap) five minutes into the boot, which makes `ivh_uc_steal_ns()` return a completely different cumulative series. `ivh_uc_tick()` clamps the delta non-negative (`core.c:2629-2630`), so the source switch costs **exactly one tick** of suppressed steal on each vCPU and nothing more.

**Verdict: ruled out, on structural grounds, at both layers.**

### 3.2 What actually needs time — and it is not the estimator

Measured live, this boot, from the state the orchestrator left behind. At **00:00, twelve minutes after boot and seven after the `/home/nick/IVH` launch**, with the guest otherwise idle:

```
cap_cont = 870   cap_clean = 954     (clean should read ~1023)
```

That is the depressed-destination signature of `ivh_script_reproduction_audit_2026-08-08.md` sec 5, confirmed independently. Watching it recover at 15 s resolution with the guest idle:

| clock | cap_clean | cap_cont | raw_wall on clean |
|---|---|---|---|
| 23:59:50 | 974.8 | 864 | 1024 |
| 00:00:35 | 986.9 | 861 | 1024 |
| 00:02:20 | 1000.6 | 759 | 1024 |
| 00:12:05 | 1014.0 | 851 | 1024 |
| 00:34:50 | **1001** | 853 | 1024 |
| 01:17:27 | 995 | 850 | 1024 |

**It never reached 1023.** Over more than two hours it oscillated in a band of roughly 1000–1015, climbing when quiet and being knocked back a few points by any guest activity at all. Meanwhile the contended population drifts *downward* toward its raw value of ~700, so the separation widens as the destination population fails to pin.

**This is the substantive difference between tonight and every prior session,** which all recorded the clean population at 1022–1023, flat, through entire 8- and 24-run blocks.

Two candidate explanations for the plateau were tested and both failed:

- **Agent/background guest load.** I confined the agent process tree to cpu0-7 with `taskset` and let the machine sit for ten minutes with a single 120 s poll as the only activity. The clean population went to **1003, not up.** Ruled out. (This experiment did have a side effect that cost me an arm — sec 4.3.)
- **A stale poller of my own.** Found and killed a leftover 20 s polling loop from earlier in the session. No effect on the plateau.

The publish arithmetic explains the *rate* but not the *plateau*: at `vcap_probe -p 200 -s 5000` an idle vCPU publishes ~2 windows per 5.2 s, and at `ivh_uc_ema_alpha_q16=868` the half-life is 52 samples ≈ 135 s, so a single bad sample of ~700 costs ~4 points and takes minutes to walk off. An equilibrium at 1003 implies roughly one bad published window per 50 s on the clean vCPUs while the guest is idle. That rate is higher tonight than it was in prior sessions, and I could not find a guest-side cause for it.

---

## 4. Reproducing the fast failure

### 4.1 It reproduces, and the zero gap is a large part of it

Arm A: fresh `/home/nick/IVH` launch, then straight into n=8 with **no gap**, exactly the orchestrator's protocol. Starting clean population 1012.

| run | wall | pre cap cont / clean | migrations | contended PV steal |
|---|---|---|---|---|
| 1 | 12.18 s | 864 / 1012 | 54 474 | 10.9 % |
| 2 | 14.70 s | 914 / 968 | 40 529 | 16.1 % |
| 3 | 16.05 s | 896 / 911 | 21 220 | 22.6 % |
| 4 | **20.81 s** | 769 / 821 | 6 353 | 32.5 % |
| 5 | 15.92 s | 613 / 765 | **0** | 31.7 % |
| 6 | 13.53 s | 644 / 865 | 54 118 | 13.0 % |
| 7 | 12.24 s | 816 / 918 | 68 766 | 9.9 % |
| 8 | 12.16 s | 913 / 960 | 79 594 | 9.0 % |

**n=8, mean 14.70 s, sd 2.76.** Against the orchestrator's 14.39/13.79/15.84/18.29/15.32/12.98/11.50/13.43, mean 14.4 s. Same mean, same period, same shape.

The mechanism is fully legible in the table and it is a **closed limit cycle**, not a decay:

1. IVH migrates work onto the clean vCPUs;
2. loaded clean vCPUs develop phantom tick-gap steal, so their published capacity falls;
3. by run 4 the whole population has sunk below `IVH_CAP_HARDFLOOR = 880` (613 / 765) and the destination scan rejects everything — **migrations reach literally zero at run 5**;
4. with IVH inert the clean vCPUs unload and recover (765 → 865 → 918 → 960);
5. the gates reopen, migrations return to 68–80 k, times return to 12.2 s;
6. and the cycle restarts.

Same machine, same state region, **only the gap changed from 0 s to 5 s** (arm B, starting clean 1013): 12.29, 12.16, 12.61, 13.53, 11.52, 11.64, 11.84, 13.32 — **mean 12.36 s, sd 0.70**, with three consecutive runs at 11.52 / 11.64 / 11.84.

**The gap is worth 2.34 s of the 3.2 s gap between the orchestrator's 14.4 s and the 11.5 s target.** It is not the whole story, but it is the single largest controllable term, and it is a protocol deviation rather than anything about the machine.

### 4.2 The launch script's own load is not the cause — ruled out by measurement

`/home/nick/IVH` does an `rmmod`/`insmod` of `vsched_module`, a full `make MY_ivh_atc` (a real compile), and restarts both daemons, then prints "IVH up" after `sleep 3`. That is real guest CPU, and if it landed on the clean vCPUs it would depress them immediately before the operator benchmarks. Measured across a live relaunch:

| | cap cont | cap clean | cleanMIN |
|---|---|---|---|
| immediately before `/home/nick/IVH` | 860 | 1009 | 1006 |
| +1 s after both daemons up | 860 | 1009 | 1007 |
| +30 s | 863 | **1011** | 1009 |

**No depression at all** — it drifted slightly *up*. Ruled out. Worth recording because it was a plausible-sounding hypothesis with a cheap test.

### 4.3 An error of mine, and the arm it cost

For the sec 3.2 experiment I confined the agent process tree to cpu0-7 with `taskset`. Shells spawned afterwards inherit that affinity, so the next batch ran **`ivh_exec` and hackbench pinned to the eight contended vCPUs only**. The result — 8 runs at 22.5–24.3 s, `migr` 0–8, `steal_cl = 0.0 %` — is not a measurement of anything I intended and is **discarded**. It is listed in sec 7 as `C_gap20` with the reason.

It is worth one line of signal: **hackbench confined to the eight 67 %-stolen vCPUs takes 23.4 s**, versus 15.87 s across all 16 with IVH off. The affinity was restored, verified by read-back on a fresh shell (`0-15`), and the arm was re-run as `C2_gap20`. The two blocks measured before the mistake (arms A and B) and all blocks after it are unaffected; the timeline was checked explicitly.

---

## 5. Why the destination population cannot hold, measured

### 5.1 The phantom on clean vCPUs is the same size as the signal on contended ones

Sampled `/proc/ivh_debug` at 50 ms through a full hackbench run and captured `win_stolen_c` at each window close, per vCPU (~73 closed windows each, 200 ms windows, `tsc_khz=2200000`):

| | median stolen / window | p90 | max | windows over the 500 µs guard |
|---|---|---|---|---|
| **contended cpu0-7** (67 % real steal) | 3.2 – 13.0 ms | 12.3 – 21.0 ms | 62 – 97 ms | 45–68 of ~72 |
| **clean cpu8-15** (0.2 % real steal) | **1.4 – 11.4 ms** | 8.5 – 21.8 ms | 14.8 – 27.4 ms | 45–68 of ~74 |

**The two distributions overlap almost completely.** cpu12 and cpu14 — vCPUs with 0.2 % genuine PV steal — book a median of 11.0–11.4 ms of *estimated* steal per window, more than cpu7 and cpu4 (3.2 ms and 7.9 ms) which are genuinely 67 % stolen.

This is a fresh, independent confirmation of `ivh_wall_path_validation_2026-08-08.md` sec 4's central result (steal term common-mode at 1.14–1.18× under load), measured by a third route — per-window accumulators at close time rather than published ratios or bpftrace tick reconstruction. All three routes agree.

### 5.2 Which is why raising `ivh_uc_min_steal_ns` cannot be the fix

The guard publishes a clean 1024 when a window's estimated steal falls under the bar. At the shipped 500 µs it fires on only **8–40 % of clean windows during a run** — under load it is largely inoperative, which is exactly why the destination population unpins.

Raising the bar cannot rescue it, and the table above is the reason: to suppress the clean population's median phantom you would need a bar around 12 ms, which also suppresses most of the contended population's genuine signal (medians 3.2–13.0 ms). There is no threshold that separates them. **This is a measured negative result, not a guess, and it is the same shape as `wall_path` sec 5.1's deadband analysis: the two distributions occupy the same buckets.**

(This does **not** contradict the audit session's finding that `min_steal_ns=10000` is much worse than 500000. At idle, and in the good basin where the clean vCPUs stay lightly loaded, the guard does fire and does pin the population. The audit result stands. What is new is that the guard is not load-invariant, and tonight the machine spends its time in the regime where it does not fire.)

---

## 6. `IVH_CAP_MARGIN_REL` — the best block of the session, and why I am not calling it a fix

`ivh_wall_path_validation_2026-08-08.md` sec 7.1 implemented a population-relative margin behind `IVH_CAP_MARGIN_REL`, live-tested it at 11.59 s, judged it a partial fix, and left it compiled out. Tonight's regime — a destination population that will not pin, and a separation that swings from 20 to 200 — is precisely what it was designed for, so I rebuilt with `IVH_CAP_MARGIN_REL 1` (NUM/DEN = 1/3, `MARGIN_MIN 20`) and reloaded. No reboot, BPF only.

**Arm E, 5 s gap, starting clean population 996:**

| run | wall | pre cont / clean | migrations | contended PV steal |
|---|---|---|---|---|
| 1 | 12.46 s | 852 / 996 | 91 575 | 8.1 % |
| 2 | **11.30 s** | 921 / 1012 | 67 789 | 6.9 % |
| 3 | **11.31 s** | 945 / 1018 | 66 434 | 7.8 % |
| 4 | 11.37 s | 952 / 1021 | 68 854 | 6.9 % |
| 5 | 11.33 s | 964 / **1022** | 68 608 | 6.8 % |
| 6 | 11.45 s | 970 / **1023** | 69 560 | 7.0 % |
| 7 | 11.62 s | 976 / **1023** | 71 127 | 6.8 % |
| 8 | 11.50 s | 980 / **1023** | 69 969 | 7.0 % |

**n=8 mean 11.54 s; runs 2-8 mean 11.41 s, sd 0.11** — against tonight's 15.87 s baseline, a 28 % improvement, and the tightest block in the project's history. And it did not merely run fast: **the destination population climbed 996 → 1023 and pinned there**, migrations held the canonical 66–71 k band, and contended PV steal fell to 6.8–7.0 %. It entered the good basin and stayed.

**Arm F, then, with the orchestrator's zero gap, starting from that converged state:** 11.54, 11.65, 11.68, 11.76, 12.04, 11.74, 12.56, 14.92 — **runs 1-6 mean 11.73 s (sd 0.15)**, then degradation at runs 7-8 as the contended population climbed into the gates (contMAX 983 → 1001) and the clean population fell to 987. So the relative margin buys about six good zero-gap runs where the absolute margin buys one.

**Arm G — and this is why it is not a fix.** Same configuration, same 5 s gap, starting clean population 1011 (if anything a *better* entry state than arm E's 996): 11.84, 13.46, 14.74, 15.68, 13.80, 13.25, 14.24, 15.42 — **mean 14.05 s**, with migration storms of 91 k, 97 k and 83 k driving the population down to an inverted 875/837.

**Two blocks, one configuration, near-identical starting states, 11.41 s and 14.05 s.** The relative margin did not determine the outcome; which basin the first run happened to fall into did. I ran arm E before arm G, and had I stopped after arm E I would have reported a fix that does not exist. Recording the failure is the finding.

For completeness, the control is already in the table: arm D, absolute `MARGIN=20`, 5 s gap, starting from an *excellent* clean population of 1015, collapsed monotonically 12.70 → 15.52 s (mean 14.43 s) via a migration storm at runs 3-4 (77 k) — `wall_path` sec 6's "`MARGIN=20` is wrong in the wide regime", reproduced. So the absolute form fails from a good entry state too. Neither gate form is reliable tonight.

**Left compiled out (`IVH_CAP_MARGIN_REL 0`) and the running program restored byte-identical to the session-start build.**

### 6.1 The probe-cadence idea, tested and rejected

Since the good basin is self-reinforcing (a well-behaved block publishes clean windows at the full 200 ms cadence at raw 1024 and races the EMA to 1023 in seconds, whereas an idle vCPU publishes ~2 windows per 5.2 s and creeps), I tried making basin entry reliable by shortening the probe period: `vcap_probe -p 200 -s 1000` instead of `-s 5000`.

Convergence at idle did improve dramatically — 902 → 1015 in **150 s**, versus the 13+ minutes it had been taking, reaching 1021 at 360 s. But the resulting batch was the **worst of the session: mean 15.24 s (n=8)**, with the contended population driven down to 730–810 (into `IVH_CAP_HARDFLOOR` territory) and contended PV steal stuck at 20–29 %. The 20 % duty cycle across 16 threads is ~3.2 vCPUs of guest CPU, and it changes the very quantity `ivh_uc_tick()` measures.

**Reverted to `-p 200 -s 5000`. Faster convergence is not worth it at this price**, though the underlying observation — that the good basin is self-reinforcing and idle convergence is the slow path into it — is sound and is the most promising direction I found (sec 8).

---

## 7. Every block run tonight, live

All corunner-verified per run. Baseline for all comparisons: **15.87 s** (IVH off, same session).

| arm | gate config | gap | start clean cap | n | mean | sd | note |
|---|---|---|---|---|---|---|---|
| **BASE**, IVH off | MARGIN 20 | 5 s | 968 | 3 | **15.87 s** | 0.19 | tonight's baseline |
| **A** | MARGIN 20 | **0 s** | 1012 | 8 | **14.70 s** | 2.76 | **reproduces the reported failure** |
| **B** | MARGIN 20 | 5 s | 1013 | 8 | **12.36 s** | 0.70 | the gap alone is worth 2.34 s |
| ~~C~~ | — | 20 s | — | 8 | ~~23.42 s~~ | — | **DISCARDED** — hackbench pinned to cpu0-7 by my own `taskset` error (sec 4.3) |
| **C2** | MARGIN 20 | 20 s | 992 | 8 | 12.73 s | 1.09 | climbs into the good basin by run 6 |
| **D** | MARGIN 20 | 5 s | **1015** | 8 | 14.43 s | 0.91 | collapses from a *good* entry state, via a storm |
| **E** | **MARGIN_REL 1/3** | 5 s | 996 | 8 | **11.54 s** | 0.36 | best block of the project; runs 2-8 **11.41 s, sd 0.11** |
| **F** | MARGIN_REL 1/3 | **0 s** | 1023 | 8 | 12.24 s | 1.06 | runs 1-6 **11.73 s**, then degrades |
| **G** | MARGIN_REL 1/3 | 5 s | 1011 | 8 | **14.05 s** | 1.17 | **E does not reproduce** |
| **H** | MARGIN_REL 1/3, probe `-s 1000` | 5 s | 1021 | 8 | 15.24 s | 1.53 | faster convergence, worst throughput |

Best single run of the session: **11.303 s**. Worst: 20.812 s.

### 7.1 An accident at the end of the session that is the most useful practical result in it

The three IVH-off baseline runs of sec 2 were the last thing executed. Their per-run capacity rows:

| baseline run | post clean cap | post cleanMIN |
|---|---|---|
| 1 | 1003 | 991 |
| 2 | 1016 | 1012 |
| 3 | **1020** | 1018 |

and the machine settled at **clean = 1023, cleanMIN = 1022** — the exact state every prior session ran in, and a state that **more than two hours of idling had never once reached tonight** (sec 3.2).

The mechanism is the sec 6.1 observation running in the benign direction. An idle vCPU publishes ~2 windows per 5.2 s, so the EMA creeps at a ~135 s half-life and any stray dip undoes minutes of progress. Under hackbench **with IVH off**, all 16 vCPUs publish at the full 200 ms cadence, and because no migrations are being made the clean vCPUs never take on the concentrated load that generates their phantom — so they publish raw 1024 at ~5 windows per second and the EMA converges in tens of seconds.

**So the fast, reliable way to converge the destination population is not to idle — it is to run two or three hackbench rounds with `ivh_universal_eligible=0` first.** That is a warm-up that costs ~50 s, needs no reboot, no kernel change and no new code, and it lands the machine in exactly the state the validated blocks started from.

**Label: observed live, n=3, once. Not yet tested as a deliberate protocol** — I found it in the last block of the session and did not have a batch left to confirm that a measured block run immediately afterwards reproduces 11.5 s. That confirmation is the single cheapest next experiment available (three IVH-off runs, flip the sysctl, eight measured runs) and it should be the first thing tried next session.

---

## 8. What I would do next, in priority order

0. **Confirm the IVH-off warm-up of sec 7.1.** Three hackbench rounds with `ivh_universal_eligible=0`, then flip it to 1 and run a measured n=8 block at a 5 s gap. If that reproduces ~11.5 s it is the missing precondition, it costs 50 s, and it belongs in both `/home/nick/IVH`'s closing advice and `ivh_verify.sh` — replacing the idle-settle advice, which tonight would have waited forever. Cheapest experiment available and the only one that could turn tonight's open question into a procedure.
1. **Stop expressing the target as an absolute wall time.** `ivh_verify.sh` sec 6 fails the batch unless `mean < 12.5` and `max < 13.5`. Tonight's baseline is 15.87 s and the *same* configuration that scores 11.41 s in one block scores 14.05 s in the next. Both a slower host and a basin flip produce a "FAIL" that says nothing about the mechanism. **Make the check relative:** run 3 IVH-off runs in the same batch and assert an improvement ratio (tonight's good block is 28 %, the failing ones 9–19 %, IVH-off is 0 % by construction). This is a small edit to `ivh_verify.sh` and it would have made the reported anomaly self-diagnosing. **Not written** — it changes the pass criterion, which is the user's call, and it costs three extra runs per verification.
2. **Add a settle gate, but a time-boxed one.** `/home/nick/IVH` already prints the settle warning; `ivh_verify.sh` does not enforce it. A loop that waits for the clean population to reach ~1020 with a hard timeout of a few minutes would have caught tonight's state — but note it would have *timed out* tonight, because 1020 was never reached at idle. That timeout firing is itself the diagnostic the operator needed. **Not written** for the same reason as #1.
3. **The volume regulator** (`wall_path` sec 7.2 item 2) is now much better motivated than it was. Across tonight's 72 valid runs the migration count is a cleaner predictor than any capacity difference: every good run sits in 66–72 k; every collapse is preceded by a storm (77–97 k) or a starvation (0–24 k). Both gate forms — absolute and relative — fail by losing volume control in one direction or the other, from the same entry states. This is BPF-side and needs no reboot.
4. **Do not spend a reboot on the estimator.** Sec 3 rules out a warm-up transient structurally, and sec 5.1 is a third independent confirmation that the tick-gap steal term is common-mode under load. The remaining work is in the gate layer, as `wall_path` sec 11 concluded.

**What I would not do:** raise `ivh_uc_min_steal_ns` (sec 5.2, measured), shorten the probe period (sec 6.1, measured), or ship `IVH_CAP_MARGIN_REL` on the strength of arm E (sec 6).

---

## 9. Exact state left on the machine

**Restored to the session-start state and verified by read-back.**

- **Sysctls:** all 16 read back identical to sec 1.3. `ivh_universal_eligible` was set to 0 for the three baseline runs and **restored to 1**, verified.
- **BPF:** `MY_ivh_atc.bpf.c` is **byte-identical to the session-start file** (`diff` clean against a backup taken before any edit). `IVH_CAP_MARGIN_REL` was flipped to 1 and back to 0; rebuilt and reloaded at 0. `ivh_cfg` = 3. Exactly one `MY_ivh_atc` running.
- **`vcap_probe`:** back at `-p 200 -s 5000`. Exactly one instance.
- **Agent affinity:** the `taskset 0-7` confinement of sec 3.2 was reverted to `0-15` and verified on a fresh shell.
- **Kernel source:** **not touched.** `core.c`, `cputime.c`, `fair.c`, `sched.h` carry only the pre-existing uncommitted work from earlier sessions. No kernel change was written and none is required by anything here.
- **`/home/nick/IVH` and `/home/nick/ivh_verify.sh`: not edited.** Sec 8's two candidate edits are described but deliberately not applied — both change a pass/fail criterion, and one of them (#2) would have failed tonight's machine by design. That is the user's decision, not mine to make silently.
- **No reboot performed.** Nothing here needs one.
- **Nothing committed or pushed.**

Only new file: this document.

---

## 10. Honest confidence

| claim | verdict | confidence |
|---|---|---|
| There is a post-boot warm-up/calibration transient in the steal estimator or the capacity EMA | **False** | **High** — both sites are explicitly built to have none; the tks carry's memory is bounded at 8 ms and the EMA's first window assigns rather than blends |
| The corunner was correctly configured | **True** | **High** — measured twice by PV steal, independent of everything under test, matching all three prior sessions |
| The reported failure reproduces | **True** — mean 14.70 s vs 14.4 s, same run-by-run shape | **High** — n=8, per-run corunner |
| The zero gap accounts for a large share of it | **True** — 14.70 s vs 12.36 s, same machine, same state region | **High** — two n=8 blocks, one variable |
| The host is slower tonight than when 11.5 s was validated | **True** — 15.87 s vs 14.98 s IVH-off baseline | **Moderate-high** — n=3 each, but non-overlapping ranges and a matching rise in baseline steal |
| The residual is `wall_path` sec 6's bistability | **True** | **Moderate-high** — five blocks collapse or oscillate; the mechanism is legible run-by-run in sec 4.1 |
| The destination population would not converge to 1023 at idle tonight | **True** | **High** — watched for over two hours at 15–120 s resolution; never above 1015 |
| I know *why* it would not converge tonight when it did on 2026-08-08 | **No** | **Low** — agent load, the launch script, a stale poller and probe cadence were each tested and none explains it. This is the open question. |
| `IVH_CAP_MARGIN_REL=1` fixes it | **Not demonstrated** — 11.41 s in one block, 14.05 s in the next | **High** that it is not demonstrated; **moderate** that it is nonetheless the better gate form (it produced the two best blocks and never produced the worst) |
| Raising `ivh_uc_min_steal_ns` would fix it | **False** | **Moderate-high** — derived from a measured per-window distribution showing full overlap, not tested at every candidate value |
| A faster probe cadence would fix it | **False** — converges 5× faster, measures 15.24 s | **Moderate** — one n=8 block |

**Overall.** The user's hypothesis was a good one and it is wrong, for a reason that is worth knowing: the two obvious places for a warm-up transient are both explicitly engineered not to have one, and I could confirm that in the source and against live behaviour. What the fresh boot actually did was put the machine into the bad half of a known bistability and then measure it there, with a protocol (no gap) that the validated result never used, on a host that is measurably slower than the one the target was set on. Two of those three are quantified. The third — why the destination population will not pin at 1023 tonight when it pinned on three consecutive prior sessions — I did not solve, and I am not going to dress up four ruled-out hypotheses as an answer.
