# NHextend4: how low can IVH's critical-section floor go?

**Kernel** `6.17.0-rseqport73+`, branch `kernel-43-clean`, `nohz=off`.
**Harness** `NHextend4.c` (new; fresh copy of `NHextend3.c` + env knobs, defaults byte-identical
in behaviour to NH3). **Ground truth** `/proc/vcap_info` host steal via `vsched_module.ko`,
verified populated (64 lines) before every single run by the sweep scripts.
**NHextend3.c / NHextend3 / vcap_probe / vcap_probe.cpp / IVH / ivh_verify.sh: NOT modified.**

---

## Terminal state reached: BOTH, and I am splitting them at a specific CS length

This did not land cleanly on one of the two acceptable outcomes. It landed on both, and the
boundary between them is sharp and measurable, so I am stating it that way rather than picking
the flattering half.

**1. The floor was lowered, by about 2x, and it reproduced.** Shipped-default IVH's floor is
between **0.99 ms and 1.49 ms** of critical section. A tuned live-sysctl configuration moves it
to between **0.48 ms and 0.73 ms**. At `loop_spin=400000` (CS ≈ 0.99 ms) the shipped default is
**+0.8 %** — i.e. break-even — and the tuned config is **+9.8 %**, the latter reproduced in
**five independent batches, 18 rounds, every round positive**. At `loop_spin=300000` (CS ≈
0.73 ms) tuned is **+5.6 %** across two clean batches (7 rounds); a third batch dissents at
−1.1 % and was contaminated (below).

> **Correction (post-review).** The first draft of this document reported the 400 k default arm
> as **−0.9 %, a regression, from a single unreplicated batch**, while giving the tuned arm three
> batches. That asymmetry was a real methodological failure — this project's reproduce-before-
> trusting rule applies to both arms of a comparison, not only to the one carrying the
> interesting result — and the orchestrator caught it. Two further independent batches (n=4 each)
> were run. **The −0.9 % regression does not hold up.** Pooled over three 10 s batches (n=11) the
> default arm is **+0.78 %**, with per-batch means of −0.94 %, −0.22 %, +1.07 %. The correct
> statement is that shipped-default IVH at CS ≈ 1 ms is **statistically indistinguishable from
> IVH-off**, not that it regresses. The floor conclusion survives unchanged, because the bar is
> ≥ 5 % and break-even does not clear it — but the claim that the default actively hurts at 400 k
> is withdrawn. Full record in sections 7 and 8.

**2. Below CS ≈ 50 µs the floor is genuinely, structurally stuck, and it is not an artifact.**
Not "we tried two things". The reason is arithmetic on ground-truth host-steal data: the total
damage IVH could possibly prevent collapses to **≤ 1–3 %** below 50 µs. A ≥ 5 % win is therefore
unreachable there by *any* mechanism, including a hypothetical zero-cost perfect one. Section 4.

**The single-digit-microsecond / nanosecond target is not reachable on this host.** But the
reason is not "CS is too short" in the abstract — it is a *ratio*, and section 6 shows the floor
moving by 15 percentage points when host contention doubles. That is the part I think is
genuinely worth the user's attention, because it means the floor is a property of the
host-contention regime, not a constant of the mechanism.

Both user hypotheses (H1 vcap interference, H2 inter-iteration sleep) were tested and **both are
falsified**. Neither lowers the floor; both raise it. The lever that actually worked was a third
thing found by instrumenting the loss.

---

## 1. Relocating the prior ~ms measurement

Found, and it matters that the shape is stated correctly, because the task framing has it inverted.

- `tools/bpf/docs/ivh_six_goals_report_2026-07-22.md` — the CS sweep table (goal 1).
- `tools/bpf/docs/ivh_state_of_the_art_2026-07-20.md:303` — the earlier crossover table.

Those record IVH **winning at long CS and losing below it**: +8.46 % at `loop_spin=600000`
(~1.6 ms), then −57.6 % / −68.4 % / −49.3 % at 100 k / 50 k / 25 k, recovering to ~+2.3 % at
10 k / 5 k. The task brief describes the opposite ("above roughly 1 ms IVH stops helping and
starts hurting"). The *goal* as stated — lower the CS floor — is unambiguous either way, and
"floor" here means **the shortest CS at which IVH still wins**. That is what I measured.

The July 20 doc also already contains the `ivh_time_left_threshold_ns` lever (−54 % → −15 % at
100 k) and its plateau. I reproduced that lever on this kernel and then got past its plateau with
two further knobs. Credit where due: I did not find that lever, I found what to stack on it.

## 2. Baseline: the floor reproduces on this kernel

The July data is from a different kernel; re-measured here first. n=3 per point, arms
**interleaved** off/on within each round (so slow host drift cancels rather than aliasing onto
one arm), 10 s runs, 5 s gap.

| `loop_spin` | CS (off, µs) | iters off → on | Δ | HP % (off) | mig/iter (on) | mig avg |
|---|---|---|---|---|---|---|
| 600,000 | 1487 | 6697 → 7872 | **+17.5 %** | 11.07 % | 11 % | 760 µs |
| 100,000 | 231 | 42957 → 21537 | −49.9 % | 1.47 % | 25 % | 891 µs |
| 50,000 | 103 | 92203 → 36574 | −60.3 % | 0.35 % | 32 % | 858 µs |
| 25,000 | 49 | 201104 → 99975 | −50.3 % | 0.018 % | 30 % | 540 µs |
| 10,000 | 23.6 | 373012 → 363750 | −2.5 % | 0.0036 % | 6.4 % | 105 µs |
| 5,000 | 15.2 | 432360 → 418718 | −3.2 % | 0.0029 % | 4.0 % | 53 µs |

The floor is real and it is on this kernel, not stale July data.

## 3. Two structural properties of the harness, found by reading it

Both were found by reading `NHextend3.c` before running anything, and both bear on the question.

**(a) The inter-iteration sleep does not scale with `loop_spin`.** `run_thread()`:
`do_sleep(100 + tdata->cpu * 27)` — a fixed **100–505 µs** `nanosleep` after every release. The
offered load on the lock is `D = N·CS/(CS+sleep)`, so shrinking the CS while holding the sleep
fixed *also silently de-contends the lock*. This is H2's target and it is quantitative:
D ≈ 13.5× capacity at 600 k, ≈ 0.66× at 5 k.

**(b) `read_vcap_steal()` runs inside the lock hold.** `grab_lock()` calls it (a `pread` of
`/proc/vcap_info` plus a `strtok`/`sscanf` parse of 4·ncpu lines plus a 2 KB stack memset) after
acquiring the lock and before the CS body — a fixed cost in the *serialized* region that floors
the true CS length no matter how small `loop_spin` gets. `NHextend4` adds `NH4_VCAP_EVERY=N` to
sample every Nth CS with a corrected denominator. This turned out not to be the binding
constraint, but it is a real measurement defect in the harness and it is now switchable.

## 4. Why it is structurally stuck below ~50 µs — the decisive table

`HP %` is the fraction of critical sections during which the **host actually preempted the lock
holder**, from `/proc/vcap_info` — real host steal, not a guest-side proxy. It is the entire pool
of damage IVH exists to recover. "Max addressable benefit" is the measured CS inflation
`(CS_off − CS_best_on)/CS_best_on` — i.e. what a *perfect, zero-cost* IVH could win.

| `loop_spin` | CS off (µs) | **HP % (off)** | cleanest CS achieved (µs) | **max addressable benefit** |
|---|---|---|---|---|
| 600,000 | 1487 | 11.07 % | 988 | ~50 % |
| 400,000 | 959 | 7.0 % | 658 | ~46 % |
| 300,000 | 735 | 5.4 % | 499 | ~47 % |
| 200,000 | 489 | 3.7 % | 333 | ~47 % |
| 100,000 | 231 | 1.47 % | 169 | ~36 % |
| 50,000 | 103 | 0.35 % | 88.6 | ~16 % |
| **25,000** | **49** | **0.018 %** | 48.6 | **~1.2 %** |
| 10,000 | 23.6 | 0.0036 % | 23.3 | ~1.2 % |
| 5,000 | 15.2 | 0.0029 % | 14.8 | ~2.8 % |

> **Correction (post-review).** The first draft's "max addressable benefit" column used the
> *tuned* config's CS as the best achievable, which understated the ceiling at 200 k–400 k (27 %
> instead of 46 %). The **shipped-default** config actually achieves the cleanest CS at every
> length — that is exactly what section 7.1 shows it doing — so the column now uses the cleanest
> CS any arm achieved. **This does not change the conclusion and makes the cliff sharper:** the
> drop is 16 % → 1.2 % between 50 k and 25 k either way.

**The cliff is between 50 k and 25 k.** HP % falls 20× (0.35 % → 0.018 %) and the addressable
benefit falls from ~16 % to ~1 %. Below CS ≈ 50 µs, *there is essentially no host-preemption
damage left to recover*. This is the honest answer to "why is the floor at ms level": the
probability that a ~ms-scale host preemption lands inside a critical section scales with the
length of that critical section, so the benefit pool shrinks with CS while the cost does not.

**The cost side, root-caused in source.** `bpf_sched_pre_lock_migrate()`
(`kernel/sched/fair.c:13551`) performs `set_cpus_allowed_ptr(current, cpumask_of(target_cpu))`,
which for a running task goes `affine_move_task()` → `wait_for_completion()` — a stopper-thread
round trip **plus the destination CPU's scheduling latency** — then a *second*
`set_cpus_allowed_ptr()` to restore the saved mask. Measured: **890–1220 µs with 30–45 % over
1 ms** at shipped defaults; **76–237 µs** even in the best tuned configuration found. Nothing
that moves a task between runqueues gets near the ~µs scale.

So: benefit → 0 as CS → 0; cost floors at 10² µs for scheduler-structural reasons. Break-even
lands where they cross, and that is the measured ~0.5–0.7 ms. To serve a 5 µs CS you would need
migration to cost ~10² **nanoseconds**, which `set_cpus_allowed_ptr` cannot do by construction.

## 5. H1 and H2, both tested, both falsified

**H2 — inter-iteration sleep.** Tested two ways, n=3, Δ vs IVH-off:

| | 600 k | 100 k | 50 k | 25 k | 10 k |
|---|---|---|---|---|---|
| baseline (100+cpu·27 µs) | +17.5 % | −49.9 % | −60.3 % | −50.3 % | −2.5 % |
| `NH4_SLEEP_US=0` (no sleep) | +25.7 % | −31.6 % | (noisy) | −61.2 % | −70.1 % |
| `NH4_SLEEP_DUTY_PCT=29` (constant duty) | +17.5 % | −40 % | −73.7 % | −76.4 % | −74.5 % |

Both variants make short CS **worse**, and the reason is consistent across all three protocols:
contending harder removes the **idle destination capacity that migration needs**. The original
100–505 µs sleep is what creates the ~23 % idle headroom IVH migrates *into*. Remove it and all
16 vCPUs are saturated — migrations then either block ~600 µs or fail in 400 ns. The sleep is not
a confound to be removed; it is a precondition for the mechanism working at all. (The duty-scaled
variant additionally hits `nanosleep` granularity below ~50 µs, which I note as a second-order
limit on that arm rather than the main effect.)

**H1 — vcap_probe interference.** The premise is backwards, and the effect is large. `vcap_probe`
pins one `SCHED_IDLE` thread per vCPU spinning full-tilt for `probe_ms` every `sleep_ms`
(`-p 200 -s 5000` = 3.85 % duty on all 16 vCPUs). Stopping it entirely:

| `loop_spin` | IVH-off iters: probe ON → OFF → ON (control) |
|---|---|
| 400,000 | 10057 → **5895** → 10071 (−41 %, fully reversible) |
| 100,000 | 43818 → **17490** → 43931 (−60 %, fully reversible) |

The prober is **load-bearing for throughput**, not a competitor. Ground truth explains it: HP %
*doubles* with the prober off (100 k: 1.42 % → 2.98 %). Its periodic spin holds the physical CPUs
against the co-runner VM; without it the vCPUs `HLT` and the host takes the cores back. The
control re-run matches the pre-kill numbers to within 0.3 %, so this is the prober and not drift.

Probe *duty* was then swept over a 50× range using a **copy** (`vcap_probe_experimental`, built
from a copied source; the working binary was never modified) at `loop_spin=100000`:

| probe config | duty | IVH-off iters | Δ (tuned IVH) |
|---|---|---|---|
| `-p 20 -s 5000` | 0.4 % | 44852 | −15.8 % |
| `-p 200 -s 5000` (shipped) | 3.85 % | 43931 | −14.2 % |
| `-p 1000 -s 5000` | 20 % | 43761 | −12.2 % |

Flat. It is the *existence* of periodic keepalive that matters, not its magnitude — even a 0.4 %
duty fully restores throughput. **H1 falsified in both variants.** Reducing or eliminating the
prober's active polling does not move the CS floor.

**This is worth flagging beyond this task: every NHextend measurement in this project is
conditional on `vcap_probe` running.** It is not a neutral instrument. A run with the prober
accidentally dead reads 40–60 % slow in *both* arms.

## 6. The result I think actually matters: the floor tracks host contention, not CS length

Section 5's H1 experiment doubles for a controlled perturbation of HP % at *fixed* CS length.
At `loop_spin=100000` (CS ≈ 0.23 ms), tuned IVH:

| condition | HP % (off) | Δ vs IVH-off |
|---|---|---|
| prober running | 1.42 % | **−13.6 %** |
| prober stopped | 2.98 % | **+1.6 %** |

**Doubling the host-preemption rate moved the same CS length by ~15 percentage points, from a
clear loss to break-even.** Same kernel, same harness, same CS, same knobs. Combined with the
section 4 table (benefit ∝ HP %, cost ~constant), the floor condition is:

> IVH wins when `HP% × damage_per_event > migration_rate × migration_cost`.

`damage_per_event` (~host timeslice) and `migration_cost` (~10² µs) are both CS-independent.
Only `HP %` scales with CS. So the floor is **not a constant of the mechanism** — it is set by
the host-contention regime, and on a more heavily contended host it sits at a shorter CS. That
is why the answer to "is the ms floor real?" is *yes on this host tonight*, and why it is not a
universal law. It is also the most likely reconciliation with hackbench/dbench/ebizzy at
single-digit-µs CS, though I did not measure those here and am not going to assert it — see
open questions.

## 7. The tuned configuration, and its honest cost

All live sysctls. No rebuild, no reboot.

```
kernel.ivh_time_left_threshold_ns = 200000     # was 4000000 (sized for a 1.6 ms CS)
kernel.ivh_eval_cooldown_ns       = 1000000    # was 50000
kernel.ivh_max_concurrent         = 4          # was 8
```

| `loop_spin` | CS off | **default IVH** | **tuned IVH** | batches (def / tuned) |
|---|---|---|---|---|
| 600,000 | 1.49 ms | **+16.5 % / +17.5 %** | +11.3 % | 2 / 1 |
| 400,000 | 0.99 ms | **+0.8 %** (pooled n=11) | **+9.8 %** (pooled n=18) | **3 / 5** |
| 300,000 | 0.73 ms | −10.1 % | **+6.1 %, +5.2 %** (−1.1 % contaminated) | 1 / 2 (+1) |
| 200,000 | 0.48 ms | −29.2 % | −4.9 % | 1 / 1 |
| 100,000 | 0.23 ms | −49.7 % | −13.6 % | 2 / 2 |
| 50,000 | 0.10 ms | −60.3 % | −10.6 % (thr+cooldown only) | 1 / 1 |

### 7.1 The 400 k point, fully replicated after review

Every batch below is interleaved OFF/default/tuned within each round. "10 s"/"5 s" is
`NHEXTEND_DURATION`.

| batch | n | OFF | default | Δ | tuned | Δ |
|---|---|---|---|---|---|---|
| `repro400000` (10 s) | 3 | 10033 | 9939 | −0.94 % | 11090 | +10.53 % |
| `defrepA` (10 s) | 4 | 10264 | 10241 | −0.22 % | 11242 | +9.54 % |
| `defrepB` (10 s) | 4 | 10398 | 10509 | **+1.07 %** | 11308 | +8.75 % |
| `rep2_400000` (10 s) | 4 | 10057 | — | — | 11126 | +10.63 % |
| `h1on_400000` (10 s) | 3 | 10071 | — | — | 11062 | +9.84 % |
| **pooled 10 s** | **18/11/18** | **10177** | **10256** | **+0.78 %** | **11176** | **+9.81 %** |
| `dur5` (5 s) | 4 | 5178 | 5289 | +2.13 % | 5704 | +10.15 % |
| orchestrator (5 s) | 3 | 5231 | 5362 | +2.50 % | 5568 | +6.4 % |

**The default arm is break-even, not a regression.** Batch means span −0.94 % to +2.50 % and
straddle zero; the pooled 10 s estimate is +0.78 %. The original −0.9 % was the low end of
ordinary batch-to-batch spread, reported as a finding because it was never replicated.

**The orchestrator's absolute counts are explained.** `NHEXTEND_DURATION` defaults to **5 s**;
my batches used 10 s. My own 5 s batch reproduces the orchestrator's magnitudes almost exactly
(OFF 5178 vs 5231) and its default-arm delta (+2.13 % vs +2.50 %). There is a *hint* that the
default arm scores slightly better at 5 s than at 10 s (both 5 s batches exceed all three 10 s
batches), but that rests on one 5 s batch of mine plus one of the orchestrator's, and I am
flagging it as an observation to check rather than a claim.

**Tuned is robust.** Five independent batches, 18 rounds, every round positive, per-batch
+8.75 % to +10.63 %. The orchestrator's +6.4 % is the lowest reading but the same sign and still
clears the 5 % bar; the tuned-over-default margin is +3.8 % (theirs) to +9.8 % (mine).

### 7.2 The HP % inversion — why tuned wins *despite* worse protection

The orchestrator flagged that tuned has higher throughput **and** higher host-preempted-cycles
than default's near-zero, and asked whether tuned's gain therefore comes from something other
than the mechanism. **It does, and this is the most important correction in this document.**

Per-batch, 400 k, `iters × cs_active / wall` = the fraction of wall time the single contended
lock is actually **held by somebody**:

| batch | arm | HP % | CS active | migrations | mig avg | stuck >1 ms | **lock utilisation** |
|---|---|---|---|---|---|---|---|
| `defrepA` | OFF | 7.04 % | 969 µs | 0 | — | — | **99.5 %** |
| | default | **0.027 %** | **658 µs** | 1066 | 1142 µs | 43.5 % | **67.4 %** |
| | tuned | 3.95 % | 780 µs | 352 | 176 µs | 6.0 % | **87.7 %** |
| `defrepB` | OFF | 6.89 % | 959 µs | 0 | — | — | **99.7 %** |
| | default | **0.019 %** | **658 µs** | 976 | 996 µs | 38.4 % | **69.2 %** |
| | tuned | 3.69 % | 774 µs | 336 | 176 µs | 6.7 % | **87.6 %** |

**HP % measures how well the mechanism protects a critical section, not whether protecting it was
worth the price.** On its own terms the shipped default is not failing — it is working
*better than tuned*: it drives host preemption of the holder to 0.02–0.03 % (from 6.9–7.0 %) and
produces the cleanest critical section of any arm, 658 µs against 959–969 µs off, a 32 %
reduction. That is precisely the mechanism doing its job.

It pays for that by putting ~1000 threads-worth of `wait_for_completion()` into a 10 s run at
~1.0–1.1 ms each — **0.97–1.22 s of blocked thread time, with 38–46 % of migrations exceeding
1 ms** — and because the workload serializes on one lock, a thread blocked in migration is a
thread not taking the lock. The lock therefore sits **idle a third of the time** (67–69 %
utilisation) under the default, against 99.5 % when IVH is off entirely. Tuned accepts 3.7–4.0 %
HP (worse protection, ~20× fewer migrations at ~6× lower cost each, 0.06 s blocked) and keeps the
lock **87.6–87.9 %** utilised. Throughput is the net of those two, and the net favours tuned.

**So the tuned config's gain is not better LHP mitigation — it is a cost reduction. It wins by
doing less of the mechanism, not by doing it better.** That is a genuine correction to how
section 7's headline framed it, and it cuts against my own result rather than for it.

Two things follow, and they matter more than the tuning itself:

1. **It is not simply "less IVH is better".** OFF = 10177, default = 10256, tuned = 11176 — the
   optimum is interior, not at either end. Partial application beats both full application and
   none, so the mechanism does deliver real net value at CS ≈ 1 ms; it just has to be rationed.
2. **It strengthens, not weakens, section 4.** The binding constraint is confirmed to be
   *migration cost*, not detection quality — the detector is already near-perfect at this CS
   (HP → 0.02 %). Improving prediction cannot help; only making migration cheaper can, and
   section 4 argues from the source why that has a ~10² µs floor.

**The tuned config is not a strict improvement and I am not going to present it as one.** At
600 k it gives +11.3 % where the default gives +16.5–17.5 % — it trades peak long-CS gain for
mid-CS viability. It is a different operating point, not a free win. Anyone shipping it should
know they are giving up roughly a third of the best-case long-CS benefit.

Contributions of each knob, isolated at `loop_spin=50000` (n=2, interleaved):
`thr` 4 ms → 400 µs → 100 µs → 50 µs → 25 µs gives −60.3 % → −26.1 % → −16.0 % → −13.2 % → −13.0 %
(plateaus, reproducing July 20). Adding `eval_cooldown=1 ms` on top: −10.6 %. Adding
`max_concurrent=4` was the knob that carried 300 k over the line.

## 8. What I did not establish, and where I was wrong

- **I under-replicated the arm that disagreed with my thesis.** The 400 k default arm got one
  batch; the tuned arm got three. I reported the resulting −0.9 % as a regression. It is not one
  — pooled n=11 gives +0.78 %, i.e. break-even (section 7.1). The failure was not the number, it
  was applying the reproduce-before-trusting rule asymmetrically: I replicated the result I found
  interesting and took the control on a single reading. The "1 / 3" batch column in the original
  table made this visible without my flagging it, which is worse, not better.
- **I mis-framed why the tuned config wins.** I presented it as IVH working better at shorter CS.
  It is IVH working *less*: the default achieves strictly better protection (HP 0.02 % vs 3.9 %,
  cleanest CS of any arm) and loses on throughput anyway because migration blocking idles the
  lock a third of the time (section 7.2).
- **Section 4's benefit ceiling was computed against the wrong reference** (tuned CS rather than
  the cleanest CS achieved), understating the 200 k–400 k ceiling by roughly half. Corrected in
  place; the conclusion below 50 µs is unaffected.
- **Another actor is mutating IVH sysctls on this machine.** On returning to re-test I found
  `time_left_threshold=200000, eval_cooldown=1000000, max_concurrent=4` live — the tuned values,
  which I had restored to defaults — evidently left by the orchestrator's own verification batch.
  `ksweep.sh` resets every relevant sysctl before each individual run, so the measurements here
  are insulated, but any result on this box taken *without* an explicit reset is suspect.

- **The 300 k `+6.09 %` did not reproduce on first attempt.** A second batch gave −1.1 % on its
  clean rounds. That batch was contaminated — its round 2 caught a host-contention episode
  (IVH-off collapsed to 7939 with HP 10.3 % against 13 800 / 5.3 % in its siblings) and its
  default arm went bimodal. A third batch (n=4, clean) gave +5.19 %. So 300 k is **+5–6 % in two
  clean batches with one contaminated batch dissenting** — I am reporting it as real but weaker
  than 400 k, not folding it into the headline.
- **Host contention drifted materially over the session** and one whole batch (`top600k`) had to
  be discarded and re-run: IVH-off at 600 k read 3884 iters / HP 20.5 % during the episode versus
  6811 / 10.7 % after recovery. Recovery was verified by re-measuring a known point before
  continuing. Late-session numbers are not directly comparable to the early baseline, which is
  why every comparison in this document is *within* an interleaved batch.
- **A `pkill -f "vcap_probe"` matched my own shell's command line and killed it**, orphaning the
  script, which then ran an uncontrolled sweep and restarted a second prober. Those files
  (`run_h1noprobe_*`) are **discarded**; `h1off_*` is the controlled replacement. Two leaked
  `vcap_probe_experimental` instances were found and killed (my `pgrep -x` could not match them —
  `/proc/comm` truncates to 15 chars). The `-p 1000` arm in section 5 ran with the `-p 20` copy
  still alive and is confounded on that axis; its conclusion (flat) is unaffected since it agrees
  with both neighbours.
- **`ksweep.sh` writes unquoted commas into its CSV**, so the CSVs mis-parse for multi-knob
  combos. All numbers here were re-derived from the raw `run_*.txt` files.
- **Sub-50 µs was not re-tested with the tuned config.** Section 4 says the addressable benefit
  there is ≤ 1–3 %, so I judged the runs not worth the wall time; that is an inference from the
  benefit table, not a measurement of tuned-at-25k.

## 9. Open question I could not close

The user's premise that **hackbench / dbench / ebizzy run at single-digit-µs CS and improve under
IVH** is in direct tension with section 4, which says the addressable damage at that CS is ≤ 1–3 %.
I did not measure those workloads in this session and will not hand-wave it. Two candidate
resolutions, both testable and neither established here: (a) their wins are not per-CS LHP
mitigation on one contended lock — NHextend's single global lock with 16 spinners makes every
migration cost the *whole lock's* critical path, a convoy penalty those workloads do not have;
(b) their effective HP % is far higher than NHextend's at comparable CS, which by section 6 would
move their floor down. Resolving this is the highest-value next experiment, because if (a) is
right then NHextend is simply the wrong instrument for asking where IVH's floor is.

## 10. Files

- `NHextend4.c` (new, in tree) — `NH4_SLEEP_US`, `NH4_SLEEP_STAGGER_US`, `NH4_SLEEP_DUTY_PCT`,
  `NH4_VCAP_EVERY`; all defaults reproduce NH3 exactly (verified: same iters/CS/migrations/HP %).
  Binary `/home/nick/NHextend4_exp`.
- `/home/nick/vsched_main/vcapacity/vcap_probe_experimental{,.cpp}` — copies, for H1 only.
- Scratchpad: `sweep.sh`, `tsweep.sh`, `ksweep.sh`, `res_*.csv`, `run_*.txt`.

**State left as found:** exactly one `vcap_probe -p 200 -s 5000`, one `MY_ivh_atc`,
`/proc/vcap_info` populated, all IVH sysctls back at shipped defaults
(`time_left_threshold=4000000`, `eval_cooldown=50000`, `max_concurrent=8`,
`universal_eligible=1`). Nothing committed or pushed.
