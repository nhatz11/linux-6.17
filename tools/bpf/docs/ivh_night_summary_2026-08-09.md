# IVH: TSC-only steal/capacity pipeline — full night summary, 2026-08-09

Orchestrator-written synthesis of everything validated tonight, across the reboot to
`nohz=off`, the idle-subtraction fix, the phase_pct recalibration, and the NHextend4
CS-floor investigation. Every headline number below has been independently
re-verified by the orchestrating conversation, not just relayed from the agent
reports that found them — per the user's explicit standing instruction this session.

## 0. The goal, and where it stands

Remove UNHALTED.REF (PMU) and the real steal page (`kvm_steal_time`) from IVH's
capacity/migration decision path entirely, targeting confidential-computing
environments (TDX/SEV-SNP) where neither is available or trustworthy — while
matching the performance of the PV/PMU-dependent baseline this project started
with.

**Status: substantially achieved, live, committed, and independently re-verified —
with one real, load-bearing accuracy caveat below (section 5) that should be read
before trusting any "ground truth" percentage NHextend3/4 print on their own.**

## 1. The chain of fixes, in order

1. **`ivh_steal_source=2`** — a TSC-only tick-gap steal estimator
   (`ivh_tick_steal_accumulate()`, `core.c`), no REF_TSC, no `paravirt_steal_clock()`.
2. **`ivh_uc_used_source=0` (WALL)** — avoids the PV-tainted `kcpustat`
   USER/SYSTEM accounting path that the ACCT variant (`=1`) silently depends on.
3. **`nohz=off`** (kernel boot parameter, persistent in `/etc/default/grub`) +
   **`ivh_idle_ns()` fallback** (`kcpustat[CPUTIME_IDLE]+[CPUTIME_IOWAIT]` when
   `!tick_nohz_active`) — root-caused and fixed a structural bug where busy time
   fragmented into sub-millisecond bursts (hackbench's actual shape) could miss
   timer-tick boundaries under tickless idle, making the estimator unable to tell
   "genuinely busy but un-ticked" from real host steal.
4. **`ivh_tks_idle_sub=0`** — stops subtracting the vCPU's own idle time as false
   carry debt. Only valid because of (3); the kernel default stays `1` for exactly
   that reason (would reinstate an unbounded phantom under a tickless boot).
5. **`ivh_tks_phase_pct=100`** (not 50) — corrects a per-preemption-event
   undercount. This kernel's tick is an absolute periodic hrtimer, so a preemption
   burst costs one whole skipped tick, not the half-tick the original in-tree
   derivation assumed. Measured deficit: exactly 1.000 tick per event, invariant
   across a 5.6x range of true steal, on continuously-runnable and idle load.
6. **`ivh_selection_trylock=1`** — found under an unpinned corunner: a blocking
   destination-scan lock pays full scan cost even when no destination is
   meaningfully better, actively regressing IVH below no-opt (26.1s → 31.3s).
   Non-blocking scan fixed it (25.7s, back in line with no-opt), first tested
   together with the TSC-only pipeline this session.

All six are live and shipped in `/home/nick/IVH`, with real measured numbers in
the script's own comments.

## 2. Headline accuracy numbers (post-recalibration, independently re-derived from raw
`rq->ivh_tks_steal_ns` vs the raw `steal_time.steal` page — NOT via NHextend's own
printed column, see section 5)

| regime | pre-recalibration ratio | post-recalibration ratio |
|---|---:|---:|
| continuously runnable | 0.655 | **0.9999** |
| idle guest | 0.559 | **1.006** (sd 0.009, 7 blocks) |
| hackbench | 0.232 | **1.143** (a known, accepted overshoot — see below) |

Idle/clean-vCPU floor, characterized on independent ground truth: clean vCPUs
receive real steal in 8–14µs *events*, ~250x smaller than contended vCPUs'
2.5–3.4ms events, ~100x below the 50µs noise deadband. The estimator correctly
reads ~0 there — this is a genuine detection floor (sub-tick preemptions are
structurally invisible to any tick-gap technique), not a calibration failure, and
it accounts for 0.008–0.014% of wall time on those CPUs, i.e. it isn't costing
anything real to leave unrecovered.

**The hackbench overshoot (1.143, not 1.000) is real and was investigated, not
ignored:** hackbench's specific workload shape produces a measured per-event
deficit of 0.83 tick, not the 1.00 tick that holds on the other two regimes. An
interior `phase_pct≈87` would minimax the error across all three regimes to ±4.6%
— computed, and deliberately **not** shipped, because it would be a host-fitted
constant rather than a principled derivation, and would make the other two regimes
worse to fix hackbench specifically. `phase_pct=100` stays the shipped value.

## 3. Performance validation — reproduced, statistically checked, no regression

**hackbench**, pinned corunner, 2 batches / 12 interleaved rounds:
OFF 15.855s → shipped(pre-recalib) 11.370s → applied(post-recalib) 11.383s.
Paired difference +0.013s, SE 0.037, **p≈0.73 — no detectable change.** Migrations
69,379 → 69,448.

**NHextend3** at `loop_spin=400000` (CS≈0.99ms), 3 batches / 11 clean rounds, both
the default-sysctl and NHextend4-tuned-sysctl arms checked:
- default: −3.06% → −1.96% (paired +1.15pp, not significant)
- NHextend4-tuned: +7.72% → +8.00% (paired +0.27pp, not significant)

The phase_pct/idle_sub recalibration measurably improved raw accuracy (section 2)
without moving either established performance number outside noise.

**Unpinned-corunner comparison** (from earlier this session, still the standing
reference — not re-run tonight post-recalibration, flagged as a good follow-up
check): real PV adaptive spinning 25.15s, TSC adaptive spinning 25.46s (within
1.2% of each other), full IVH with `trylock=1` 23.80s — beating both.

## 4. The NHextend4 CS-floor investigation

Original question: prior testing found IVH stops helping (and can hurt) above
~1ms critical-section length, while hackbench/dbench/ebizzy (all single-digit-µs
CS) reliably improve — an apparent contradiction worth root-causing rather than
shrugging off.

**Both of the user's hypotheses were tested and falsified:**
- **H2 (inter-iteration sleep)**: removing it makes things *worse* (a 10k-loop_spin
  test went from −2.5% to −70.1%). The sleep is what creates the idle destination
  capacity migration needs — a precondition, not a confound.
- **H1 (vcap_probe interference)**: backwards. Stopping the prober *cut* throughput
  41–60% in both tested arms, fully reversible, control-verified. It's load-bearing
  (holds physical cores against the co-runner VM), not a competitor. Probe duty was
  separately swept 50x via a copy (`vcap_probe_experimental` — original never
  touched) and found flat.

**The real floor is a ratio, not a constant**, and it moved:

| `loop_spin` | CS off | default IVH | tuned IVH |
|---|---|---:|---:|
| 600,000 | 1.49ms | +16.5/+17.5% | +11.3% |
| 400,000 | 0.99ms | break-even (+0.78% pooled, 11 rounds) | **+9.81% pooled, 5 batches/18 rounds** |
| 300,000 | 0.73ms | −10.1% | +5.6% (2 clean batches) |
| 200,000 | 0.48ms | −29.2% | −4.9% |

Tuned config (live sysctls, no rebuild): `ivh_time_left_threshold_ns=200000`,
`ivh_eval_cooldown_ns=1000000`, `ivh_max_concurrent=4`. **Not shipped as the new
default** — it trades peak long-CS gain for mid-CS viability (600k: +11.3% vs
default's +16.5–17.5%), a different operating point, not a strict improvement.
Left available as a documented, tested option.

**An important correction happened mid-investigation, worth recording as
methodology, not just result:** the original draft reported "default actively
regresses at 400,000 (−0.9%)" from a single, unreplicated batch. Independent
re-verification (both by the orchestrator and by the agent re-testing itself)
found this doesn't hold — default is break-even there, not a regression. The
original number was the low end of ordinary run-to-run spread, reported as a
finding only because it was never reproduced. This is exactly the standing rule
this project has needed to restate multiple times tonight: reproduce **both**
arms of a comparison, not just the one that confirms something interesting.

**Below CS≈50µs, the floor is genuinely, structurally stuck — arithmetic, not a
failed search.** Ground-truth host-preempted-cycles data: at CS=49µs, only
~1.0% of total wall time is even theoretically addressable by *any* migration
mechanism, including a hypothetical zero-cost perfect one (vs ~16% at CS=103µs —
a 16x cliff between 50µs and 25µs `loop_spin`). A ≥5% win is mathematically
unreachable there.

**A genuinely valuable reframing came out of the correction pass**: "lock
utilization" (iterations × CS-active-time / wall-time) resolves an apparent
paradox — the *default* config has better critical-section protection quality
(lowest real host-preempted fraction of any arm) but *lower* throughput, because
it pays for that protection with `wait_for_completion()` blocking
(~1s of every 10s run blocked). The tuned config wins by doing **less** of the
migration mechanism, not by doing it better — a real, non-obvious result about
where the cost actually lives (`bpf_sched_pre_lock_migrate()` →
`set_cpus_allowed_ptr()` → `affine_move_task()` → `wait_for_completion()`,
76–237µs even fully tuned).

**Open, explicitly flagged, not resolved tonight:** hackbench/dbench/ebizzy all
improve under IVH at single-digit-µs CS, apparently contradicting the table
above. Not measured this session — the leading hypothesis is that NHextend's
single-global-lock structure makes every migration cost the whole lock's
critical path in a way hackbench's lock structure doesn't, meaning NHextend may
be the wrong instrument for characterizing the true floor. Flagged as the
highest-value next experiment, not investigated further tonight.

## 5. The ground-truth labeling issue — read this before trusting any NHextend
"Host-preempted CS cycles... ground truth" percentage on its own

Found by the phase_pct recalibration agent, verified directly by the orchestrator
against source (`core.c:443`, `get_steal_and_preemptions()`):

```c
switch (READ_ONCE(ivh_steal_source)) {
case 2:
        *steals_time = READ_ONCE(rq->ivh_tks_steal_ns);   /* the TSC estimator */
        return;
...
```

`/proc/vcap_info` — and therefore NHextend3/4's own printed "ground truth" column
— is fed by this function, which **switches on `ivh_steal_source`**. At the value
used essentially all night (`2`), that column is reading the TSC estimator's own
output, not independent host telemetry. The "ground truth" label in NHextend3.c's
source predates this project moving `ivh_steal_source` off `0` and is now
misleading.

**What this does and doesn't affect:**
- Affected: every "Host-preempted CS cycles (ground truth)" number printed by
  NHextend3/4 tonight, including the orchestrator's own NHextend3 verification
  earlier this session (the "+18.3% throughput, ~85x reduction" comparison) and
  the original NHextend4 report's HP%-based analysis. These numbers are real —
  they reflect what the estimator itself reported — but they are not independent
  confirmation the way they were described.
- Not affected: every direct bpftrace comparison against the raw `steal_time.steal`
  page (`kaddr("steal_time")` + `__per_cpu_offset`, bypassing
  `get_steal_and_preemptions()` entirely) done throughout tonight, including the
  tick-deficit derivation and the section-2 accuracy table above. Those read
  hardware/hypervisor-provided data directly.
- Not fatal to tonight's conclusions: the phase_pct recalibration's Phase B
  validation was designed around this (checked `ivh_uc_capacity` directly, not
  HP%), and the NHextend4 investigation's resolving metric for its central
  finding (the "lock utilization" reframing) is independent of this issue.
  The qualitative direction of every finding tonight is very likely still
  correct; some of the specific magnitudes quoted via NHextend's own built-in
  metric should be treated as "what the estimator reported," not "verified
  against an independent host source."

## 6. State — verified directly, not just relayed

- Kernel: `6.17.0-rseqport73+`, `nohz=off` (GRUB, persistent).
- **Committed and pushed**: `c0451f85a` on `kernel-43-clean`
  (`git log`/`git status` confirmed clean at time of writing this doc; this doc
  and one other file created after that commit are new/untracked, not yet
  committed — see below).
- `/home/nick/IVH`: reflects all 6 fixes in section 1, comments carry real
  measured numbers, `bash -n` clean.
- Daemons: exactly one `MY_ivh_atc`, one `vcap_probe -p 200 -s 5000`. Original
  vcap backup untouched at `/home/nick/vsched_main/vcapacity_ORIGINAL_BACKUP_2026-08-08/`.
- `/proc/vcap_info` live (module rebuilt for this kernel via
  `install_module.sh` after an initial cache miss).
- `NHextend3.c`/`NHextend3` binary: untouched all night, md5-verified repeatedly.
- `NHextend4.c`: a pre-existing file from earlier (pre-session) project history,
  cleaned of stale unrelated experimental code and rebuilt as a genuine
  NHextend3-based copy for the CS-floor investigation.
- **Not committed**: this summary doc, and
  `tools/bpf/docs/ivh_phase_pct_recalibration_2026-08-09.md` /
  `ivh_nhextend4_cs_floor_2026-08-09.md` (the two agent reports from tonight's
  final round) — all written after the `c0451f85a` push. Recommend a final
  commit+push once you've reviewed this doc.

## 7. Recommended next steps, priority order

1. Commit + push tonight's final two reports and this summary.
2. Re-run the unpinned-corunner 3-arm comparison (section 3) post-recalibration —
   it was validated before `idle_sub`/`phase_pct` were corrected and is worth
   a fresh number, though nothing suggests it would move.
3. Investigate the open NHextend-vs-hackbench CS-floor contradiction (section 4) —
   likely needs an NHextend variant with a less-global lock structure, or direct
   dbench/ebizzy floor characterization using the same methodology.
4. Consider whether `/proc/vcap_info`'s "ground truth" label/behavior (section 5)
   is worth fixing at the source level — either restore a genuinely
   `ivh_steal_source`-independent host-truth path for it, or update NHextend3.c's
   own printed label so it stops claiming ground truth it can no longer provide
   once `ivh_steal_source` moves off 0.
