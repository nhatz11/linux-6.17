# IVH: does HP% reduction predict throughput outcome? Paired analysis, 2026-07-20

Kernel `6.17.0-rseqport54+`, branch `kernel-43-clean`. Companion to
`ivh_state_of_the_art_2026-07-20.md` (mechanism, sysctls, methodology) and
`ivh_benchmark_search_2026-07-20.md` (the winners/losers whose throughput
deltas this analysis pairs against real host-preempted-CS numbers). Read those
first.

## What this measures and how

The benchmark-search report has throughput deltas but almost no paired,
per-workload host-preempted-CS (HP%) numbers. The question here: **is HP%
reduction tightly coupled to whether a workload's throughput improves, or are
they decoupled?** (Established once already for PARSEC dedup: ~96% relative HP%
reduction *and* a net throughput loss.)

For each benchmark/config I ran, under the standard test sysctls (§2.3:
capacity=1010, time_left_threshold=4ms, max_concurrent=8, time_left_source=1,
selection_trylock=0, migrate_mechanism=0, hot_threads=0), with
`vcap -p 200 -s 5000` + `MY_ivh_atc` running:

- **`ivh_universal_eligible` held at 1 for the whole system in *both* arms.**
- **Baseline ("no-opt")**: `ivh_exec -v -n <cmd>` — the workload is
  `ivh_exclude`'d, so IVH is off *for it* while the rest of the system is on.
- **Protected**: `ivh_exec -v <cmd>` — same command, workload IVH-on.

This is a deliberate **single-variable paired design**: the *only* thing that
changes between the two arms is whether *this workload's own* lock acquisitions
are IVH-protected. Everything else (system-wide IVH traffic, vcap steal
injection) is held constant. Both arms report the workload's own real
host-preempted kernel-lock holds via `/proc/ivh_debug`'s
`ivh_obs_total_holds`/`ivh_obs_stolen_holds` deltas — which are only fed for
tasks with `ivh_observe` set (i.e. only the wrapped workload), so with one
measured workload at a time the counters are effectively scoped to it (verified
against the source: `kernel/locking/spinlock.c:334`, increment gated on
`current->ivh_observe`). One workload measured at a time throughout.

**This regime differs from the benchmark-search report's**, which toggled
`ivh_universal_eligible` 0↔1 for the *entire system* (no-opt = nothing on).
The difference matters and is itself a finding — see the tmpfs/dedup/vips note
below. Throughput deltas in the table are therefore reported **twice**: my own
paired delta (self-consistent with the HP numbers, since both come from the
*same* runs) and the report's whole-system-toggle delta.

Rounds: 3 per config (2 for memtier; 6 for swaptions after the first 3 were
load-contaminated). HP absolute stolen-hold counts are small for low-lock
workloads, so I pool stolen/total across rounds for the percentages and report
per-run absolute stolen counts alongside so the noise is visible.

## Summary table

Throughput Δ sign convention: **positive = better** (faster wall time, or higher
records/s · MB/s · ops/s). "stolen/run" = mean absolute host-preempted lock
holds per run (base → protected). HP% = pooled stolen/total.

| benchmark | config | no-opt HP% | prot HP% | HP rel. reduction | stolen/run base→prot | my paired tput Δ | report tput Δ | class (report) |
|---|---|---|---|---|---|---|---|---|
| ebizzy | mmap `-S20 -t16 -m` | 0.0001% | ~0.0000% | **97.6%** | 85 → 3 | **+61.8%** | +136% | **WIN** |
| dbench | fsync `-F` 16 clients | 0.0002% | ~0.0000% | **94.9%** | 30 → 2 | **+12.1%** | +33.7% | **WIN** |
| dbench | fsync `-F` 8 clients | 0.0000% | 0.0000% | **100%** | 5 → 0 | **+18.1%** | +20.2% | **WIN** |
| hackbench | `-g4 -l30000` | 0.0040% | 0.0020% | **49.2%** | 6541 → 3210 | **+15.5%** | +28.6% | **WIN** |
| swaptions | `-ns256 -sm500000 -nt16` | 0.0016% | 0.0000% | (n/a, ~0 stolen) | 0.3 → 0 | −2.0% (neutral) | +8.4% | WIN |
| ebizzy | malloc `-S20 -t16` | 0.0095% | ~0.0000% | **99.8%** | 155 → 0.3 | **−5.4%** | −2% | **LOSS** |
| dbench | tmpfs 16 clients | 0.0014% | ~0.0000% | **99.6%** | 4037 → 20 | **+22.5%** (⚠ see note) | −19.3% | LOSS(report) |
| hackbench | `-g20 -l8000` | 0.0028% | 0.0031% | **−8.8%** (worse) | 6211 → 6582 | **−16.9%** | −11.7% | **LOSS** |
| dedup | native, 16 threads | 0.0018% | 0.0001% | **97.2%** | 213 → 8 | +6.5% (noisy) | +16–33% slower | LOSS(report) |
| vips | native, 16 threads | 0.0039% | 0.0002% | **95.2%** | 36 → 2 | +1.7% (neutral) | +33–39% slower | LOSS(report) |
| memtier | redis, `-t16 -c10 -30s` | 0.0001% | ~0.0000% | **88.4%** | 30 → 3 | **−7.3%** | −5% | **LOSS** |
| pbzip2 | `-p8` compressible | 0.0001% | ~0.0000% | (n/a, ~1 stolen) | 1 → 0 | −1.3% (neutral) | ~0% (noisy) | LOSS |

Two absolute-magnitude facts to keep in view for everything below:
1. **No-opt HP% is tiny everywhere** — 0.0001%–0.0095%. The 100 µs steal floor
   means only a long hold that straddles a steal window is counted, so even the
   most LHP-exposed workload has <0.01% of its holds stolen. The interesting
   variable is never the *size* of HP%, it's the *leverage* of those rare
   stolen holds.
2. **Protected HP% collapses to ~0 almost everywhere** the workload has any
   lock exposure at all — winners and losers alike.

## Q1 — Do winners show a larger HP% reduction than losers?

**No. HP% reduction is large and roughly uniform across both groups wherever the
workload touches kernel locks at all. The win/loss split is set by collateral
cost and lock leverage, not by how much protection is achieved.**

Relative HP% reduction, winners vs losers:

- **Winners**: ebizzy mmap 97.6%, dbench fsync8 100%, dbench fsync16 94.9%,
  hackbench g4 49.2%.
- **Losers**: ebizzy malloc **99.8%**, memtier 88.4%, dedup 97.2%, vips 95.2%,
  dbench tmpfs 99.6%.

The losers' reductions are, if anything, *higher* on average than the winners'.
ebizzy malloc — a throughput **loss** — has the single largest relative HP%
reduction in the whole set (99.8%) and one of the largest *absolute* reductions
(≈155 stolen holds/run eliminated), larger than the flagship winner ebizzy mmap
(≈82/run). IVH is doing its narrow job — killing steal-during-hold — equally
well in the losers.

The **one real exception is hackbench g20**, the only case where protection
actually *fails* (reduction −8.8%: stolen holds *rise* under protection). That
is not collateral cost, it's the mechanism breaking down under 50× CPU
oversubscription — there is no un-stealable CPU to migrate onto, and each forced
migration is itself another chance to be caught holding across a steal. (More in
Q2.) hackbench g4's reduction is also only 49%, notably below the 90–100% seen
elsewhere, because a large share of hackbench's stolen holds are rq-locks /
socket locks held in scheduler-internal contexts IVH structurally cannot
migrate out of — yet g4 still *wins* on half-protection, because the holds it
*can* protect are high-leverage.

## Q2 — Do the within-benchmark flip pairs' HP stories explain the flip?

Three flip pairs, three different answers — and that spread is the point.

**ebizzy mmap (+62%) vs malloc (−5%): the flip is PURELY collateral cost, not
differential protection.** Both sides get near-total protection (97.6% vs
99.8%); malloc's is larger in both relative and absolute terms. Same mechanism,
same steal-during-hold elimination, opposite throughput sign. The difference is
**leverage**: mmap-mode stolen holds are `mmap_lock` held across page faults —
one stolen hold stalls all 16 threads, so preventing ≈82/run is worth +62%.
malloc-mode "holds" are short kernel locks around userspace memcpy with almost
no cross-thread leverage; preventing ≈155/run buys nothing, and the migration
collateral (cache-locality disruption on pure-userspace work) makes it a net
−5%. This is the cleanest possible illustration that **HP% reduction and
throughput are decoupled**: the *more*-protected side is the loser.

**hackbench g4 (+15%) vs g20 (−17%): the HP story DOES explain this flip.**
Unlike ebizzy, here protection effectiveness itself flips: g4 gets 49%
reduction (protection working), g20 gets −8.8% (protection failing under 50×
oversubscription — 800 tasks on 16 CPUs, no safe migration target). So
hackbench's flip is protection-failure **plus** maximal migration collateral,
qualitatively different from ebizzy's pure-collateral flip.

**dbench fsync (win) vs tmpfs (⚠): in my paired design the flip DISAPPEARS.**
Both win in my single-variable measurement (fsync16 +12%, fsync8 +18%, tmpfs16
**+22%**), both with ~95–100% HP reduction. tmpfs even has the largest absolute
protection of the whole set (4037 → 20 stolen/run). This **does not reproduce
the report's tmpfs −19% loss** — see the regime note below. What it does show:
protecting dbench's *own* VFS/inode lock holds is beneficial on tmpfs too; the
report's loss was not the workload's own protection cost.

## Q3 — Any loser with a genuinely large, meaningful absolute HP% reduction?

**Yes — several. This is the clearest collateral-cost illustration in the set.**

- **ebizzy malloc** (net **−5.4%** throughput): **99.8%** relative HP%
  reduction, baseline HP% 0.0095% (the highest of any lock-exposed workload
  here) driven to ~0.0000%; **≈155 real host-preempted lock holds eliminated
  per run**. Full, real protection; net loss.
- **memtier/redis** (net **−7.3%**): 88.4% reduction, ≈27 stolen holds/run
  prevented on the client threads. Protection is real but aimed at the wrong
  place — the bottleneck is the single-threaded redis *server*, which has no
  multi-thread LHP to fix, so protecting the *client* threads only adds
  migration cost.
- **dedup** (report **−16 to −33%**): 97.2% reduction, ≈205 stolen holds/run
  eliminated — the original decoupling case, now confirmed with paired numbers
  and generalized.
- **vips** (report **−33 to −39%**): 95.2% reduction, ≈34 stolen holds/run
  eliminated.

So the dedup pattern is **not** an outlier: a loser routinely gets
near-complete steal-during-hold protection. ebizzy malloc is the sharpest
single data point (largest baseline HP%, ~total reduction, still a loss).

## Q4 — Any winner whose HP% reduction is negligible (win comes from elsewhere)?

**Yes — swaptions, and it should not be filed under "IVH wins by preventing
LHP."**

swaptions has **negligible kernel-lock exposure**: 15k–98k total holds per run,
3–4 orders of magnitude below the lock-heavy workloads (ebizzy mmap 58M,
dbench tmpfs 292M, hackbench 160M+), and **0–1 stolen holds per run**. Its HP%
"reduction" (0.3 → 0 stolen) is statistically meaningless — there is essentially
no lock-holding to protect. Whatever the report's +8.4% is, it **cannot** be
LHP mitigation.

In my clean paired rounds (4–6; rounds 1–3 were contaminated by residual load
from a concurrently-shut-down process, wall times 33–37s vs the clean ~22.6s)
swaptions is **neutral: base 22.6s vs protected 23.0s, −2%, within noise.** So I
do not even reproduce the win under the single-variable design. Any real
swaptions effect is a scheduling / steal-distribution / rebalancing artifact,
not steal-during-hold avoidance. pbzip2 is the same story (≈1 stolen hold/run,
near-zero exposure, neutral) and its compressible input runs in ~4.5s, below the
reliable-measurement floor the report already flagged.

## The tmpfs / dedup / vips regime discrepancy (secondary finding)

My paired single-variable design systematically shows **less throughput
regression** than the report's whole-system `ivh_universal_eligible` toggle for
the high-throughput short-lock workloads:

| workload | report (whole-system toggle) | mine (paired, workload-only) |
|---|---|---|
| dbench tmpfs 16 | **−19.3%** | **+22.5%** |
| dedup native | −16 to −33% | +6.5% (noisy, ≈neutral) |
| vips native | −33 to −39% | +1.7% (neutral) |

Interpretation: my design isolates the effect of protecting **the workload's
own** locks, holding system-wide IVH traffic constant in both arms. The report's
toggle bundles that together with **system-wide migration collateral** (every
other non-excluded process on the box migrating when universal flips on). For
these high-throughput workloads — where a single fast core does a lot of
low-leverage lock work — protecting the workload itself is neutral-to-positive,
and the report's loss is dominated by the *system-wide* collateral, not the
workload's own protection cost. This is consistent with, and sharpens, the
collateral-cost model: the cost that turns these into losers lives in the
migration volume of the *whole system*, not in the target workload's own holds.
(Caveat: my dbench "tmpfs" used an explicit `/dev/shm` working dir and `-t20`;
the report used dbench's default no-`-D`, no-`-F` config. Some of the gap may be
config, not just regime — but the sign reversal is too large and too consistent
across 3 tight rounds, 9700 vs 11900 MB/s, to be only that.)

## Verdict

**HP% reduction does NOT predict throughput outcome. They are genuinely
decoupled variables.**

IVH reduces steal-during-hold by **~90–100% almost everywhere the workload holds
kernel locks at all — in winners and losers alike** (the sole failure is
hackbench g20 under 50× oversubscription). The narrow mechanism works
essentially uniformly. Yet the same set spans +62% to −17% throughput. The
loser with the *largest* protection (ebizzy malloc, 99.8%, ~155 holds/run
saved) loses; the flagship winner (ebizzy mmap, 97.6%, ~82 holds/run saved)
wins. Protection magnitude and throughput sign move independently.

**What actually predicts the outcome** (tying back to the collateral-cost model,
state-of-the-art §3.3) is the sign of:

&nbsp;&nbsp;&nbsp;&nbsp;**(leverage of the protected holds) − (migration collateral)**

not the magnitude of HP% reduction. Concretely:

1. **Leverage of the stolen holds.** A stolen `mmap_lock` (held across page
   faults) or a disk-blocking VFS lock stalls *all* peer threads → one prevented
   steal is worth a lot (ebizzy mmap +62%, dbench fsync +12–18%). A stolen
   userspace-memcpy lock or a client-side lock behind a single-threaded server
   bottleneck has near-zero cross-thread leverage → preventing it buys nothing
   (ebizzy malloc, memtier). Same mechanism, same HP% reduction, opposite value.
2. **Migration collateral vs available slack.** Each migration drains lock
   throughput (state-of-the-art §3.3: ~2.5–5.5 CS-equivalents per migration).
   When there is CPU slack and the protected holds are high-leverage, the trade
   pays (winners). Under heavy oversubscription the collateral swamps the
   benefit *and* the mechanism stops working (hackbench g20: −8.8% protection,
   −17% throughput — the worst of both).
3. **A workload can also "win" with no LHP protection at all** (swaptions:
   negligible lock exposure, neutral-to-positive from scheduling effects) — so
   not every reported win is even an LHP win.

The one thing HP% reduction *does* reliably tell you: **IVH is mechanically
doing what it claims** — steal-during-hold is really being prevented, in every
lock-exposed workload, not just the ones that happen to profit. The dedup
decoupling generalizes cleanly: **real, near-total protection is the norm; net
benefit is the exception, gated entirely by leverage and collateral.**

## Data provenance

Raw per-round CSV and per-run stdout captures:
`/tmp/claude-1000/-home-nick-kernels-linux-6-17-rseqport/4ab2961e-3163-49c1-b957-e10d703a5ef1/scratchpad/results.csv`
and `.../scratchpad/raw/`. ebizzy was rebuilt from the LTP source (the
prior-session binary was gone); it reproduces the qualitative mmap≫malloc
lock-exposure split (mmap 58M holds/run vs malloc ~1.6M) but its absolute
records/s differ from the report's binary, so its throughput Δ is measured fresh
here, self-consistent with its HP numbers.

## Final state

Sysctls restored to safe defaults: `ivh_universal_eligible=0`, capacity=1010,
time_left_threshold=4ms, max_concurrent=8, time_left_source=1,
selection_trylock=0, migrate_mechanism=0, hot_threads=0. Daemons (`vcap`,
`MY_ivh_atc`) left running. Scratch dbench working dirs and the dedup output
file removed.
