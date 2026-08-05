# `ivh_uc_capacity` gate recalibration — measurement, verdict, and redesign

**Date:** 2026-08-03
**Tree:** `/home/nick/kernels/linux-6.17-rseqport`, branch `kernel-43-clean`, HEAD `a0bfc3784`
**Boot:** `6.17.0-rseqport69-byevcap+` (Build A of the retirement plan is live: `ivh_uc_*` present in `/proc/ivh_debug`)
**Status:** DESIGN ONLY. No source touched, no BPF edited, no git, no map writes. `ivh_cap_source` left at 0 throughout.
**Follows:** `ivh_vcap_retirement_build_plan_2026-08-03.md` (G1/G2 outcome), `ivh_tsc_final_state_report_2026-08-02.md` §6.3.

---

## 0. Executive summary

**The load-bearing question — "is there enough real separation in `ivh_uc_capacity` to gate a
migration decision on?" — has a two-part answer, and the split is the whole finding:**

- **Ordinal separation: yes, and it is essentially perfect.** Across 3,217 independent 16-CPU samples
  spanning three qualitatively different load regimes, the EMA'd `ivh_uc_capacity` ranked every
  host-contended vCPU below every uncontended vCPU **100.00 % of the time**, with a worst-case
  inter-group gap of **77** points on the 1024 scale.
- **Cardinal separation: no, and it cannot be fixed by picking a better constant.** The *position* of
  the contended cluster on the 1024 scale is a function of **guest demand**, not of host pressure. The
  same eight physical vCPUs, under an unchanged host corunner, read **506–520** (saturated),
  **742–823** (guest idle), and **890–936** (bursty hackbench). That is a 430-point excursion with the
  independent variable — host contention — held constant. `IVH_CAP_FLOOR = 850` is on the correct side
  of the population in two of those three regimes and on the wrong side in the third, and the third is
  **the regime IVH actually operates in.**

**Therefore: recalibrating `IVH_CAP_FLOOR` to a new number is a dead end, but `ivh_uc_capacity` is
not.** The redesign is to change the *shape* of both BPF capacity gates from absolute-threshold to
relative-with-margin plus population-normalised, which is exactly the information the signal reliably
carries. A modelled conjunction gate — `dest ≥ src + 50` **and** `dest ≥ scan_max − 50` — classifies
**205,888 / 205,888** candidate pairs correctly with **zero** lateral (contended → contended)
acceptances and **zero** empty destination sets, across all three regimes, and is robust over a wide
parameter box (D ∈ [25,75] × K ∈ [25,75]) rather than sitting on a knife edge.

This is a BPF-only change plus two sysctl writes. No kernel rebuild.

**§8 records a separate, strategically important finding:** the reason `ivh_uc_capacity` loses its
cardinal separation is entirely the choice of `avail` (non-idle wall time) as the denominator. The
numerator and the raw wall-clock denominator are **both already computed inside `ivh_uc_tick()`**;
the ratio `stolen / elapsed` — which the function throws away — separates the same two CPU groups by
**5×–130×** in the same three regimes where the shipped ratio separates by 1.09×–1.97×. That is the
honest Option B, and it costs one `u64` accumulator and one extra ratio.

---

## 1. Provenance — what was measured, and proof the guest was genuinely contended

All measurement was read-only. `ivh_cap_source` stayed at 0 (vcap authoritative) for the entire
session; `ivh_universal_eligible`, `ivh_uc_shadow` and `ivh_decision_shadow` were toggled to 1 for
instrumentation and returned to 0 at the end (verified). One `vcap` (`-p 200 -s 5000`) and one
`MY_ivh_atc` were running throughout, singletons confirmed by `pgrep` before and after.

**Host contention was verified before anything was trusted.** Per-CPU steal from `/proc/stat` and
from `rq->ivh_ref_steal_ns` (`ivh_ref_cpu:`) both show a persistent host corunner pinned against the
pCPUs backing **cpu0–7**, with cpu8–15 essentially uncontended. Cumulative `ivh_ref_steal_ns` over the
8 h uptime: cpu0–7 = **645–650 s**, cpu8–15 = **11.8–12.6 s** — a **54×** standing asymmetry. Live
steal rates are in §8.1.

Throughout this document **"contended group" = cpu0–7** and **"healthy group" = cpu8–15**. That
grouping is a property of the host, established independently of any signal under test, which is what
makes it usable as ground truth.

Sampler: `ivh_uc_cpu:` line from `/proc/ivh_debug` at 5–10 Hz. Columns used —
`$5 uc_capacity`, `$7 uc_acct`, `$8 raw_wall` (pre-EMA), `$10/$11` open-window `avail_c/stolen_c`.

| run | regime | duration | samples (16-CPU) |
|---|---|---|---|
| **run1** | one `ivh_exec -v hackbench -T -g 1 -f 8 -l 400000` (12.0 s) then rest | 90 s @10 Hz | 839 |
| **run2** | **ten** back-to-back identical hackbench runs — sustained load | 150 s @10 Hz | 1,366 |
| **run3** | 60 s guest-idle → 90 s pure busy-loop pinned 1:1 on cpu0–7 → idle | 150 s @5 Hz | 1,012 |
| — | **total** | | **3,217** |

run3 is the controlled experiment: it varies **only** guest demand on the contended CPUs, with host
pressure untouched, and is therefore the direct test of demand-dependence.

---

## 2. The measured distribution

### 2.1 run1 — one hackbench burst, then rest (the briefing's condition)

`uc_capacity`, mean per 5 s bin (contended | healthy):

```
t=  0s  688 657 665 646 673 645 655 638 | 1012 1013 1012 1012 1012 1012 1011 1013
t= 15s  796 785 780 770 785 758 749 726 | 1012 1014 1014 1013 1013 1012 1013 1013
t= 45s  805 774 769 759 770 744 735 715 | 1004 1012 1011 1010 1011 1010 1010 1009
t= 85s  795 755 753 739 747 718 710 703 |  995 1010 1009 1008 1008 1007 1007 1004
```

Contended **700–809**, healthy **995–1014**. Separation ratio **1.35×**, clean gap of ~185 points.
`IVH_CAP_FLOOR = 850` sits inside that gap and would classify **perfectly**. vcap on the same run:
464–503 vs 1015 (2.1×).

### 2.2 run2 — sustained load. This is where the design breaks.

Same measurement, 10 s bins, alongside vcap on the identical samples:

```
        uc_capacity  cpu0-7                     | healthy   ||  vcap cpu0-7          | healthy
t=  0s  777 748 741 745 742 730 718 701 | 1006..1012  ||  425 463 423 513 ... | 1015
t= 30s  890 898 890 888 888 874 859 834 | 1013..1014  ||  363 356 341 377 ... | 1015
t= 60s  913 914 905 898 900 896 867 859 | 1013..1015  ||  351 344 348 356 ... | 1015
t=110s  927 930 921 917 918 916 902 890 | 1013..1015  ||  336 337 337 333 ... | 1015
t=140s  903 908 888 885 895 888 875 864 | 1010..1014  ||  468 503 415 401 ... | 1015
```

**The two signals move in opposite directions under sustained load.** vcap descends 425 → 336
(host giving a smaller share as total demand rises — the correct, provoked reading).
`ivh_uc_capacity` *ascends* 701 → 930, converging on the healthy cluster.

At t = 110 s the contended cluster is **890–930** and the healthy cluster is **1013–1015**.
Separation ratio **1.09×**; gap **83 points**. **`IVH_CAP_FLOOR = 850` is now below the entire
contended population — every one of the 16 CPUs passes the floor and `GATE_CAPACITY_LOW` has gone
silent.** This reproduces the 818–946 figure the G1/G2 validation reported, and identifies the
condition that produces it: *sustained* load, not *any* load.

`uc_acct` (the kcpustat variant, col 7) tracks `uc_wall` within ~40 points and drifts in the same
direction — the ACCT/WALL choice does not escape this.

### 2.3 run3 — the controlled demand experiment (host pressure fixed)

60 s idle → 90 s of eight pinned `while :; do :; done` on cpu0–7 → idle. Nothing about the host
changed. `uc_capacity`, 10 s bins:

```
phase A (guest idle)   t=  0s  823 814 796 784 792 798 773 769 | 1009..1013
                       t= 50s  795 790 767 747 761 763 743 742 | 1007..1011
phase B (cpu0-7 spin)  t= 70s  619 622 611 602 610 610 600 601 | 1005..1008
                       t=110s  514 520 518 518 519 518 518 519 | 1006..1007
                       t=140s  507 513 512 513 513 512 514 513 | 1005..1008
```

Under saturation the contended CPUs settle at **507–520** — i.e. the host really is giving them ~50 %
— and the healthy CPUs are unmoved at 1005–1008 (separation **1.97×**). This is the *honest* number,
and it is the number vcap's spinners are designed to provoke.

### 2.4 The three regimes side by side — the finding in one table

Contended group cpu0–7, host pressure identical throughout:

| guest demand on cpu0–7 | `ivh_uc_capacity` | healthy group | ratio | verdict of `FLOOR = 850` |
|---|---:|---:|---:|---|
| **saturated** (run3 B) | 506–520 | 1005–1008 | 1.97× | correct |
| **idle** (run3 A / run1 tail) | 742–823 | 1004–1013 | 1.28× | correct |
| **bursty, sustained hackbench** (run2) | **890–936** | 1013–1015 | **1.09×** | **wrong — everything passes** |

`ivh_uc_capacity` is **non-monotonic in guest demand**: it is *lowest* at saturation, *highest* at
bursty intermediate demand, and in between when idle.

**Mechanism.** `x_wall = (avail − stolen)/avail` with `avail = elapsed − idle`. A mostly-idle vCPU
that wakes for a short burst is scheduled promptly by the host — it has accumulated fair-share credit
— so the small `avail` it does measure contains very little steal, and the ratio reads ~930. Once it
saturates it burns that credit and the ratio falls to its true 512. **The denominator is guest
demand.** Normalising steal by "wall time this vCPU wanted the CPU" is precisely what removes the
host-pressure information from the numerator, because a lightly-demanding guest is, by construction,
almost always given what it asks for.

This is §1.3 of the retirement plan re-derived from the other end. The plan's WALL formula was built
to be *invariant to whether vcap's spinner is present*. It succeeded at that and, in doing so,
inherited the property the spinner existed to defeat: **without a demand source, "of what I asked
for, what did I get" is close to 1 no matter how loaded the host is.**

### 2.5 What is stable: the ordering, and only the ordering

Per-sample check — is `max(contended) < min(healthy)` in that sample?

| run | samples | ordering correct | mean gap | **min gap** |
|---|---:|---:|---:|---:|
| run1 | 839 | **100.00 %** | 214 | 192 |
| run2 | 1,366 | **100.00 %** | 112 | **77** |
| run3 | 1,012 | **100.00 %** | 378 | 181 |

Population extremes over all 3,217 samples × 8 CPUs per group:

```
contended  min=506  p50=774  p90=910  p99=927  p99.9=934  max=936
healthy    p0.1=995 p1=999   p10=1006 p50=1011            max=1015
```

The two populations do not overlap **once**, in 25,736 CPU-samples per group. But their separating
value sits anywhere in [936, 995] depending on regime, and 936 is only the observed ceiling of this
host's contention level — there is no argument that it is the ceiling.

**The pre-EMA `raw_wall` is not usable and this matters for tuning.** Per-sample ordering on
`raw_wall`: run1 77.5 %, run2 **53.9 %**, run3 98.1 %, with negative gaps down to −45. The 10.4 s
half-life EMA (`ivh_uc_ema_alpha_q16 = 868`) is what converts a per-window sample that is barely
better than a coin flip into a perfectly-ordered statistic. **Do not shorten α to make the signal
"more responsive" — responsiveness is what destroys it.**

---

## 3. Live gate-level confirmation (`ivh_decision_shadow=1`, five sustained hackbench runs)

Deltas from `/proc/ivh_debug` over the run, with the **current** gates modelled (`FLOOR = 850`,
`GATE_NOT_BETTER` at zero margin):

| counter | delta | reading |
|---|---:|---|
| `ivh_uc_pass_both` | 9,708,552 | both signals accept the destination |
| `ivh_uc_pass_uc_only` | **2,328,859** | **uc accepts what vcap rejects — ratio 0.240** |
| `ivh_uc_pass_vcap_only` | 0 | uc never rejects what vcap accepts |
| `ivh_uc_pass_neither` | 6,166,124 | |
| `ivh_destset_empty_uc` | 0 | |
| `ivh_destset_empty_vcap` | 0 | |
| `ivh_destset_empty_tsc` | 942,241 | Part C empties the set — confirms retirement plan §1.4 |
| `ivh_dec_uc_only_go` / `_real_only_go` | 0 / 0 | Gate 1 agreed perfectly *in this regime* |
| `ivh_uc_thr850_both / _uc_only / _neither` | +2,410 / **+1,431** / +803 | **31 % of CPU-window samples disagree at 850, all uc-passes** |
| `ivh_uc_thr1010_vcap_only` | **0 here, +529 in an earlier at-rest batch** | 1010 is *on* the healthy cluster's boundary |

`pass_uc_only / pass_both = 0.240` scrapes under the retirement plan's G2 bar of 0.25 — but it is
**entirely one-directional**, which is the migration-storm signature (`pass_tsc_only = 5.0 M` was the
same fingerprint for Part C, `ivh_tsc_final_state_report_2026-08-02.md` §6.3). A gate that only ever
*adds* destinations relative to the incumbent is not "in agreement"; it is a looser gate.

The `thr1010` row is a second, independent finding: `ivh_capacity_threshold = 1010` **does not
transfer**. Healthy CPUs on the uc scale read 995–1015 — the threshold is inside their distribution,
so which side of it a healthy source lands on is decided by regime and noise. Measured: at
`T = 1010`, **40.5 %** of healthy-CPU samples are misclassified as contended.

---

## 4. Verdict on the load-bearing question

> **Recalibrating `IVH_CAP_FLOOR` to a different constant cannot work, and should not be attempted.**

The evidence, ranked:

1. **The constant that works is regime-dependent, over a 126-point range, with the independent
   variable held constant.** Modelled false-pass rate of the contended group at `FLOOR = 850`:
   run1 **0.00 %**, run3 **0.00 %**, run2 **83.81 %**. Same host, same corunner, same eight vCPUs.
2. **The regime where it fails is the operating regime.** IVH exists to move a spinning lock waiter
   off a stolen vCPU during a sustained contended workload. That is run2.
3. **The failure direction is the dangerous one.** When the contended cluster drifts above the floor,
   `GATE_CAPACITY_LOW` accepts every CPU and `GATE_NOT_BETTER` (zero margin) becomes the only capacity
   gate — a bare relative comparison on a population compressed into 83 points. That is,
   mechanically and exactly, the configuration `ivh_tsc_final_state_report_2026-08-02.md` §6.3 traced
   to the 72.7 K-migration regression.
4. **A floor that works on today's data (`FLOOR ≈ 950–990`, 0 % / 0 % over all 3,217 samples) is a
   knife edge, not a calibration.** It has 59 points of headroom between the observed contended
   maximum (936) and the observed healthy minimum (995). Point 1 says a 126-point drift happens
   routinely. The next heavier host corunner moves the contended cluster through it, and the failure
   is silent — the gate does not error, it just stops rejecting.

**But the signal is not useless, and the correct conclusion is not "different signal entirely".**
`ivh_uc_capacity` carries **perfectly reliable ordinal information** (§2.5: 100 % over 3,217 samples,
three regimes, min gap 77). What it does not carry is a stable origin. Gates that read the ordering
work; gates that read the absolute value do not.

**Why this is not the Part C failure wearing a new hat — the distinction that the whole redesign
rests on.** Report §6.3 concluded that a *relative* comparison degrades to arbitrary under
compression while an *absolute* one degrades gracefully. That conclusion was correct **for Part C's
signal and for the reason it gave**: `ivh_vact_capacity` pinned every CPU at or near 1024 because its
thresholded-excess estimator *could not see the steal at all*. Its compression was a **loss of
ordinal content** — the ranking was noise, so `dest > source` was decided by noise. `ivh_uc_capacity`
is compressed in a different way: its ordinal content is intact and exact, and only its *scale
origin* moves. Those two situations have opposite remedies. For Part C, absolute was safer because
relative was random. For uc, relative is safer because absolute is unanchored. **The test that tells
them apart is §2.5's per-sample ordering rate, and it is 53.9 %-at-worst for uc's raw samples versus
100.00 % for the EMA'd value — which is why the redesign below also fixes α in place.**

---

## 5. The redesign

Three changes. All are BPF-source constants plus one sysctl; **no kernel rebuild**.

### 5.1 `GATE_CAPACITY_LOW` — replace the absolute floor with a population-normalised top-band test

**Concept, not a value.** "Is this destination in the healthiest tier *currently observable*",
not "is this destination above 850".

```c
/* resolved ONCE per scan, before the bpf_loop() over candidates -- same
 * reasoning as the retirement plan sec 6.2's cap_source: a mid-scan change
 * must not produce a half-old, half-new candidate set. */
u32 scan_max = 0;
/* first pass over online CPUs: scan_max = max(ivh_cap_of(rq_i)) */
...
#if GATE_CAPACITY_LOW
    if (cap_of(select_rq) + IVH_CAP_TOPBAND < ctx->scan_max) {
        bump_reason(REJ_CAPACITY_LOW);
        return 0;
    }
#endif
```

**`IVH_CAP_TOPBAND = 50`.** Modelled over all 3,217 samples:

| K | contended false-pass | healthy false-reject |
|---:|---:|---:|
| 25 | 0.00 % | 0.00 % |
| **50** | **0.00 %** | **0.00 %** |
| 75 | 0.00 % | 0.00 % |
| 100 | 7.04 % | 0.00 % |
| 150 | 33.13 % | 0.00 % |

It is scale-free by construction, so the 126-point regime drift of §2.4 moves the *threshold* with the
population instead of leaving it behind. It is also exactly what `IVH_CAP_FLOOR`'s original comment
said it was for — *"require a migration target to be well above the source trigger ceiling"* — with
"well above" measured against the live population rather than a constant from vcap's scale.

**Keep a vestigial absolute rail, deliberately far from the operating point.** Retain a second
absolute test at **`IVH_CAP_HARDFLOOR = 600`**, which rejects catastrophically stolen destinations
(the saturated regime's 506–520 cluster) and is *never* the binding constraint in run1 or run2. It
exists so that a scan in which every CPU is deeply stolen cannot promote one of them to "best tier"
and migrate onto it. It must be documented as a rail, not a calibration — if it ever becomes the
binding gate, the redesign has failed and §8 is the answer.

### 5.2 `GATE_NOT_BETTER` — a fixed absolute margin, not a bare `>`

```c
#if GATE_NOT_BETTER
    if (cap_of(select_rq) < ctx->source_capacity + IVH_CAP_MARGIN) {
        bump_reason(REJ_NOT_BETTER);
        return 0;
    }
#endif
```

**`IVH_CAP_MARGIN = 50`.** Modelled: for every sample, for every contended source, count destinations
accepted, split by whether the destination was contended (lateral, wasteful) or healthy (useful), and
count sources with no destination at all.

Margin alone:

| δ | useful accepts | lateral accepts | lateral % | empty destination set |
|---:|---:|---:|---:|---:|
| **0 (today)** | 205,888 | 86,001 | **29.5 %** | 0.0 % |
| 25 | 205,888 | 29,301 | 12.5 % | 0.0 % |
| 50 | 205,888 | 6,171 | 2.9 % | 0.0 % |
| 75 | 205,888 | 1,023 | 0.5 % | 0.0 % |
| 100 | 189,846 | 0 | 0.0 % | **7.0 %** |
| 150 | 137,007 | 0 | 0.0 % | 33.1 % |

Margin **in conjunction with** the §5.1 top-band gate — this is the proposal:

| D \ K | 25 | 50 | 75 | 100 |
|---|---|---|---|---|
| **25** | 0 lat / 0 empty | 0 / 0 | 0 / 0 | 2,685 lat (1.29 %) / 0 |
| **50** | **0 / 0** | **0 / 0** | **0 / 0** | 117 lat (0.06 %) / 0 |
| **75** | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |

**205,888 useful accepts, 0 lateral, 0 empty destination sets**, at D = 50, K = 50, across all three
regimes. And it is a *plateau*, not a point: every cell in D ∈ [25,75] × K ∈ [25,75] is perfect. The
margin-only column shows why the conjunction matters — margin alone has a viable window of only
δ ∈ [75, 85] (below it, laterals; at 100, empty sets), which *is* a knife edge. Pairing it with the
normalised gate widens the safe box by 3× in each dimension.

**Why this does not reduce to §6.3's near-arbitrary failure.** Three independent reasons, in
decreasing order of strength:

1. **The premise of that failure is measured absent.** §6.3's mechanism requires that the ranking be
   noise. Here the ranking is exact: 100.00 % correct group ordering over 3,217 samples with a
   worst-case gap of 77 (§2.5). A margin of 50 is **below the worst observed true gap (77)** and
   **above the worst observed intra-contended-group spread that a lateral would need** — the D=50/K=50
   cell produced literally zero laterals in 205,888 evaluations.
2. **It is not a *bare* relative gate.** §6.3's subject was `dest > source` with no margin — a
   comparison that a single LSB of noise flips. Both proposed gates have a margin wider than the
   signal's per-sample dispersion, so no single-sample perturbation changes a verdict.
3. **The uniform-contention case that `GATE_NOT_BETTER` was written for is handled *better* by the
   margin than by the floor.** If every CPU is equally stolen, no destination clears `src + 50`, the
   destination set empties, and IVH correctly does nothing. Under the current absolute floor the same
   situation is handled only by accident of where the constant happens to sit.

The residual risk is stated plainly in §7.1 and given its own gate in §6.

### 5.3 `ivh_capacity_threshold` (Gate 1, kernel side) — **yes, it must change: 1010 → 965**

Gate 1 is `if (ivh_gate_capacity(rq, cap_src) > ivh_capacity_threshold) reject` — a *source*-side
"this CPU is healthy, don't bother" test. On the uc scale the healthy population is 995–1015, so 1010
bisects it. Measured over all 3,217 samples:

| T | contended wrongly called healthy | healthy wrongly called contended |
|---:|---:|---:|
| 940 – 990 | 0.00 % | **0.00 %** |
| 1000 | 0.00 % | 1.28 % |
| **1010 (today)** | 0.00 % | **40.54 %** |

Recommend **965** — the midpoint of the observed valid band [936 contended max, 995 healthy min], and
the centre of a 50-point-wide plateau on both sides.

**And note the structural point, because it changes how much this value matters.** Gate 1 is an
absolute threshold on a signal §4 just argued has no stable origin, so it inherits the same drift
risk — but *once §5.1/§5.2 are in place, both of its failure directions are self-limiting*:

- contended cluster drifts **above** 965 → Gate 1 stops firing → IVH goes quiet → degrades to
  baseline. Safe.
- healthy cluster drifts **below** 965 → Gate 1 fires on healthy sources → the BPF scan runs, but a
  near-`scan_max` source cannot find a destination at `src + 50` → empty set → no migration. Wasted
  CPU, not a wrong decision.

That asymmetry is a direct consequence of making the BPF side relative, and it is the reason this
plan does not also need to make Gate 1 population-normalised. **It is not a reason to leave it at
1010** — 40.5 % spurious source-side triggering is a real cost in scan work.

### 5.4 What must NOT change

- **`ivh_uc_ema_alpha_q16` stays at 868.** §2.5: the pre-EMA sample orders the groups correctly only
  53.9 % of the time under sustained load. The long half-life is load-bearing, not conservatism.
- **`ivh_uc_used_source` stays at 0 (WALL).** §2.2: `uc_acct` drifts in the same direction and by a
  similar amount. The ACCT variant offers no escape and carries the spinner-dependence risk of
  retirement-plan §8 risk 1.
- **`ivh_uc_window_ns`, `ivh_uc_duty_ns`, `ivh_uc_min_avail_pct` stay at defaults.** Nothing measured
  here implicates them, and changing more than one thing at a time is what §6 exists to prevent.

---

## 6. Validation plan for the builder

Scope: **one BPF recompile + reload, plus sysctl and map writes.** No kernel build, no reboot.
Prerequisite: retirement-plan **Build B** (the `ivh_cfg` map and `ivh_cap_of()` helper) must be in
place first — it is not, as of this document; `MY_ivh_atc.bpf.c` still reads `select_rq->cpu_capacity`
directly at `:342, :424, :434, :658`. Do Build B **before** any gate-shape change, verify it is a
no-op at `ivh_cfg[0] = 0`, and only then apply §5.

Every stage is pass/fail on a number. No judgement calls.

**Standing rules for every stage below:**
- Workload: `/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000`, **run ten times back to back**
  per round. A single 12 s run is *not* sufficient — §2.1 vs §2.2 shows a single run measures the
  transient, not the operating point. This is the single most important procedural instruction here.
- Before every round, confirm host contention: `ivh_ref_steal_ns` delta over 10 s must show ≥ 1 %
  of wall on the contended group and ≤ 0.1 % on the healthy group at guest idle. If not, the host
  corunner is gone and **nothing measured is interpretable** — stop.
- `ivh_universal_eligible = 1` for every measured round (`/home/nick/IVH` does **not** set it; this is
  a known gap).
- `pkill MY_ivh_atc` before every reload. One `vcap`, one `MY_ivh_atc`, verified by `pgrep -c`.
- Save `MY_ivh_atc.bpf.o` and the binary as `.known-good` before the first edit.

---

**V0 — baseline, no new code consumed. PREREQUISITE.**
`ivh_cap_source=0`, `ivh_cfg[0]=0`, `ivh_uc_shadow=1`, `ivh_decision_shadow=0`. Three rounds.
- **PASS:** `ivh_migrations_done` per round in **48–53 K** (historical healthy band); throughput at
  current baseline; `ivh_uc_windows` advancing on all 16 CPUs.
- **FAIL:** baseline has moved → stop, nothing downstream is interpretable.
- **Record:** per-round migrations and hackbench wall time. These are the comparands for V3/V4.

**V1 — reproduce §2.4 on the builder's own boot. GO/NO-GO on the whole plan.**
Sample `ivh_uc_cpu:` at 10 Hz through one V0 round (10 back-to-back hackbench runs, ~150 s).
Compute, per sample, `max(contended group)` and `min(healthy group)` — using the *host*-derived
grouping from the standing steal check, not from `ivh_uc_capacity` itself.
- **PASS:** per-sample ordering correct ≥ **99 %** of samples, **and** worst-case gap ≥ **50**.
- **PASS:** contended-group max over the round ≥ **870** (i.e. the drift of §2.2 is present and
  `IVH_CAP_FLOOR = 850` is confirmed inside the contended population on this boot).
- **FAIL (ordering < 99 % or gap < 50):** stop. The premise of §5.2 reason 1 does not hold on this
  boot and a relative gate is **not** safe. Do not proceed; the answer is §8.
- **FAIL (contended max < 870):** the drift is absent on this boot — this document's problem
  statement has not reproduced. Do not "fix" it by proceeding; find out why (host corunner intensity
  is the first suspect) before changing any gate.

**V2 — offline re-derivation of D and K from V1's own capture. No code change.**
Re-run §5.1/§5.2's models on the V1 sample file. Sweep D ∈ {0,25,50,75,100}, K ∈ {25,50,75,100,150}.
- **PASS:** at least one cell with lateral-accept **= 0 %** and empty-set **= 0 %**, and that cell has
  **all four orthogonal neighbours also at 0 %/0 %** (i.e. it is a plateau, not a point).
- **Choose D and K as the centre of the largest such plateau.** If that centre differs from (50, 50)
  by more than one grid step, use the measured value and say so in the commit message.
- **FAIL:** no 0 %/0 % plateau exists → stop, go to §8. Do not ship a cell with nonzero laterals
  "because it is better than today's 29.5 %".

**V3 — BPF flip, capacity source still vcap. Isolates gate SHAPE from gate INPUT.**
Apply §5.1 + §5.2 to `MY_ivh_atc.bpf.c` with the chosen D, K, plus `IVH_CAP_HARDFLOOR = 600`.
Recompile, reload. **Leave `ivh_cfg[0] = 0` — the gates still read `rq->cpu_capacity` (vcap).**
Three rounds.
- **PASS:** migrations/round ≤ **56 K** (baseline + 8 %) and ≥ **43 K** (baseline − 18 %); throughput
  ≥ V0 baseline − 3 %.
- **IMMEDIATE REVERT** (restore `.known-good` .o, `pkill`, relaunch) if migrations/round > **61 K**,
  without waiting for the round to finish. Migration count moved 1.4× *before* throughput moved in the
  Part C regression; it is the leading indicator.
- **FAIL low (< 43 K):** the margin is too wide on vcap's much more separated scale. Reduce D by one
  grid step, repeat once. If it fails again, stop — the two gates cannot be shape-changed and
  input-changed independently, and that is worth knowing before V4.
- **Why this stage exists:** vcap's population (368–503 vs 1015) has a 500-point gap, so D = 50 and
  K = 50 should be *inert* against it. Any behaviour change here is a bug in the new gate code, not a
  property of the new signal, and finding it now costs one reload instead of confounding V4.

**V4 — the real flip: capacity input → `ivh_uc_capacity`, both sides.**
`bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0` **and**
`echo 3 > /proc/sys/kernel/ivh_cap_source` **and** `echo 965 > /proc/sys/kernel/ivh_capacity_threshold`.
`ivh_uc_shadow=1`, `ivh_decision_shadow=1` for the first round only (it is O(nr_cpus) per evaluation).
Three rounds.
- **PASS (canary, checked continuously):** migrations/round ≤ **56 K**. **Immediate revert at
  > 61 K** — rollback is `ivh_cfg[0]=0` + `ivh_cap_source=0` + `ivh_capacity_threshold=1010`, all
  three, in that order.
- **PASS:** throughput ≥ V0 baseline − 3 %.
- **PASS:** `ivh_destset_empty_uc` ≤ **1.2 ×** `ivh_destset_empty_vcap` over the round. (Both were 0
  in §3; any nonzero value here is the margin gate biting and must be understood.)
- **PASS:** `reject_reasons[REJ_NOT_BETTER] / (REJ_NOT_BETTER + ACC_TIER1_ACTIVE + ACC_TIER2_IDLE)`
  ≤ **0.60**. If the margin gate is rejecting more than 60 % of candidates that reach it, D is too
  wide for this boot regardless of what V2 modelled.
- **PASS:** `ivh_steal_imminent_capacity_reject` rate within **±30 %** of its V0 rate — this is the
  direct test of the 1010 → 965 change.
- **FAIL any:** revert all three, return to V2 with the shadow data.

**V5 — regime robustness. The stage that this whole document exists to force.**
With V4's configuration live and passing, re-run V4's PASS checks under **two additional demand
regimes**, because §2.4 is the finding:
1. **Saturated:** eight pinned busy-loops on the contended CPUs, concurrent with hackbench.
2. **Idle-ish:** a single hackbench run followed by 60 s rest, repeated three times.
- **PASS:** migrations/round and throughput bounds hold in **all three** regimes; `destset_empty_uc`
  stays 0 in all three.
- **FAIL:** revert. A configuration that passes only in the regime it was tuned in is exactly the
  failure this document diagnosed, reproduced one level up.

**V6 — only then, consider retiring vcap** (retirement plan G6). Not in scope here, and it must not
be attempted until V5 passes, because vcap is the only independent comparator for §3's counters.

---

## 7. Risks, ranked

1. **[HIGHEST] The ordering guarantee is measured on one host contention pattern.** All 3,217 samples
   come from a single corunner configuration that produces a clean 8/8 split. A host load that
   contends *unevenly* across the guest's vCPUs, or that contends all 16, has not been observed. The
   margin gate degrades safely under uniform contention (empty set → no migration), but a *partially
   ordered* population — say four deeply stolen, four mildly stolen, eight clean — is untested, and
   `D = 50` would permit mild→clean **and** deep→mild. Mitigation: V5's saturated regime is a partial
   probe; a genuinely uneven corunner is the missing experiment and should be added if one can be
   arranged on the host.
2. **`scan_max` is computed from a live, unsynchronised read of 16 rqs.** Two evaluations microseconds
   apart can compute different `scan_max` and reach different verdicts for the same candidate. The
   retirement plan's §6.2 discipline — resolve **once per scan**, stash in `task_ctx` — is mandatory
   and is why §5.1 specifies it. It also costs a second pass over the CPU list inside the BPF program;
   the verifier and instruction-count impact must be checked at V3, not assumed.
3. **`IVH_CAP_HARDFLOOR = 600` could become binding.** If the host's contention ever exceeds ~40 %
   sustained on *all* vCPUs, the whole population drops below 600 and IVH goes silent. That is the
   correct behaviour, but it must be *observable*: `reject_reasons[REJ_CAPACITY_LOW]` going to ~100 %
   is the signature, and V4's check on it should be read as a canary, not just a bound.
4. **`ivh_capacity_threshold = 965` is still an absolute threshold on an unanchored signal** (§5.3).
   Self-limiting in both directions once §5.1/§5.2 ship, but it will need revisiting if the contended
   cluster's ceiling is ever observed above ~940.
5. **Build B has not been done.** Everything here assumes the `ivh_cfg` runtime kill switch exists.
   Applying §5 on top of the current unconditional `select_rq->cpu_capacity` reads would recreate the
   "revert did not revert" hour of `ivh_tsc_final_state_report_2026-08-02.md` §6.4.

---

## 8. Option B, and it is cheap: the ratio `ivh_uc_tick()` already computes and discards

§2.4's mechanism is entirely the **denominator**. `ivh_uc_tick()` computes, on every tick:

```c
d_elapsed_c = now - rq->ivh_uc_prev_tsc;            /* wall  */
d_steal_c   = ivh_tsc_ns_to_cycles(steal delta);    /* steal */
avail_c     = d_elapsed_c - d_idle_c;
d_steal_c   = min(d_steal_c, avail_c);              /* <-- information destroyed here */
```

It then accumulates `avail_c` and the clamped `d_steal_c`, and publishes `used/avail`. The unclamped
`d_steal_c / d_elapsed_c` — **steal as a fraction of wall time** — is never accumulated, and it is
the demand-invariant quantity.

### 8.1 Measured, same host, same three regimes

`Δ ivh_ref_steal_ns / Δ wall`, as a percentage:

| regime | contended cpu0–7 | healthy cpu8–15 | **ratio** | (`ivh_uc_capacity` ratio for comparison) |
|---|---|---|---:|---:|
| guest idle | 1.09 – 1.56 % | 0.02 – 0.04 % | **~50×** | 1.28× |
| sustained hackbench | 1.65 – 2.66 % | 0.33 – 0.65 % | **~5×** | **1.09×** |
| all-16-CPU spin | 46.8 – 49.8 % | 0.36 – 0.39 % | **~130×** | 1.97× |

It is **monotone increasing in host pressure in every regime**, and it never inverts. A single
absolute threshold at **1.0 %** classifies all 48 CPU-regime observations correctly (contended
minimum 1.09 %, healthy maximum 0.65 %). That is not a wide margin at the operating point — 2.5× —
and it should not be oversold: **this signal too is best used relatively.** But unlike
`ivh_uc_capacity` it at least *admits* a constant, and its dynamic range is 5×–130× rather than
1.09×–1.97×.

### 8.2 What it would cost

One `u64 ivh_uc_win_elapsed_c` and one `u64 ivh_uc_win_steal_raw_c` in `struct rq`; two `+=` in
`ivh_uc_tick()` **using values already in registers**; one more ratio and one more EMA in
`ivh_uc_close()`; one more published `unsigned long ivh_uc_pressure`; `ivh_uc_used_source` gains
value 2. Roughly 25 lines, entirely inside code the retirement plan already wrote. It *does* require
a kernel rebuild, which is why it is Option B rather than the proposal — but it is a far smaller
change than the retirement plan's Build A was, and it is strictly additive (all existing fields and
knobs keep their meaning).

**Publish it on a permille or log scale, not as a 1024-scale capacity.** Linearly, `1024·(1 − steal)`
crushes the whole idle and hackbench regimes into [997, 1024] — a 10-point separation on a 1024 scale,
which is a knife edge even though the underlying ratio is 50×. The information is in the *ratio*, so
the representation must preserve ratios.

### 8.3 The honest framing of the whole exercise

vcap's counterfactual semantics were not an accident of implementation to be engineered away — they
are a response to a real property of the problem. **Host pressure is only observable through the
guest's own attempts to run**, so a passive measurement of "what fraction of what I asked for did I
get" converges to 1 for any guest that is not asking for much, no matter how loaded the host is. The
retirement plan's WALL formula successfully made the signal invariant to vcap's spinner, and by doing
so it also made it invariant to the thing the spinner was there to reveal.

What survives passively is (a) **ordering** — which vCPUs are worse than which — measured here at
100.00 % reliability, and (b) **steal per unit wall time**, which is a rate rather than a ratio and so
does not divide out the demand. Both support relative gating. Neither supports the absolute floor
that vcap's provoked, demand-normalised scale made possible. **`IVH_CAP_FLOOR` as a concept — a fixed
constant on the capacity scale — is a vcap-shaped artefact and does not survive vcap's retirement.**
The redesign in §5 is what replaces it; §8 is what to build if §5's V1 or V2 gate fails.

---

## 9. Appendix — reproducing the measurements

Nothing here needs a build. All read-only except the three instrumentation sysctls, all of which were
restored to 0.

```sh
# 1. verify host contention BEFORE anything else
grep '^ivh_ref_cpu:' /proc/ivh_debug | awk '{print $2,$3}'  # snapshot, sleep 10, snapshot, delta/1e10 = %

# 2. instrumentation on (restore to 0 afterwards)
echo 1 | sudo tee /proc/sys/kernel/ivh_universal_eligible
echo 1 | sudo tee /proc/sys/kernel/ivh_uc_shadow

# 3. sample at 10 Hz while the workload runs
#    ivh_uc_cpu: <cpu> <vcap_custom> <cpu_capacity> <uc_capacity> <uc_wall> <uc_acct>
#                <raw_wall> <raw_acct> <win_avail_c> <win_stolen_c> <windows> <extended>
#                <skipped> <vact_capacity>
while :; do date +%s.%N | tr '\n' ' '; grep '^ivh_uc_cpu:' /proc/ivh_debug; sleep 0.1; done

# 4. the workload -- TEN back to back, not one (sec 2.1 vs sec 2.2)
for i in $(seq 10); do /home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000; done

# 5. the controlled demand experiment (sec 2.3)
for c in 0 1 2 3 4 5 6 7; do taskset -c $c timeout 90 bash -c 'while :; do :; done' & done

# 6. restore
echo 0 | sudo tee /proc/sys/kernel/ivh_universal_eligible
echo 0 | sudo tee /proc/sys/kernel/ivh_uc_shadow
echo 0 | sudo tee /proc/sys/kernel/ivh_decision_shadow
pgrep -c vcap; pgrep -c MY_ivh_atc   # must both be 1
```
