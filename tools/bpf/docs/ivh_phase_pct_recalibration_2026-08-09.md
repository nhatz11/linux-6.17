# `ivh_tks_idle_sub=0` + `ivh_tks_phase_pct=100`: the estimator now matches the steal page on runnable AND idle vCPUs, at no throughput cost — and the "idle vCPU" that still reads 0.0 is right to

**Date:** 2026-08-09 session (wall clock 2026-08-13)
**Kernel under test:** `6.17.0-rseqport73+`, **live**, `/proc/cmdline` carries `nohz=off`; branch `kernel-43-clean`, base commit `c0451f85a`
**Host:** INTEL(R) XEON(R) GOLD 6554S, `tsc_khz=2200000`, 16 vCPU guest, `CONFIG_HZ=1000`
**Predecessors:** `ivh_undershoot_correction_2026-08-09.md` (phase mechanism), `ivh_idle_sub_reboot_prep_2026-08-09.md` (the prediction this document tests), `ivh_nhextend4_cs_floor_2026-08-09.md` (the NHextend baseline this document must not disturb)
**Status:** every number below is live-measured on the running kernel tonight. No emulation, no replay. No kernel source change, no BPF change, no rebuild, no reboot, nothing committed or pushed. `/home/nick/IVH` **was** edited (sec 9) and the new operating point **is** live (sec 9).

---

## 0. The answer, up front

**Recommended and applied: `ivh_tks_idle_sub=0`, `ivh_tks_phase_pct=100`.** Everything else unchanged — in particular `ivh_tks_deadband_ns` stays at 50000 and `ivh_tks_carry_ticks` stays at 8, both tested and both left alone deliberately (sec 4).

### Accuracy — ratio of `rq->ivh_tks_steal_ns` to the `kvm_steal_time` page, contended vCPUs (cpu0-7)

| load regime | shipped `is=1,p=0` | **applied `is=0,p=100`** | blocks | reproduced |
|---|---|---|---|---|
| **continuously runnable** (8 pure spinners) | 0.655 | **0.9999** | 4 | 0.9999 / 0.9998 / 0.9999 / 1.0000 |
| **idle guest** (no guest load, corunner on) | 0.559 | **1.006** | 7 | 0.989 / 1.007 / 1.006 / 1.008 / 1.008 / 1.008 / 1.016 |
| **hackbench** (fine-grained blocking) | 0.232 | **1.143** | 6 | 1.097 / 1.117 / 1.162 / 1.163 / 1.159 / 1.162 |

The two regimes the task named — *contended* (runnable) and *idle* — land at **1.0000 and 1.006**. The third regime, fine-grained blocking load, **overshoots by ~14 %**; that is a real residual, it is stated rather than buried, its mechanism is identified in sec 3.4, and it is still a 5× improvement on the shipped 0.232.

### The idle/clean-vCPU floor, characterised honestly rather than chased

On the genuinely-unstolen vCPUs (cpu8-15) the estimator reads **0.0 ms** at both settings, against 5–8 ms of real steal per 60 s. That is **not** a calibration failure and **must not** be "fixed":

> Their real steal arrives in **8–14 µs quanta** — 4.9–8.3 ms spread over 559–648 steal-bearing ticks in a 60.5 s window. The contended vCPUs' arrives in **2.5–3.4 ms quanta** in the same window. That is a **~250× gap in event size**, and a tick-gap estimator can only see an event that pushes a tick past the 50 µs deadband. The missed quantity is **0.008–0.014 % of wall**. Sec 5.

### No regression, on both required benchmarks, reproduced

| benchmark | shipped | **applied** | paired difference |
|---|---|---|---|
| **hackbench** `-T -g 1 -f 8 -l 400000`, 2 batches, 12 interleaved rounds (IVH-off same batch: 15.855 s) | 11.370 s (sd 0.225) | **11.383 s** (sd 0.303) | **+0.013 s, SE 0.037, t=0.35, p≈0.73** |
| **NHextend** `loop_spin=400000`, shipped sysctls, 3 batches, 11 clean rounds, vs same-round IVH-off | −3.06 % (sd 1.19) | **−1.96 %** (sd 2.05) | **+1.15 pp, SE 0.73, n.s.** |
| **NHextend** `loop_spin=400000`, NHextend4-**tuned** sysctls, same 11 rounds | +7.72 % (sd 1.18) | **+8.00 %** (sd 1.89) | **+0.27 pp, SE 0.44, n.s.** |

Migrations are unchanged (hackbench 69 379 → 69 448, 0.1 % apart). Clean-vCPU `ivh_uc_capacity` is unchanged (1021–1023 in all 24 IVH-on hackbench runs at both settings).

---

## 1. Live state, verified rather than assumed

Checked before anything was touched.

| check | result |
|---|---|
| `uname -r` | `6.17.0-rseqport73+` |
| `/proc/cmdline` | `... quiet splash nohz=off ...` — **`nohz=off` live**, which `idle_sub=0` requires |
| `/proc/sys/kernel/ivh_tks_idle_sub` | **exists, = 1** — the reboot `ivh_idle_sub_reboot_prep` prepared for **has happened**; the branch is live-testable for the first time |
| `ivh_tks_phase_pct` | 0 |
| `ivh_time_left_threshold_ns` / `ivh_eval_cooldown_ns` / `ivh_max_concurrent` | 4000000 / 50000 / 8 — shipped defaults, **not** the NHextend4-tuned values |
| `ivh_tks_deadband_ns` / `carry_ticks` / `uc_min_steal_ns` / `capacity_threshold` | 50000 / 8 / 500000 / 1010 |
| `ivh_steal_source` / `cap_source` / `uc_used_source` / `universal_eligible` | 2 / 3 / 0 (WALL) / 1 |
| daemons | exactly one `MY_ivh_atc` (7868/7872), exactly one `vcap_probe -p 200 -s 5000` (25006/25007) |
| `/proc/vcap_info` | populated, 64 lines |
| `git diff` | clean over `kernel/`, `include/`, `tools/bpf/*.bpf.c`; only `NHextend4.c` + untracked docs |

**Corunner, measured (16 busy loops, `/proc/stat` steal delta):** cpu0-7 **66.2–67.0 %**, cpu8-15 **0.2–0.3 %**. Strongly skewed and consistent with the 67.6–67.8 % of the `undershoot_correction` session. Re-checked implicitly on every one of the 24 hackbench runs in sec 7 (contended 32–37 %, clean 0.0–0.2 %, every run).

**Independent reproduction of the prior work, first try, before any new measurement:** CR load at `is=1, p=0` gave contended ratio **0.636–0.648**, deficit/event **0.999–1.003 ticks**, and `didle == 0` on every contended vCPU — matching `ivh_undershoot_correction_2026-08-09.md` sec 3.2 exactly. The measurement pipeline is sound and the prior result stands.

---

## 2. Method

Instrumentation is the predecessor's, reused rather than reinvented (scratchpad `snap.bt` / `snap.sh` / `delta.sh` / `agg.sh`):

- per-CPU `rq->ivh_tks_{steal_ns,samples,events,carry_c,skipped}` read out of `runqueues` via `__per_cpu_offset` with `bpftrace`;
- ground truth is the **`kvm_steal_time` page itself** (`kaddr("steal_time")` + per-CPU offset), nanosecond resolution, not `/proc/stat`'s 10 ms quantisation;
- `nsecs` taken inside the same `BEGIN` block, so **every window length is measured**, and `bpftrace`'s attach latency falls outside the window rather than inside it;
- `/proc/stat` captured in the same snapshots for the idle/busy/steal cross-check.

Three load regimes:

- **CR** — 8 pure spin loops pinned to cpu0-7, zero idle. Isolates the tick-phase mechanism; `didle ≡ 0` here, which is what makes `idle_sub` a provable no-op on this regime.
- **IDLE** — no guest load at all, corunner on. This is the regime the estimator has never been calibrated in, because before `idle_sub` existed the idle debt swamped it.
- **HB** — `hackbench -T -g 1 -f 8 -l 400000`, the project's reference workload and the hard case.

**Ordering discipline, applied because this project has been burnt by its absence:** every grid was run in at least two independent orderings (ascending then descending, or a rotating interleave), so monotone session drift cannot alias onto one arm. `A2_IDL` (sec 2.1) is a worked example of that discipline catching something.

### 2.1 One block discarded, and the objective reason

The rep-2 IDLE block (descending order) was **discarded whole**. Two independent tells fired together mid-block: contended true steal fell from 500–640 ms/20 s to 300–350 ms/20 s (the host backed off ~40 %), and 1.1–2.0 s of clean-vCPU phantom appeared **on both arms, including `idle_sub=1`**, where it is structurally impossible under `nohz=off`. Both are host-side, neither is attributable to the knobs. Same class as the batches `ivh_nhextend4_cs_floor_2026-08-09.md` sec 8 discarded. It was replaced by a rotating 4-rep interleave, not by re-running the ordering that happened to look better.

---

## 3. Phase A — the grid

### 3.1 Continuously-runnable load: `idle_sub` is a provable no-op, `phase_pct=100` is exact

Contended vCPU ratio, two independent orderings (rep 1 ascending, rep 2 descending):

| `phase_pct` | `idle_sub=1` | `idle_sub=0` |
|---|---|---|
| 0 | 0.6464 / 0.6585 | 0.6600 / 0.6603 |
| 25 | 0.7422 / 0.7427 | 0.7441 / 0.7446 |
| 50 | 0.8285 / 0.8292 | 0.8300 / 0.8287 |
| 75 | 0.9134 / 0.9140 | 0.9138 / 0.9143 |
| **100** | **0.9999 / 0.9998** | **0.9999 / 0.9998** |

Perfectly linear in `pct`, as the mechanism requires, and **the two `idle_sub` columns are identical to within 1.4 %** — because `didle == 0` on every contended vCPU in every one of these blocks, so the line `if (!READ_ONCE(ivh_tks_idle_sub)) d_idle_c = 0;` is literally a no-op here.

This confirms `ivh_idle_sub_reboot_prep_2026-08-09.md` sec 4.1's central argument **by execution rather than by inspection**: the `phase_pct=100` calibration was not conditional on the idle subtraction, and it survives the fix untouched. Re-confirmed twice more in the final verification block (sec 6): **0.9999, 1.0000**.

### 3.2 Idle guest: this is where the fix earns its reboot

Four reps, rotating interleave, contended vCPU ratio:

| arm | rep 1 | rep 2 | rep 3 | rep 4 | mean |
|---|---|---|---|---|---|
| `is=1, p=0` (shipped) | 0.5067 | 0.5528 | 0.5547 | 0.5450 | **0.534** |
| `is=1, p=100` | 0.7358 | 0.7737 | 0.8070 | 0.8259 | 0.786 |
| `is=0, p=0` | 0.6785 | 0.6744 | 0.6938 | 0.6614 | 0.677 |
| **`is=0, p=100`** | **0.9890** | **1.0070** | **1.0055** | **1.0081** | **1.0024** (sd 0.009) |

Two things beyond the headline:

1. **The prediction held.** `ivh_idle_sub_reboot_prep` sec 3.3 predicted 1.03–1.06 from an out-of-kernel emulator. Measured **1.006–1.016** across seven blocks — the same side of 1.0, marginally better than predicted. The emulator was honest.
2. **`is=0, p=100` is also the most drift-robust arm.** Host contention drifted monotonically across those four reps, and it shows: `is=1, p=100` walked 0.736 → 0.826 while the true steal moved. `is=0, p=100` did not move at all (sd 0.009). An estimator whose reading is invariant to the thing it is measuring changing is doing its job; one whose reading tracks the drift is partly measuring its own residual.

Deficit per event on this regime at `is=0, p=0` measures **0.94–0.99 tick** (predicted 0.87), which is why `p=100` lands a whisker above 1.0 rather than exactly on it.

### 3.3 Hackbench: 5× better, and honestly overshooting

Three reps interleaved, then a second independent block:

| arm | block 1 (3 reps) | block 2 (3 reps) | pooled |
|---|---|---|---|
| `is=1, p=0` (shipped) | 0.214 / 0.206 / 0.221 | 0.293 / 0.225 / 0.233 | **0.232** |
| `is=1, p=100` | 0.304 / 0.395 / 0.306 | — | 0.335 |
| `is=0, p=0` | 0.408 / 0.429 / 0.399 | — | 0.412 |
| **`is=0, p=100`** | 1.097 / 1.117 / 1.162 | 1.163 / 1.159 / 1.162 | **1.143** |

This is `ivh_undershoot_correction_2026-08-09.md` sec 4.2's residual being closed and then some: the idle-subtraction term was worth 0.232 → 0.412 on its own, and the two together carry it from 0.232 to 1.143.

### 3.4 Why the third regime overshoots, stated as a mechanism rather than an excuse

`phase_pct` adds exactly **one whole tick per counted event**. The *true* deficit per event is one tick only when a preemption burst reliably straddles a tick deadline. Measured directly, at `is=0, p=0`:

| regime | measured deficit / event | consequence of adding 1.000 tick/event |
|---|---|---|
| continuously runnable | **1.000 tick** | exact |
| idle guest | 0.94–0.99 tick | +0.6 % |
| hackbench | **0.81–0.85 tick** | **+14 %** |

The overshoot is not miscalibration; it is that on fine-grained blocking load a counted event is on average only ~0.83 of a skipped tick, because bursts frequently begin and end inside one tick interval. It is the same physics that makes the clean-vCPU floor of sec 5 exist, seen from the other side.

Event counts corroborate: `is=0` roughly triples the counted events on hackbench (600–650 vs 206–246 at `is=1`), because idle intervals whose excess was previously driven a full tick negative can now clear the deadband.

---

## 4. Two knobs tested and deliberately **not** changed

### 4.1 `ivh_tks_deadband_ns` — the obvious lever, and it does not work

`ivh_idle_sub_reboot_prep` sec 6.1 names the deadband as the correct lever if `idle_sub=0` misbehaves, so it was swept, at `is=0, p=100`, on hackbench, 3 reps interleaved:

| `deadband_ns` | hackbench ratio | contended events |
|---|---|---|
| **50 000 (shipped)** | 1.161 / 1.159 / 1.162 | ~600 |
| 250 000 | 1.154 / 1.148 / 1.135 | ~620 |
| 1 000 000 | **0.631 / 0.661 / 0.637** | **~190** |

250 µs buys 1.5 points of the 14. 1 ms overshoots hard in the other direction and collapses the event count by 3×. **The extra events at `idle_sub=0` are not small-excess spurious ones** — they carry ~1.8 ms of excess each — so raising the deadband destroys real signal well before it touches the residual. Left at 50000. This keeps the change surface at exactly two knobs.

### 4.2 An interior `phase_pct` — computed, and rejected on principle

The three regimes are linear in `pct` at `is=0`:

```
CR    ratio = 0.660 + 0.00340·p        IDLE  ratio = 0.680 + 0.00326·p
HB    ratio = 0.412 + 0.00732·p
```

The minimax value across all three is **p ≈ 87**, giving CR 0.954 (−4.6 %), IDLE 0.962 (−3.8 %), HB 1.045 (+4.5 %) — a ±4.6 % envelope instead of 0.0 % / +0.6 % / +14.3 %.

**Not recommended, for three reasons, in decreasing order of weight:**

1. **100 is derived, 87 would be fitted.** 100 follows from the tick being an `HRTIMER_MODE_ABS_PINNED_HARD` hrtimer forwarded by `TICK_NSEC` — a burst spanning deadlines fires one interrupt on resume and `hrtimer_forward()` skips the rest, so the loss is one *whole* tick. That derivation is exact on the regime where it can be checked, and it checks out to 0.01 %. 87 has no mechanism behind it and would be a constant fitted to this host's hackbench, on a night when this project has repeatedly documented the host moving underneath the measurement.
2. **It is the same mistake the multiplier idea was rejected for.** `ivh_undershoot_correction_2026-08-09.md` sec 6(b) rejected a fixed gain precisely because a workload-fitted constant does not self-adapt. Picking 87 to average three workloads is that argument with extra steps.
3. **The two regimes the task actually names are the two 100 gets exactly right.** Degrading both to −4 % to improve a third by 10 is a worse answer to the question that was asked.

The exact numbers are recorded here so the trade is re-derivable without re-running the grid.

---

## 5. The idle/clean-vCPU detection floor — characterised, not chased

The task asked to characterise the floor rather than pursue an impossible match. Measured on ground truth, 60.5 s window, idle guest, shipped config.

`rq->preemptions` (`cputime.c:296`) increments once per tick on which `steal_account_process_time()` booked **nonzero steal from the KVM steal page**. It is therefore a steal-page-derived count of steal-bearing ticks, completely independent of the tks estimator, and it is exported per-CPU as field 1 of `/proc/vcap_info`.

| | real steal (steal page) | steal-bearing ticks | **real steal per event** | tks estimate | tks events |
|---|---|---|---|---|---|
| **clean cpu8-15** | 4.9 – 8.3 ms | 559 – 648 | **8 – 14 µs** | **0.0 ms** | 2 – 4 |
| **contended cpu0-7** | 1343 – 1632 ms | 424 – 637 | **2.5 – 3.4 ms** | 830 – 847 ms | 399 – 547 |

**The floor is a ~250× gap in event size, not a calibration error.** A tick-gap estimator books an event only when a preemption pushes a tick past the 50 µs deadband. A 10 µs preemption cannot do that at *any* `phase_pct`, and even a zero deadband would only book µs-scale excess. The quantity being missed is **0.008–0.014 % of wall**.

So the correct statement for the paper, and the correct engineering posture, is:

> On uncontended vCPUs the estimator reports 0.0 and **should**. Reporting 0 for 0.01 % of wall delivered in sub-deadband quanta is the desired behaviour of a steal detector, not a miss. The estimator's useful domain is preemptions at or above one tick period, and within that domain it is now exact.

### 5.1 The one honest cost of `idle_sub=0`: intermittent clean-vCPU phantom

Removing the idle subtraction **does** occasionally book phantom steal on clean vCPUs, which the pre-reboot emulator predicted it would not. Measured across every block in this session (totals over all 8 clean vCPUs per window):

- `idle_sub=1`: **0.0 ms in essentially every cell** (3 exceptions in ~30 cells, max 10.0 ms).
- `idle_sub=0`: **0.0 ms in the majority of cells**, but intermittently 15–200 ms, and once **585 ms** (worst single vCPU **81 ms / 20 s = 0.40 % of wall**). Typical nonzero case: 25–37 ms/20 s = 0.12–0.18 %.

Three things bound it:

1. **It is episodic, not systematic.** The final verification block (sec 6) read 0.0 ms on all four `idle_sub=0` cells. It correlates with the same host episodes that produce the discarded blocks.
2. **It is 6–16× below the level a previous report already judged tolerable.** `ivh_undershoot_correction_2026-08-09.md` sec 4.3 measured 54.4 ms over 6.8 s (0.8 % of wall) at `carry_ticks=1` and called it "not obviously fatal". The worst case here is 0.40 %; the typical case is 0.15 %.
3. **It does not reach the gate.** Clean-vCPU `ivh_uc_capacity` read **1021–1023 in all 24 IVH-on hackbench runs at both settings** (sec 7). `ivh_uc_tick()` keeps its own unconditional idle subtraction and its `min(d_steal_c, avail_c)` per-tick clamp, so steal recovered during idle intervals is clamped away before it can reach capacity — exactly as `ivh_idle_sub_reboot_prep` sec 2.6 predicted, now measured rather than predicted.

This is the item most worth an independent re-check, and it is the reason sec 10 records it at only Moderate-high confidence.

---

## 6. Final independent reproduction of the headline, taken last

Run after all of Phase B, hours after the Phase A grid, as a fresh check that the claim survives the session:

| | rep 1 | rep 2 |
|---|---|---|
| CR, `is=1, p=0` | 0.6573 | 0.6565 |
| **CR, `is=0, p=100`** | **0.9999** | **1.0000** |
| IDLE, `is=1, p=0` | 0.6029 | 0.5788 |
| **IDLE, `is=0, p=100`** | **1.0080** | **1.0083** |

Clean-vCPU phantom in all four `is=0` cells of this block: **0.0 ms** (one 0.6 ms).

---

## 7. Phase B — hackbench, reproduced

`/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000`, matching `ivh_final_tsc_only_build_2026-08-08.md` / `ivh_wall_path_validation_2026-08-08.md`. Three arms — IVH **off**, shipped tks, applied tks — **interleaved and rotated within every round**, 5 s settle, all other sysctls reset per run. Per-run corunner check from `/proc/stat`. Two independent batches, 6 rounds each.

| arm | n | wall mean | sd | vs IVH-off | post-run cap cont / clean | migrations |
|---|---|---|---|---|---|---|
| IVH **off** | 12 | **15.855 s** | 0.262 | — | 892 / 1023 | 0 |
| shipped `is=1, p=0` | 12 | **11.370 s** | 0.225 | **−28.3 %** | 903 / 1023 | 69 379 |
| **applied `is=0, p=100`** | 12 | **11.383 s** | 0.303 | **−28.2 %** | **641** / **1022** | 69 448 |

Paired within round, applied − shipped, all 12 rounds:

```
-0.044  +0.128  +0.388  -0.101  -0.092  -0.012
-0.090  +0.006  -0.027  -0.038  +0.047  -0.011     seconds
mean +0.0128 s   sd 0.129   SE 0.037   t = 0.35   p ~ 0.73
```

Ten of twelve rounds fall inside ±0.1 s. **No detectable difference.** Corunner held on every run (contended 32–37 %, clean 0.0–0.2 %).

**One real behavioural change, and it is in the intended direction.** Contended `ivh_uc_capacity` settles at **641** instead of 903, because the estimator now reports the steal that is actually there; clean capacity is untouched at 1022. Gate separation therefore widens from ~120 points to ~380. Migrations did not change (0.1 %), so the wider gate was not the binding constraint at this workload — but the signal the gate is reading is now a much better-conditioned one.

**On the hard-floor failure mode** (`ivh_solution_search_2026-08-09.md` sec 9: the whole population falls below `IVH_CAP_HARDFLOOR = 880` and IVH silently stops migrating): `IVH_CAP_HARDFLOOR` is tested against **destination** capacity only (`MY_ivh_atc.bpf.c:614`, `:925`, `:929`). This change moves the **source** population down by ~260 points and leaves the destination population bit-for-bit where it was (1021–1023). It therefore does **not** make that failure mode more likely.

---

## 8. Phase B — NHextend at `loop_spin=400000`, reproduced, on both sysctl configurations

Harness is `ivh_nhextend4_cs_floor_2026-08-09.md`'s own (`NHextend4_exp`, default env knobs, verified there to reproduce NH3 exactly; `NHEXTEND_DURATION=10`, `-n -l 16`, `/proc/vcap_info` asserted populated before every run, every relevant sysctl reset per run). **Five arms interleaved and rotated within each round:**

`OFF` · `DEF_ship` · `DEF_cand` · `TUNED_ship` · `TUNED_cand`

where `TUNED` = `time_left_threshold_ns=200000, eval_cooldown_ns=1000000, max_concurrent=4` — the NHextend4 tuned point, which is *not* live/shipped — and `cand` = `idle_sub=0, phase_pct=100`.

### 8.1 Contamination rule, declared before analysis

Drop a **whole round** (all five arms) if any IVH-on arm's `cs_active_ns` exceeds **1.3×** the same round's IVH-off arm. Applied to 13 rounds it flagged exactly two:

| | clean rounds (11) | flagged rounds (2) |
|---|---|---|
| max CS ratio | 0.78 – 0.83 | **1.63**, **1.88** |

The separation is total, so this is a rule and not a judgement call. Both flagged rounds show the signature of the host episodes documented in NHextend4 sec 8 (IVH-off unaffected, IVH-on arms collapsing to 4 734 / 5 825 / 6 168 iterations with migration counts 3–10× out of band).

### 8.2 Result, 11 clean rounds across 3 independent batches

| arm | n | mean Δ vs same-round IVH-off | sd |
|---|---|---|---|
| `DEF_ship` (shipped sysctls, shipped tks) | 11 | **−3.06 %** | 1.19 |
| **`DEF_cand`** (shipped sysctls, applied tks) | 11 | **−1.96 %** | 2.05 |
| `TUNED_ship` (tuned sysctls, shipped tks) | 11 | **+7.72 %** | 1.18 |
| **`TUNED_cand`** (tuned sysctls, applied tks) | 11 | **+8.00 %** | 1.89 |

Paired within round, candidate − shipped:

| config | paired mean | sd | SE | verdict |
|---|---|---|---|---|
| default sysctls | **+1.15 pp** | 2.42 | 0.73 | not significant, sign positive |
| tuned sysctls | **+0.27 pp** | 1.45 | 0.44 | not significant, sign positive |

**No regression on either operating point.** The calibration is throughput-neutral at the CS length where NHextend4 established IVH's floor.

### 8.3 Absolute baselines sit ~2–3 pp below NHextend4's, and why that is not a discrepancy

NHextend4 recorded pooled +0.78 % (default) and +9.81 % (tuned) at this point; tonight's *shipped-tks* arms — byte-identical configuration — read −3.06 % and +7.72 %. **Both arms moved together**, which is the signature of the host-contention drift this project has documented repeatedly (`ivh_solution_search_2026-08-09.md` sec 9; NHextend4 sec 8). The tuned figure sits just below NHextend4's per-batch range (+8.75 % to +10.63 %) and inside the range once the orchestrator's +6.4 % is included; the default figure sits ~2 pp below its −0.94 %…+2.50 % spread.

Nothing in this document rests on the absolute values: **every comparison is paired within an interleaved round**, so a common host offset cancels exactly. I checked the harness for the class of mismatch that produced a spurious discrepancy in NHextend4 (`NHEXTEND_DURATION` defaulting to 5 s where the other tester used 10 s): tonight's runs use `NHEXTEND_DURATION=10`, matching the batches those reference numbers came from, and the same binary and flags.

### 8.4 A measurement finding that changes how NHextend's `HP %` must be read

`get_steal_and_preemptions()` (`kernel/sched/core.c:443`) switches on `ivh_steal_source`:

```c
switch (READ_ONCE(ivh_steal_source)) {
case 2:
        *steals_time = READ_ONCE(rq->ivh_tks_steal_ns);
        return;
...
#ifdef CONFIG_PARAVIRT
        *steals_time = paravirt_steal_clock(cpunum);
```

**`ivh_steal_source=2` is the shipped value.** So `/proc/vcap_info`'s steal column is `rq->ivh_tks_steal_ns` — **the TSC estimator** — not the hypervisor steal page. NHextend's `Host-preempted CS cycles` / `HP %` is computed from that column, and is therefore **a function of `phase_pct` and `idle_sub`**.

`ivh_nhextend4_cs_floor_2026-08-09.md` describes `/proc/vcap_info` as "real host steal, not a guest-side proxy" (header, sec 4). That is true only at `ivh_steal_source=0`. At the shipped 2 it is the estimator. Consequences:

- **`HP %` is not a valid cross-arm invariant in any experiment that varies the tks knobs.** Iterations/wall time is. That is why sec 8.2 reports only iterations.
- NHextend4's *within-config* HP % conclusions (sec 7.2's lock-utilisation analysis) are unaffected — every arm there ran at the same tks setting — but they are measuring the estimator's view of holder preemption, not KVM's, and the two now differ by roughly 5× at this workload.
- `ivh_uc_steal_ns()` (`core.c:2524`) performs the identical switch, so the same series feeds `ivh_uc_capacity` and hence the migration gate. That is precisely why sec 7 measured the gate rather than assuming it.

Anyone wanting genuine host ground truth from that file must set `ivh_steal_source=0` first, or read the `kvm_steal_time` page directly as this document does.

---

## 9. State left on the machine

**The recommended operating point was applied**, per the task's instruction to ship it once confirmed on accuracy and both benchmarks:

```
kernel.ivh_tks_idle_sub  = 0        (was 1)
kernel.ivh_tks_phase_pct = 100      (was 0)
```

`/home/nick/IVH` was edited to match, following the script's established pattern (real measured numbers in the comment, as `ivh_uc_min_steal_ns` and `ivh_selection_trylock` are documented there): both `set_ivh_sysctl` lines carry the three-regime accuracy table, the clean-vCPU floor, the `nohz=off` precondition, the hackbench and NHextend validation numbers, and a pointer to this document. `bash -n` clean. The `ivh_tks_deadband_ns` comment gained the sweep that justifies leaving it at 50000.

**Everything else restored to shipped defaults and verified by fresh `cat`:**

```
ivh_tks_deadband_ns 50000    ivh_tks_carry_ticks 8        ivh_time_left_threshold_ns 4000000
ivh_eval_cooldown_ns 50000   ivh_max_concurrent 8         ivh_migration_timeout_ns 500000
ivh_selection_trylock 1      ivh_universal_eligible 1     ivh_uc_min_steal_ns 500000
ivh_capacity_threshold 1010  ivh_steal_source 2           ivh_cap_source 3
ivh_uc_used_source 0
```

- **No reboot, no kernel build or install, no BPF rebuild or reload, no GRUB change.** Running kernel is still `6.17.0-rseqport73+`.
- **Kernel source not touched.** `git diff` over `kernel/`, `include/`, `tools/bpf/*.bpf.c` is empty. `NHextend4.c`'s modification is the pre-existing one from the previous session.
- **`NHextend3.c`, `NHextend3`, `vcap_probe`, `ivh_verify.sh`, `/etc/default/grub`: not touched.**
- **Daemons as found:** one `MY_ivh_atc` (7868/7872), one `vcap_probe -p 200 -s 5000` (25006/25007), `/proc/vcap_info` 64 lines.
- **No stray guest load.** All spinners and hackbench instances exited and verified gone.
- **Nothing committed, nothing pushed.**
- New scratchpad tooling: `focus.sh`, `focus2.sh`, `meas2.sh`, `pk.sh`, `hbb.sh`; raw data `A1_*.txt`, `A2_*.txt`, `F_*.txt`, `D_hb.txt`, `V_*.txt`, `floor.txt`, `pk_B{1,2,3}.csv`, `hb_H{1,2}.csv`.

**Reverting is one command** if this is judged wrong: `sudo sysctl -w kernel.ivh_tks_idle_sub=1 kernel.ivh_tks_phase_pct=0`, plus reverting the two `/home/nick/IVH` lines.

---

## 10. Honest confidence

| claim | verdict | confidence |
|---|---|---|
| `idle_sub` is a provable no-op on continuously-runnable load (`didle ≡ 0`) | **True** | **High** — measured `didle == 0` in every CR block; two `idle_sub` columns identical across a full 5-point `phase_pct` sweep in both orderings |
| `phase_pct=100` is exact on CR load and survives the idle-sub fix | **True** | **High** — 0.9999/0.9998/0.9999/1.0000, four blocks, two orderings, hours apart |
| `is=0, p=100` reaches 1.006 on idle-guest load | **True** | **High** — 7 blocks, sd 0.009, and the most drift-robust of the four arms |
| The pre-reboot emulator's 1.03–1.06 prediction held | **True** | **High** — measured 1.006–1.016, same side, marginally better |
| `is=0, p=100` overshoots ~14 % on hackbench | **True** | **High** — 6 observations, two independent blocks, 1.097–1.163 |
| …and the cause is a per-event deficit of 0.83 tick, not miscalibration | **True** | **Moderate-high** — measured directly at `p=0` in the same windows; mechanism consistent with the event-count inflation, not isolated by a separate intervention |
| The deadband can fix that overshoot | **False** | **High** — 250 µs buys 1.5 of 14 points; 1 ms overshoots to 0.64 and collapses events 3× |
| Clean/idle vCPUs are structurally undetectable, not miscalibrated | **True** | **High** — 8–14 µs per real event vs 2.5–3.4 ms on contended vCPUs, from a steal-page-derived counter independent of the estimator |
| `idle_sub=0` intermittently books clean-vCPU phantom (up to 0.40 % of wall) | **True** | **Moderate-high** — clearly attributable (interleaved `idle_sub=1` cells read 0.0 between them), but episodic and host-correlated; magnitude is a range, not a constant |
| …and it does not reach `ivh_uc_capacity` | **True** | **High** — clean capacity 1021–1023 in all 24 IVH-on hackbench runs at both settings |
| hackbench is unaffected | **True** | **High** — 2 batches, 12 interleaved rounds, paired +0.013 s, p≈0.73, 10/12 rounds inside ±0.1 s |
| NHextend at 400 k is unaffected, at **both** sysctl configs | **True** | **Moderate-high** — 3 batches, 11 clean rounds, paired +1.15 pp / +0.27 pp, both n.s.; n=11 cannot exclude a ~1.5 pp effect |
| Tonight's absolute NHextend baselines differ from NHextend4's because of host drift, not the change | **True** | **High** — both tks arms moved together by the same amount; all comparisons are within-round paired |
| `/proc/vcap_info`'s steal column is the estimator, not the steal page, at `ivh_steal_source=2` | **True** | **High** — read directly from `core.c:443` |
| An interior `phase_pct ≈ 87` would be better overall | **Rejected, not refuted** | It genuinely minimaxes to ±4.6 %; it is rejected because it is a fit where 100 is a derivation, and because it degrades both regimes the task named |

**Overall.** The idle-subtraction fix does what it was rebooted for, and `phase_pct=100` carries over to the new regime intact rather than needing re-fitting — both of which the pre-reboot analysis predicted and both of which are now executed rather than emulated. The estimator reproduces the hypervisor's own steal accounting to **0.01 % on continuously-runnable load and 0.6 % on an idle guest**, up from 35 % and 47 % undershoot, at no measurable throughput cost on either validation benchmark and at either NHextend sysctl operating point. Two things are left honestly open: a **+14 % overshoot on fine-grained blocking load**, whose mechanism is identified and whose only obvious lever is measured not to work; and an **intermittent, host-correlated clean-vCPU phantom of up to 0.4 % of wall** that is demonstrably confined below the capacity signal but is the item most deserving of an independent re-check.
