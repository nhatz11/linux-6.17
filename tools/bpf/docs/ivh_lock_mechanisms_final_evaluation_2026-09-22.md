# Adaptive spinning: final evaluation of all three lock mechanisms, 2026-09-22

Kernels `6.17.0-G-LOCK-37-bypass+` and `6.17.0-G-LOCK-38-diag+` (the latter adds
six diagnostic knobs, commit `3fceee45c870` on `ivh-rebuild-main`). VM resized
16 -> 72 -> 36 -> 72 -> 16 vCPUs across the session; **every result below is
labelled with the vCPU count it was taken at.** Migration OFF throughout
(`ivh_universal_eligible=0`) -- this evaluates adaptive spinning in isolation.
A co-runner VM supplied real host contention for the whole session
(user-confirmed).

---

## 1. Verdict

| mechanism | verdict | best evidence |
|---|---|---|
| **tier 1 + tier 2** (early bail) | **the contribution** | +2.05% vs stock PV, t=+2.17, n=100 pooled (36v) |
| **head bypass** | **works, but its shipped gate never fires** | +2.02% on top of t12, t=+2.08, n=30 (36v) |
| **lock skipping** (evict-and-requeue) | **dead** | ns on every workload, every firing rate, both PV and no-PV |

---

## 2. Lock skipping: why it does not work

### 2.1 The structural argument (from source, not measurement)

`set_pending()` lives at `qspinlock_paravirt.h:3271`, inside
`pv_wait_head_or_lock()`, which a node reaches only **by executing**. A
host-preempted successor cannot execute, so it never sets pending. The lock word
is therefore left at `locked=0, pending=0`, which is exactly the condition the
unfair steal valve tests at `:194`:

```c
if (!(val & _Q_LOCKED_PENDING_MASK) && try_cmpxchg_acquire(&lock->locked, &old, _Q_LOCKED_VAL))
```

**The bit that blocks stealers can only be set by a thread that is running.** So
preemption of the successor *opens* the escape hatch. Promotion is passive (the
predecessor writes `next->locked`); arming pending is active. Lock skipping
intervenes in a state the lock word has already resolved.

### 2.2 The frequency argument (measured, vanilla PV, 36v)

Instrument: `ivh_rot_probe_walk()` increments `ivh_rot_handoffs` on entry and
`ivh_rot_preempted` on classification, same function, same `probe` gate, one
each per handoff. No attribution, no sampling.

| workload | handoffs | successor preempted | steals past a NON-EMPTY queue |
|---|---|---|---|
| hackbench | 1,266,132 | **0.419%** | 0.69 / handoff |
| dentry | 2,140,601 | **0.834%** | **4.46 / handoff** |
| dbench | 175,721 | **1.370%** | 0.20 / handoff |

At a 500 us threshold (unambiguous long preemption only): 0.204% / 0.494% /
0.305%. **The case is rare and the rescue is abundant** -- on dentry there are
4.46 queue-jumping steals per handoff against 0.83% of handoffs needing one.

### 2.3 Dose-response: firing more changes nothing (36v)

Sweeping `ivh_pv_beat_threshold` 220000 -> 22000:

| workload | evictions | throughput |
|---|---|---|
| hackbench | 1,887 -> 4,533 (2.4x) | +0.01% ns |
| dbench | 1,220 -> 15,312 (12.6x) | +0.04% ns |
| ebizzy_mmap | 415 -> 11,508 (**27.7x**) | -0.17% ns |
| stressng_flock | 3,726 -> 80,491 (21.6x) | +5.61% ns |
| stressng_dentry | 10,453 -> 32,634 (3.1x) | +0.66% ns |

**Denominator-free and workload-independent: 27x more firing, nothing moves.**

### 2.4 It is not a bug -- two real bugs were found and fixed, and it still fails

1. **`pv_evict_walk()` marks before classifying the replacement.** The commit
   (`:2618`) happens with only `after != NULL` known; liveness is checked on the
   *next* iteration. At `hop_cap=1` **100% of evictions promote an unclassified
   node** (`ivh_evict_hop_cap == ivh_evict_marked` exactly). No back-out exists.
2. **The requeued victim re-enters ABOVE the unfair-steal camp loop**
   (`qspinlock.c:367` sits above the trylock at `:404`), and 75% of victims win
   that camp.

Fixes: `ivh_pv_evict_lookahead` (commit only once a live replacement is
confirmed -- the user's professor's rule) and `ivh_pv_requeue_nosteal` (victim
goes straight to `xchg_tail`). Each recovers most of the regression
(nosteal +6.45%/+14.76%, 8/8; look-ahead eliminates it at hop_cap<=2), and
**together they take skipping from -16.6% to -0.21% ns. Cost-neutral is the
ceiling.**

### 2.5 What it DOES do: shorten waiting, without converting

`ivh_slowpath_wait_ns / ivh_slowpath_wait_events` (`qspinlock.c:61-88`),
denominator = contended acquisitions only.

**36 vCPU:** dbench -11.43% (t=-4.85) and -10.21% (t=-3.49) at the two spin
thresholds; hackbench and dentry ns.
**72 vCPU:** dentry -12.14% (t=-3.67) and dbench -19.38% (t=-5.61) at
`spin_threshold=1048576`; dbench also -18.32% (t=-7.27) at stock.

**Throughput does not follow.** The apparent +9.21% on dentry at 72v is base
DEGRADING under a long spin threshold (-7.2%), not skip improving (+1.3%).
Against the best baseline configuration the gain is ~+1.3% (dentry) and ~+2.1%
(dbench). Skipping changes **when** waiters are served, not how much work the
lock can do.

### 2.6 No-PV: the same result, for the same reason (72v)

`ivh_pv_allow` appears in `qspinlock_paravirt.h` at exactly two places
(`:1268`, `:1280`), both in the bail/halt decision. It touches **nothing** in the
steal path; `#define queued_spin_trylock -> pv_hybrid_queued_unfair_trylock`
(`:183`) is unconditional. So `allow=0` swaps only the wait/wake vehicle.

| | vanilla PV | IVH_NOPV |
|---|---|---|
| steals / handoff | 0.69 / 4.46 / 0.20 | **0.27 / 8.26 / 0.31** |
| successor preempted | 0.42 / 0.83 / 1.37% | **0.090 / 0.060 / 0.313%** |
| skip throughput | ns | **ns** |
| skip wait | -12%..-19% SIG | **ns** (hackbench +13.56% SIG WORSE) |

**Stronger than the PV result:** in no-PV the target case is 3-14x RARER and the
rescue MORE abundant, so skipping has even less to do.

---

## 3. Head bypass: the mechanism was fine, the gate was shut

It targets the ONE state where the lock genuinely stalls: **pending SET by a head
that then got preempted.** Stealers locked out (`:194`), head not running, lock
free and unclaimable. Every other preemption case self-heals per 2.1.

The gate (`:1671-1683`) required `runs >= 3` consecutive actionable samples AND
`now - blk_start >= hold` (220000 cyc = 100 us), inside ONE `pv_wait_node()`
tenure -- `hb` is a stack local reset per call, and the observer samples every
`PV_PREV_CHECK_MASK+1 = 256` iterations.

| runs / hold | fired | actionable | taken |
|---|---|---|---|
| 3 / 220000 (shipped) | **0** | 134,900 | 0.000% |
| 1 / 2200 | 5 | 141,757 | 0.004% |
| **1 / 0** | **2,186** | 2,192 | **99.7%** |

**`actionable` COLLAPSES 134,900 -> 2,192 when firing is allowed** -- those were
~2,200 real episodes re-sampled ~61x each, not 134,900 opportunities. Every
episode is SHORTER than the 100 us threshold, so the gate never opened.

**Validation that the episodes are real:** at a 1 ms staleness threshold bypass
still fires 1,069 times/run. Publish lag can manufacture at most ~143 us of false
staleness, so those are genuine ~857 us+ absences.

**Measured contribution (36v, n=30 pooled):** +2.02% on top of tier1+tier2,
t=+2.08. Not significant on any individual workload; marginal when pooled.

---

## 4. tier 1 + tier 2: the contribution

**+2.05% vs stock PV**, t=+2.17, n=100 pooled over 5 workloads x 2 campaigns
(36v). Positive on hackbench in **3/3 independent campaigns** (+3.76% SIG,
+7.81% SIG, +3.22% ns). Also **+9.43%** (10/10, t=+7.78) in the no-PV boot,
where the alternative is burning 32,768 spin iterations with no way to halt.

---

## 5. Measurement hazards found (read before trusting any number here)

1. **qlockbench disagrees with every real workload, in both directions.** It is
   99.88% steals / 0.12% queue; hackbench and dbench are 82-88% queue. The
   "stealing dominates, queue carries 3% of traffic" claim was a qlockbench
   artifact and is RETRACTED.
2. **The backoff pedestal.** `ivh_pv_preempt_src=2` publishes an `rdtsc` in
   `pv_init_node()` that NOTHING reads in most arms, and it is worth **+11.44%**
   on qlockbench (12/12) and **-13.92%** on hackbench. It contaminated several
   earlier "wins". Always include a `src=0` control.
3. **Host drift flips significance.** Stock-PV hackbench moved 6.4s -> 7.1s
   between morning and evening, sd 11.3% -> 16.6%. Pool or re-run; never quote a
   single campaign.
4. **Arm hygiene.** `spin_mode 1`'s `reset_skip_knobs` does NOT touch
   `ivh_head_bypass_*`. A `pv` arm that does `spin_mode 1; return` inherits the
   previous arm's bypass settings. This voided one run's pv column. Zero every
   knob BEFORE the pv branch and assert it.
5. **`ivh_rot_idle_*` has 0.5-3.7% attribution** and is blind to steals
   (`pv_handoff_ack` at `qspinlock.c:543` is skipped by every `goto release`).
   Its class breakdown is suggestive, not probative.
6. **The spin tenure is 0.3-1.1 ms, not 387 us.** Measured from
   `(slowpath_wait_ns - halt_cycles) / spin_iters` = 34.92 ns/iter upper bound;
   userspace bare `pause` gives 20.4 cyc/iter as the floor. The `~45 us` figure
   in `ivh_is_cs_preempted_build_plan_2026-09-14.md:999,1686` is wrong by ~8x.
7. **Publish interval (143 us) EXCEEDS the staleness threshold (100 us)** at the
   measured iteration cost, so a healthy spinning waiter reads stale for part of
   every publish window. Shortening it 16x did NOT reduce the head-bypass stale
   fraction (5.27% -> 5.12%), so head-bypass staleness is genuine; the eviction
   path's 27%-return-under-1us population is the one this explains.
8. **`ivh_exec` is dead on this kernel** (no `/proc/ivh_debug`, no
   `PR_SET_IVH_ELIGIBLE`). Its successor is `ivh_slowpath_wait_ns/_events`.

---

## 6. Retractions made during this evaluation

- "lock skipping's cost is an extra critical section per steal" -- off by ~370x
- "the displaced head halts more" -- head halts are FLAT (+0.011..0.027/eviction)
- "halt duration is the mediator" -- decoupled 3.5x from the throughput cost
- "stealing dominates, queue is 3% of traffic" -- qlockbench-only
- "skipping is merely under-triggered" -- 27x firing changed nothing
- "head bypass is inert/harmful" -- it never fired; the gate was shut
- "upstream's TAS fallback is wrong by 3x" -- qlockbench-only; TAS ties dbench/
  ebizzy and BEATS every MCS arm on hackbench
- "tier1+tier2 beats lock skipping" (Phase A) -- confounded, skip arms had
  tier1/tier2 OFF; corrected by Phase C
