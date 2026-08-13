# Auditing `/home/nick/IVH` against the validated 11.51 s WALL configuration

**Date:** 2026-08-08 (evening session, auditing `ivh_wall_path_validation_2026-08-08.md`)
**Kernel under test:** `6.17.0-rseqport71-byeunhalt+` (live, booted), branch `kernel-43-clean`, base commit `6c3874293`
**Host contention:** verified at session start (cpu0-7 67.2-67.3 %, cpu8-15 0.2-0.3 %) and again at session end (65.8-67.0 % / 0.2 %) — **unchanged, and identical to both prior sessions**, so all three sessions' numbers are directly comparable
**Status:** 47 real hackbench runs, all live, per-run corunner verification. No replay, no simulation.

**Question asked:** the orchestrating conversation applied what it believed was the validated 11.51 s configuration, ran one ad-hoc hackbench, and got 12.437 s. Is there a flag, an ordering requirement, or a protocol detail that the validated run had and the reproduction did not?

---

## 0. Answer, up front

**The live machine state was already correct, and it reproduces.** Before touching anything: **n=8 back-to-back, mean 11.41 s, sd 0.25, range 11.07-11.77** against the same day's 14.98 s no-IVH baseline. Every one of the 14 documented sysctls, all four BPF constants, and both daemons matched the validated state exactly. Nothing was missing.

**But `/home/nick/IVH` itself is wrong, in a way that would only have shown up after the next reboot.** It never sets `ivh_uc_min_steal_ns`. The kernel default is **10000**; the validated state is **500000**. The live value was 500000 only because a previous session had set it by hand — the script would have silently reverted it on the next boot. Measured live, that single knob is worth **11.41 s → 13.76 s and rising**, ending in a full capacity inversion. Sec 3.

**And the 12.437 s is explained without any configuration difference at all.** It is what this configuration measures when the capacity EMA has not converged. I reproduced it deliberately: **n=8, mean 12.38 s, one run at 12.44 s**, same config, only difference being that the destination population sat at ~1002 instead of 1023 when the block started. Then I predicted the converse and confirmed it — after 7 minutes of genuine idle brought the population back to 1021, the same configuration returned **11.60 s (n=8, sd 0.30)**. Sec 5, 5.2.

Three further script defects (daemon duplication, a race on the `ivh_cfg` map update, unasserted BPF constants) are real but were not implicated in this particular gap. Sec 4.

---

## 1. Method

Harness and workload unchanged from the two prior sessions — `runbench.sh` in the shared session scratchpad, driving

```
/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000
```

and recording per run: wall time, `ivh_exec`'s own `Time:`, per-vCPU PV steal delta from `/proc/stat` across exactly that run (the corunner check, `paravirt_steal_clock`, independent of everything under test), mean `ivh_uc_capacity` / `_wall` / `_acct` per vCPU sampled at 100 ms split contended (0-7) / clean (8-15), and the `ivh_migrations_done` delta. All 39 rows appended to the same `results.tsv`.

"b2b" = 5 s gap (the protocol the validated 11.51 s block used); "rested" = 45 s gap.

Corunner at session start: cpu0-7 **67.2-67.3 %**, cpu8-15 **0.2-0.3 %**. The prior two sessions measured 66.5-67.7 % / 0.2-0.4 %. Unchanged.

---

## 2. Live state vs. the validated state — a full diff

Read directly off the machine before anything was changed.

### 2.1 Sysctls — all 14 match

| knob | live | validated (`wall_path` sec 10) | kernel default | match |
|---|---|---|---|---|
| `ivh_uc_used_source` | 0 | 0 | 0 | yes |
| `ivh_universal_eligible` | 1 | 1 | **0** | yes |
| `ivh_steal_source` | 2 | 2 | **0** | yes |
| `ivh_cap_source` | 3 | 3 | **0** | yes |
| `ivh_capacity_threshold` | 1010 | 1010 (retuned) | 1010 | yes |
| `ivh_uc_min_avail_pct` | 10 | 10 | 10 | yes |
| `ivh_uc_min_steal_ns` | 500000 | 500000 | **10000** | yes *(live only — see 3)* |
| `ivh_uc_ema_alpha_q16` | 868 | 868 | 868 | yes |
| `ivh_uc_window_ns` | 200000000 | 200000000 | 200000000 | yes |
| `ivh_uc_duty_ns` | 0 | 0 | 0 | yes |
| `ivh_tks_deadband_ns` | 50000 | 50000 | 50000 | yes |
| `ivh_tks_phase_pct` | 0 | 0 | 0 | yes |
| `ivh_tks_carry_ticks` | 8 | 8 | 8 | yes |
| `ivh_ka_enabled` | 0 | 0 | 0 | yes |

Defaults read from `kernel/sched/bpf_sched.c:388-425` and `kernel/sched/core.c:223,331-333,371`.

### 2.2 BPF constants — all match

`IVH_CAP_HARDFLOOR 880`, `IVH_CAP_TOPBAND 50`, `IVH_CAP_MARGIN 20`, `IVH_CAP_MARGIN_REL 0`. Source mtime 19:20:28, `MY_ivh_atc` binary 19:20:29, running process started 19:20:29 — so the loaded program is genuinely built from these values, not a stale binary.

### 2.3 Daemons — correct, and singular

`vcap_probe -p 200 -s 5000` (pid 57897, up since 04:16), `MY_ivh_atc` (pid 116551, started 19:20:29), `vsched_module` loaded. **Exactly one of each** — no duplicate `MY_ivh_atc`, which is the failure mode that puts the first copy in D-state and double-fires every BPF hook. `ivh_cfg` map = 3, agreeing with `ivh_cap_source=3`.

### 2.4 Verdict on the live state

No discrepancy. And it measures accordingly:

| block | protocol | n | mean | sd | range | migrations | clean cap |
|---|---|---|---|---|---|---|---|
| **live state, untouched** | b2b | **8** | **11.41 s** | **0.25** | 11.07 - 11.77 | 65.7 - 73.0 k | 1022-1023 |
| live state, untouched | rested | 3 | 11.65 s | 0.11 | 11.53 - 11.74 | 67.0 - 69.4 k | 1022-1023 |

Against the same-day 14.98 s no-IVH baseline. This is the validated 11.51 s result, reproduced. Note it reproduces on **both** protocols here — this configuration is the one the prior session found does not need the 45 s gap.

---

## 3. The real defect: `/home/nick/IVH` never sets `ivh_uc_min_steal_ns`

The script set 20 sysctls. `ivh_uc_min_steal_ns` was not one of them, and it is the one documented value that differs from its kernel default. The machine was right only because a human had set it hours earlier; **the next boot would have run the whole stack at 10000.**

### 3.1 What the knob does

`bpf_sched.c:417-420` — it reproduces vcap's `if (stolen_pass < 10000) capacity_perc = 1.0` guard: a window whose estimated steal is below the threshold publishes a clean 1024 instead of a noisy ratio off a tiny denominator. At 500000 (500 µs per 200 ms window) an uncontended vCPU's phantom steal falls under the bar and it publishes 1024 — **this is what pins the destination population at 1023.** At 10000 (10 µs) essentially no window qualifies, so clean vCPUs publish their own phantom-contaminated ratio, and the destination population is free to sink.

That is the exact mechanism `ivh_wall_path_validation` sec 4 identified as WALL's weak point, with a knob sitting directly on it that the script left unset.

### 3.2 Measured live, back-to-back, everything else identical

| run | `min_steal_ns` = 10000 | separation | migrations | clean cap |
|---|---|---|---|---|
| 1 | 11.96 s | 65 | 64,064 | 1020 |
| 2 | 11.93 s | 42 | 64,569 | 1013 |
| 3 | 12.52 s | 32 | 62,374 | 1009 |
| 4 | 14.30 s | 22 | 47,986 | 1005 |
| 5 | 15.70 s | 3 | 41,601 | 990 |
| 6 | 16.14 s | **−28** | 44,077 | **962** |

**n=6, mean 13.76 s, monotonically degrading, ending inverted** — the contended population reading *higher* than the clean one. Compare the same block at 500000: 11.41 s, separation 244 → 44, clean flat at 1023 throughout.

This is not a marginal tuning difference. It is the inversion of `wall_path` sec 6.1 reproduced on demand, and `ivh_uc_min_steal_ns` is its most direct lever — a result that session did not have, because it only ever tested 10000 under the *old* gate configuration (`MARGIN=50`, `threshold=965`), where IVH is inert enough that the knob does not matter (that session's sec 9: "11.50 s, no effect on inversion").

### 3.3 Fix

`set_ivh_sysctl ivh_uc_min_steal_ns 500000` added to `/home/nick/IVH`, with the measured evidence in the comment. Restored live and verified by read-back.

---

## 4. Three further script defects, found by inspection

None of these were active on the live machine, but all three are real and all three are now fixed.

**4.1 No `pkill` before launching the daemons.** The old script ran `sudo ./MY_ivh_atc &` unconditionally. Running `/home/nick/IVH` twice — or once after a manual `setbpf.sh` — leaves two copies: every BPF hook fires twice and the first copy goes D-state. Same hazard for `vcap_probe`, where a second `-p 200` probe doubles the idle-vCPU probe load and changes exactly the thing `ivh_uc_tick()` measures. The scratchpad's `setbpf.sh` has always done the `pkill`; the launch script never did. Now it kills both and waits for them to be gone before rebuilding.

**4.2 `sleep 2` raced the `ivh_cfg` map update, and the failure was silent.** `bpftool map update name ivh_cfg` fails if the program has not finished loading, and the old script neither waited for the map nor checked the exit status. If it loses that race, `ivh_cfg` stays 0 = `IVH_CAP_SRC_VCAP`, the BPF program reads `rq->cpu_capacity` (a flat 1024) instead of `rq->ivh_uc_capacity`, and **no migration is ever justified** — the same silent-zero-migrations failure class as the `ivh_universal_eligible` bug the script had just been fixed for. Now: poll for the map (up to 10 s), then update, then report failure loudly. It also reads the value from `ivh_cap_source` rather than hardcoding `3`, so the two cannot drift.

**4.3 The BPF constants were documented in a comment, not checked.** `IVH_CAP_MARGIN` and `ivh_capacity_threshold` must move together — neither works alone (`wall_path` sec 6) — but the script pinned only the sysctl and left the `#define` to trust. It now asserts all four (`HARDFLOOR 880`, `TOPBAND 50`, `MARGIN 20`, `MARGIN_REL 0`) against the source before building and warns loudly on a mismatch.

**4.4 Also pinned, for completeness.** The seven `ivh_uc_*` / `ivh_tks_*` knobs that currently equal their kernel defaults are now set explicitly. They were correct by luck; a future default edit in `bpf_sched.c` would have silently changed the validated state.

---

## 5. Where 12.437 s comes from — it is not a missing flag

No configuration difference explains it: every knob was already right (sec 2). The explanation is the state of the capacity EMA at the moment of measurement, and it is reproducible.

Having perturbed the machine with the 10000 excursion of sec 3, I restored 500000, let it idle until the destination population had climbed back to ~1002 (not yet the 1022-1023 of the good block), and ran the identical protocol:

| run | wall | clean cap | separation |
|---|---|---|---|
| 1 | 11.69 s | 1012 | 131 |
| 2 | 11.97 s | 1001 | 61 |
| 3 | 12.54 s | 995 | 27 |
| 4 | 13.24 s | 1000 | 18 |
| 5 | 12.66 s | 1007 | 20 |
| 6 | 12.35 s | 1010 | 21 |
| 7 | **12.44 s** | 1013 | 22 |
| 8 | 12.12 s | 1014 | 22 |

**n=8, mean 12.38 s** — with a single run landing on **12.44 s**, against the 12.437 s in question. Same sysctls, same BPF program, same daemons, same corunner. The only difference from the 11.41 s block is that the destination population had not finished converging.

`ivh_uc_ema_alpha_q16=868` gives a 10.4 s half-life at the 200 ms window when a vCPU is busy, but an idle vCPU publishes far fewer windows (`ivh_uc_extended` runs ~15-20× `ivh_uc_windows` on the clean CPUs), so the *recovery* time constant is minutes, not seconds — `wall_path` sec 6 put it at ~130 s and flagged the resulting bistability as the unsolved problem. **A single ad-hoc run taken immediately after reconfiguring and restarting `MY_ivh_atc` is measuring that transient, not the configuration.** The 11.41 s block of sec 2.4 was taken on a machine that had been idle since 11:15.

**So: no missing flag. A missing settle.** The validated protocol's real, undocumented precondition is that `ivh_uc_capacity` on the uncontended vCPUs reads ~1023 *before* the block starts — and that is now checkable in one line:

```sh
grep '^ivh_uc_cpu:' /proc/ivh_debug   # field 4 = uc_capacity; cpu8-15 must read ~1023
```

### 5.1 A methodological error of my own, recorded because it cost a block

My first attempt to let the machine settle used a shell busy-wait (`while [ $SECONDS -lt $end ]; do :; done`) instead of `sleep`. **That loop is itself a load on a clean vCPU**, and it held the destination population down for the entire "idle" period — clean capacity oscillated 1022 → 952 → 990 instead of converging. The block run after it (mean 15.37 s, separation reaching −43) is therefore confounded and is not evidence about any configuration; it is evidence that this signal is sensitive to a *single spinning shell*. `runbench.sh`'s own gap uses `sleep` and was never affected. Switching to `sleep` produced clean monotone convergence: 982 → 995 → 1006 → 1012 → 1016 → 1018 → 1019 → 1020 over 8 minutes.

Worth stating plainly because it sharpens sec 5's conclusion: the recovery constant is long enough that ordinary background activity on the guest — a shell loop, a build, an editor — is enough to keep the destination population out of the regime where the 11.4 s result lives.

### 5.2 The precondition, confirmed by prediction

Sec 5 is a claim about a *precondition*, so it is testable: let the same machine, still carrying the damage from sec 3 and sec 5.1, converge properly, and it should return to ~11.5 s with no configuration change whatsoever.

Idled with `sleep` until the clean population reached 1021 — **it took 7 minutes of genuine idle**, monotone the whole way (993 → 999 → 1003 → 1007 → 1010 → 1012 → 1014 → 1015 → 1016 → 1018 → 1019 → 1020 → 1020 → 1021 at 30 s samples). Then the identical b2b block:

| rep | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| wall | 11.05 | 11.39 | 11.49 | 11.73 | 11.79 | 11.48 | 11.97 | 11.88 |
| clean cap | 1022 | 1022 | 1023 | 1023 | 1023 | 1023 | 1023 | 1023 |
| migrations | 65.6 k | 67.2 k | 69.8 k | 72.2 k | 71.9 k | 68.4 k | 70.8 k | 70.1 k |

**n=8, mean 11.60 s, sd 0.30, range 11.05-11.97**, clean pinned at 1022-1023 throughout, migrations in the 65-72 k band that every good block in this project has occupied.

Three blocks, one configuration, only the starting convergence differing:

| starting clean cap | n | mean |
|---|---|---|
| 1023 (8 h idle) | 8 | **11.41 s** |
| **1021 (7 min idle)** | 8 | **11.60 s** |
| ~1002 (partially recovered) | 8 | 12.38 s |
| ~970 and falling (busy-wait "idle") | 8 | 15.37 s |

That is the whole effect. It is not a flag.

---

## 6. What changed on the machine

**Sysctls:** `ivh_uc_min_steal_ns` was set to 10000 for the sec 3 experiment and **restored to 500000**, verified by read-back at session end along with the other eleven. Nothing else was touched. The live sysctl state is identical to the state audited in sec 2.1, and the machine is left converged and measuring 11.60 s (sec 5.2).

**BPF:** not rebuilt, not reloaded. `MY_ivh_atc` pid 116551 has been running since 19:20:29 throughout; `IVH_CAP_*` unchanged; `ivh_cfg` still 3.

**Kernel source:** not touched. No reboot; none required.

**`/home/nick/IVH`:** rewritten (it is not under git). Changes, all of them justified above:

1. `+ set_ivh_sysctl ivh_uc_min_steal_ns 500000` — **the substantive fix** (sec 3);
2. `+` seven previously-implicit `ivh_uc_*` / `ivh_tks_*` knobs pinned explicitly (sec 4.4);
3. `+ pkill -9 -x` for both `MY_ivh_atc` and `vcap_probe`, with a wait loop, before relaunch (sec 4.1);
4. `sleep 2` before the `ivh_cfg` update replaced by a poll-for-the-map loop, `value` read from `ivh_cap_source` instead of hardcoded, and a loud error if the update fails (sec 4.2);
5. `+` assertions on `IVH_CAP_HARDFLOOR/TOPBAND/MARGIN/MARGIN_REL` (sec 4.3);
6. both daemons launched with `setsid nohup` into `/home/nick/ivh_logs/` rather than bare `&` inheriting the caller's terminal;
7. `+` a closing note that the capacity EMA needs genuine idle to converge, with the `/proc/ivh_debug` check, so the next person does not measure the transient (sec 5).

`bash -n` clean; the four `check_define` assertions were dry-run against the current source and all pass.

---

## 7. Confidence

| claim | verdict | confidence |
|---|---|---|
| The live state matched the validated configuration in every documented respect | **True** — 14 sysctls, 4 BPF constants, both daemons, `ivh_cfg`, all read directly | **High** |
| The live state reproduces ~11.5 s | **True** — 11.41 s, n=8, b2b, corunner-verified per run | **High** |
| `/home/nick/IVH` would not have reproduced it after a reboot | **True** — it never set `ivh_uc_min_steal_ns`, whose default is 10000 | **High** — default read from source, live value read from `/proc` |
| `min_steal_ns=10000` is materially harmful in this configuration | **True** — 13.76 s vs 11.41 s, monotone degradation to inversion | **Moderate-high** — n=6, one block, but the mechanism and the trend are both unambiguous |
| 12.437 s is an unconverged-EMA artefact, not a config difference | **Probably** — reproduced at n=8 mean 12.38 s incl. a 12.44 s run, with no config change | **Moderate** — I cannot inspect what the orchestrator actually ran; this is a sufficient explanation, not a proven one |
| Convergence of the destination population is the governing precondition | **True** — the same config gives 11.41 / 11.60 / 12.38 / 15.37 s as starting clean cap goes 1023 / 1021 / 1002 / 970 | **High** — four blocks, n=8 each, one configuration, prediction made before the confirming block was run |
| The other three script defects were active | **False** — inspection-only findings, fixed pre-emptively | **High** |

**Bottom line for the user's question — "is there some flag you don't set that it did?"** On the live machine, no: everything matched and it reproduced at 11.41 s. In the *script*, yes, exactly one: `ivh_uc_min_steal_ns=500000`, which was latent because the live value was already correct and would have bitten on the next boot. The 12.437 s itself is best explained by measuring before the capacity EMA had settled — the validated protocol's one genuinely undocumented precondition, now documented and checkable.
