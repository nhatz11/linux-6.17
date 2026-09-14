# NHextend3: first live validation of the migration engine on the GLOCK rebuild, 2026-09-11

Kernel `6.17.0-G-LOCK-25-tier1confirm+`, branch `ivh-rebuild-main` (source tree
`/root/kernels/linux-6.17-vanilla`). Docs/history repo `/root/linux-6.17`,
branch `kernel-43-clean`. Migration/`ivh_universal_eligible` had never been
turned on and tested on this rebuild lineage before today — confirmed 100%
dormant as of `ivh_adaptive_spinning_glock13_findings_2026-09-03.md` §7 (a
kprobe on `bpf_sched_pre_lock_migrate()` recorded 0 hits across a full
hackbench run). Step 8 of `ivh_rebuild_plan.md` was "specified, sysctl +
daemon" but never executed. This doc is that execution, plus a CS-length
sweep the rebuild plan never ran either.

## 1. Setup performed this session (no kernel rebuild required)

1. `spin_mode 1` (STOCK_PV) held fixed throughout every test below, so
   migration is the only thing varying — adaptive spinning (tier 1/tier 2,
   `ivh_pv_tier1_confirm`) stayed at its default-off state the whole time.
2. Full production sysctl combo (TSC-only capacity/steal pipeline, matching
   `cvm_setup/IVH_start.sh`'s validated values, adapted to this tree's
   `/proc/sys/kernel` paths — `ivh_universal_eligible` held at 0 until the BPF
   program was loaded, per that script's own documented hazard):
   ```
   ivh_capacity_threshold=1010   ivh_time_left_threshold_ns=4000000
   ivh_max_concurrent=8          ivh_time_left_source=1
   ivh_selection_trylock=1       ivh_migrate_mechanism=0
   ivh_steal_source=2            ivh_cap_source=3
   ivh_uc_enabled=1              ivh_uc_used_source=0
   ivh_uc_min_steal_ns=500000    ivh_uc_window_ns=200000000
   ivh_uc_duty_ns=0              ivh_uc_ema_alpha_q16=868
   ivh_uc_min_avail_pct=10       ivh_tks_deadband_ns=50000
   ivh_tks_idle_sub=0            ivh_tks_phase_pct=100
   ivh_tks_carry_ticks=8
   ```
3. Loaded `/root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc` (built
   `IVH_CAP_HARDFLOOR=700`, matching production) — loaded and attached clean
   against this modified kernel's BTF despite two harmless CO-RE relocation
   misses in `search_latency` (`struct rq.avg_latency` doesn't exist in this
   rebuild's `struct rq` — libbpf gracefully substitutes an invalid-insn trap
   for that dead field access; the destination-selection hook `process_cpu`/
   `test3` itself loaded and JITed with no relocation failures). Confirmed via
   `bpftool prog list`/`map list`, not just process-alive.
4. `bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0` (must match
   `ivh_cap_source`, per the launch script's own comment — a mismatch here
   silently makes the BPF program read a flat 1024 and never migrate anyone).
5. Launched `vcap_probe -p 200 -s 200` (from `/root/vcapacity/`).
6. `/proc/ivh_debug` does **not exist on this rebuild** (deliberately not
   ported — see `kernel/sched/fair.c:13653`'s comment; the underlying counters
   still exist, just no `/proc` reader). Convergence/activity was instead
   confirmed directly via `bpftool map dump name last_migration`, which shows
   real per-CPU migration events (`comm`, `src_cpu`, `dst_cpu`, `count`) — used
   as the canary before trusting any timed number.
7. **Canary, before any timed run**: one `NHextend3 -n 16` under
   `ivh_universal_eligible=1` produced real, attributable migrations
   (`comm=NHextend3 src_cpu=0 dst_cpu=9 count=210`, growing further on
   inspection) — the mechanism is genuinely firing, not a silent no-op.

## 2. Baseline: does migration help NHextend3 at all here?

`NHextend3 -n 16` (default `NHEXTEND_LOOP_SPIN=600000`, ~1.6ms CS — the exact
length that showed the cleanest win in the *old*, pre-rebuild kernel's
`ivh_state_of_the_art_2026-07-20.md` §3.3, +21% to +35% there). 5 rounds,
interleaved, toggling only `ivh_universal_eligible` 0/1 between arms:

| | STOCK (no migration) | IVH migration on |
|---|---|---|
| mean "Ran for N times" | 2257.4 | 3781.6 |
| range | 2150–2337 | 3710–3847 |

**+67.5%, 5/5 rounds, fully non-overlapping bands.** Confirms migration
provides a real, large, first-time-measured benefit on this rebuild's engine
at NHextend3's own default CS length — bigger than the old kernel's
historical number, though architecture/sysctls/host-contention level all
differ too much to call that a direct comparison.

## 3. The loop_spin sweep: does it hold at shorter CS lengths?

3-round screen, `NHEXTEND_DURATION=10`, sweeping `NHEXTEND_LOOP_SPIN` down
from the default:

| loop_spin | ~CS | improvement | consistency |
|---|---|---|---|
| 600,000 | ~1.6ms | **+59.6%** | 3/3, tight |
| 300,000 | ~800µs | +8.0% | 3/3 positive, below a 10% bar |
| 150,000 | ~400µs | +0.2% | mixed sign — a wash |
| 100,000 | ~234µs | **-11.1%** | 3/3, real loss |
| 50,000 | ~102µs | **-19.0%** | 3/3, worst point |
| 25,000 | ~50µs | -4.0% | 3/3, smaller loss |
| 10,000 | ~22µs | **+26.6%** | 3/3, tight |
| 5,000 | ~13µs | **+20.4%** | 3/3, tight |

A clear "migration cost valley" shape (mirrors the *shape* the old kernel's
§3.3 found, though not the same crossover points — that table was never
re-run on this engine before). **This 3-round/10s screen was not fully
trusted at face value** — see below, one of its two "recovered" short-CS
wins did not survive a proper confirm pass.

## 4. Confirm pass (8 rounds, `NHEXTEND_DURATION=20`, `-v -l` for real CS length)

| loop_spin | screen (3r/10s) | confirm (8r/20s) | consistency |
|---|---|---|---|
| 10,000 | +26.6% | **-14.6%** (reversed!) | 8/8 negative, -10.2% to -17.6% |
| 5,000 | +20.4% | **+20.4%** (matched exactly) | 8/8 positive, +12.5% to +31.8% |

**10,000 flips from a clean win to a clean loss.** The `-l` CS-length output
explains why: at loop_spin=10,000, stock CS averaged **24.8µs** vs migration's
**33.5µs — 35% longer**. Migration is directly inflating the wall-clock
duration of the critical section it interrupts (`NHextend3`'s
`last_cs_overall_ns` is wall-clock, so a mid-CS migration stall is counted
inside it) — that inflation alone is large enough to erase the whole
throughput benefit and go net negative. At loop_spin=5,000 the same CS-length
metric stays flat (13.1µs → 13.3µs, +1.7%) — no such confound, and the +20%
throughput win is clean.

**Methodological lesson, stated plainly**: the 3-round/10s screen was
actively *wrong*, not just noisy, for loop_spin=10,000 — not merely
imprecise. The loss/wash valley region (150,000 through 25,000) was **only
ever measured at this same 3-round/10s rigor** and has not been re-confirmed
at 8-round depth. Treat those specific numbers as provisional, not final, if
future work wants to build on the exact valley shape rather than just the
5,000 endpoint.

## 5. Solidify pass: 10 rounds at loop_spin=5,000

| | stock | migration |
|---|---|---|
| mean "Ran for N times" | 884,924 | 1,072,368 |
| range | 842,508–912,177 | 890,532–1,118,232 |

**+21.2% mean, 10/10 rounds positive** (range +5.7% to +27.6%), paired-diff
sd=6.5pp, se=2.1pp, **t≈10.24** — one of the most statistically decisive
results in this project's history. CS length stayed flat (stock 13,074ns vs
migration 13,221ns, +1.1%), ruling out the CS-inflation confound seen at
10,000. Three independent passes at this loop_spin value now agree closely:
3-round screen +20.4%, 8-round confirm +20.4%, 10-round solidify +21.2%.

One outlier worth noting, not contradicting the result: round 7 had both the
smallest improvement (+5.7%) *and* the only elevated CS length that run
(16,737ns vs the ~12–13k baseline everywhere else) — a real transient
CS-inflation event that round, consistent with the same mechanism identified
at loop_spin=10,000, just rare at this shorter length rather than dominant.

## 6. Exact reproduction recipe

```bash
# one-time setup (§1 above), then per comparison:
NHEXTEND_DURATION=20 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend3 -n -v -l
# with, immediately before each invocation:
echo 0 > /proc/sys/kernel/ivh_universal_eligible   # arm A: stock
echo 1 > /proc/sys/kernel/ivh_universal_eligible   # arm B: migration
```
`-n` = unpinned (no_pin), `-v` = per-thread verbose stats, `-l` = CS-length
stats (`show_last` — this is the flag that prints "Global avg overall", not
`-v`; both are passed together). Nothing else differs between arms. The
toggle's realness was independently verified three ways before trusting any
number: live read-only sysctl polling showed it actually flipping in sync
with round boundaries (not stuck); `bpftool map dump last_migration` showed
real, attributable `NHextend3` migration events appearing only once eligible
was set; and the CS-length signature itself (35% inflation at 10,000, flat
at 5,000) could not appear under a stuck/non-switching configuration.

Scripts: `/root/ivh_tools/nhextend3_stock_vs_migration.sh` (baseline),
`/root/ivh_tools/nhextend3_loopspin_sweep.sh` (3-round screen — note its
inline Python summary has a cosmetic bash-array-join bug, harmless, the raw
`ran_for=` lines are unaffected), `/root/ivh_tools/nhextend3_confirm_valley.sh`
(8-round confirm), `/root/ivh_tools/nhextend3_5000_10round.sh` (10-round
solidify).

## 7. Bottom line

Migration is a real, large, reproducible win for NHextend3 on this rebuild's
engine — confirmed for the first time this session, not just carried forward
from old-kernel history. **loop_spin=5,000 (~13µs CS) is the shortest CS
length validated so far with a robust ≥10% win** (actually ~21%, t≈10.2).
Do **not** trust loop_spin=10,000 despite its promising 3-round screen — it
reverses to a real loss once measured properly, via a genuine CS-length-
inflation mechanism, not noise. The 150,000–25,000 valley region is a real
shape but only screened, not confirmed — re-run at 8+ rounds before relying
on its exact numbers.
