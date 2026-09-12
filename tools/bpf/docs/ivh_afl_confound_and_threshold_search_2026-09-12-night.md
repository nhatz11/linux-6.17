# AFL threshold search, confound isolation, and post-reboot reconfirm — overnight 2026-09-12

Continuation of `ivh_nhextend_adaptive_futex_lock_2026-09-12.md` (same day, later that night,
then a VM reboot). Kernel unchanged (`6.17.0-G-LOCK-25-tier1confirm+`).

## 1. Trio screen: does (capacity threshold, time-left threshold, AFL spin-gate) tuning fix the
migration-alone "valley"?

The user's theory: migration alone only helps at very short or very long CS, not mid-CS (matches
the loop_spin sweep valley already documented: real losses at 100000/50000/25000, washes/small
wins elsewhere). Swept 8 corners of `(ivh_capacity_threshold∈{950,1020}, ivh_time_left_threshold_ns
∈{50000,4000000}, IVH_AFL_SPINS∈{64,1024})` against a same-day PV baseline, 5 loop_spin values
(600000, 100000, 50000, 25000, 5000), 1 round/10s each.

**Result**: all 8 trios showed +42% to +134% vs PV at every loop_spin value tested, including the
valley points, with under 3 percentage points of spread between trios at any given loop_spin —
i.e. no threshold effect was detectable above the (unmeasured, single-round) noise floor. `IVH_AFL_SPINS_BEFORE_CHECK` was made an env-var override (`IVH_AFL_SPINS`, mirroring `IVH_AFL_WAKE`/
`IVH_AFL_STALE_NS`) specifically to make this sweep possible without rebuilding per value.

## 2. The critical isolating experiment: is it the lock, or a confound?

To rule out "today's host is just less contended than yesterday" as the explanation, ran a 3-arm
confirm (PV / IVH-alone-NHextend3 / IVH+adaptivespin-NHextend-full) at the SAME cap=1010/
tleft=4000000, 3 rounds, all 8 loop_spin values from the original 2026-09-11 sweep. Finding:
today's IVH-alone reproduced the valley floor closely (loop_spin=50000: -19.3% vs yesterday's
-19.0%) but NOT the long-CS end (300000: +8.0%→+60.8%, a 52.8pp move) — so host-drift is ruled out
*at the valley floor specifically*, not generally.

**Opus review dispatched** (fresh agent, full context) found a real, more fundamental problem:
arm C (`NHextend-full`) and arms A/B (`NHextend3`) are different binaries, differing in TWO ways —
the adaptive lock, AND the `read_vcap_steal()`-moved-outside-CS fix from the same day's earlier
work. The valley-closing effect could be either. Opus's strongest positive evidence: the C-vs-B
marginal effect at loop_spin=600000 (+19.8%) reproduced yesterday's independently-confirmed +19.7%
almost exactly — genuine cross-session corroboration, though not a full resolution. Recommended
next step: a 4th arm, `NHextend-full` with the lock disabled (`IVH_AFL_DISABLE=1`), same binary as
C, to isolate the lock from the CS-shortening fix.

**Ran that isolating test** (8 rounds, loop_spin=100000/50000/25000, per-round logged). Result did
NOT give the clean answer expected:

| loop_spin | PV | B′ (NHextend-full, lock disabled) | C (NHextend-full, lock enabled) | C vs B′ |
|---|---|---|---|---|
| 100000 | 47,714 | 21,146 (**-55.7%** vs PV, t=-104) | 83,960 (+76.1%, t=49) | **+297.3%** (t=86) |
| 50000 | 97,991 | 42,740 (**-56.3%**, t=-78) | 173,040 (+76.7%, t=39) | **+305.2%** (t=64) |
| 25000 | 218,132 | 91,987 (**-57.8%**, t=-112) | 356,461 (+63.5%, t=56) | **+287.8%** (t=71) |

B′ is not a valid neutral control — it's dramatically *worse* than even NHextend3's original
migration-alone valley (-12% to -19%). Working hypothesis (not independently verified): NHextend3's
original spin loop polls with `lfence`; `ivh_afl_lock()`'s spin-wait uses `pause`
(`ivh_afl_cpu_relax()`), which on modern microarchitectures inserts real per-iteration latency
(deliberately, for power/SMT reasons — the design doc itself estimated ~140 cycles on Skylake+).
Fine when bounded by a real sleep escape hatch; ruinous when forced to spin on it forever with
sleeping disabled. So B′ answers "does the escape hatch matter, given this exact spin
implementation" (yes, overwhelmingly, +287–305%, t=64–86, the cleanest same-binary result of the
whole investigation) rather than "does the lock beat NHextend3's original design" (still open).

**Named next step, not yet done**: change the disabled-path spin instruction to `lfence` (matching
NHextend3) and rerun the same 3-point test. If B′ then lands near NHextend3's original -12% to
-19%, the binary-diff confound is finally closed. If it still lags badly, the `pause`/`lfence`
question needs its own dedicated microbenchmark before trusting any cross-binary comparison in this
whole investigation (including yesterday's own +17.5%/+19.7% headline numbers).

## 3. Hackbench regression check (scoped correctly per Opus's review)

`hackbench -T -g1 -f8 -l400000`, 5 rounds, cap=1010/tleft=4000000: PV 59.57s vs IVH 15.55s,
**+73.9%**, 5/5 positive. This exercises the kernel migration engine only (the userspace lock isn't
in this path), and `g1/f8` is close to this feature's best-case configuration — supports "no
regression observed at g1/f8 under that night's load," not an unqualified no-regression claim.

## 4. Thread-count sweep — interrupted, results unreliable, not re-run

Attempted a sweep of thread count (16/8/4/2/1) at loop_spin=5000 to test whether the adaptive-spin
increment shrinks with less contention (user's theory: the effect depends on how many threads are
waiting at once). Interrupted partway through by explicit user request (pivoting to other work).
**The two completed data points (threads=16: -0.5%, threads=8: -1.0%) contradicted every other
same-config measurement taken that day (which consistently showed +12% to +24% at threads=16,
loop_spin=5000)** — flagged as likely anomalous/noisy rather than a real reversal, but never
re-confirmed. Do not cite these two numbers; re-run cleanly before drawing any conclusion about
thread-count sensitivity.

## 5. VM reboot, post-reboot reconfirm

VM rebooted (same kernel, `6.17.0-G-LOCK-25-tier1confirm+`, fresh 2-minute uptime). All sysctls
reset to compiled defaults, daemons gone, as expected. Brought back up via
`cvm_setup/nhextend_full_best_config.sh` (written the previous night — the full sysctl block, both
daemons, the `ivh_cfg` BPF map write, `ivh_universal_eligible=1`, kernel `spin_mode 1`), then a
short idle wait (15s, not full multi-minute EMA convergence — a fast sanity check, not a rigorous
run) and a live migration canary (`bpftool map dump last_migration`, confirmed real NHextend3
migration events) before trusting any number.

**3 rounds, loop_spin=600000, ~1.6ms CS (the "known good" point), stopped-early check built in
after round 1**:

| mode | mean | range | vs PV | vs IVH |
|---|---|---|---|---|
| PV | 5,764 | 5,415–6,029 | — | — |
| IVH | 11,505 | 11,197–11,796 | **+99.6%** | — |
| IVH+adaptivespin | 13,793 | 13,630–14,058 | **+139.3%** | **+19.9%** |

Matches the pre-reboot numbers closely (PV 5,834/IVH 11,434 +96.0%/IVH+AS 13,700 +134.8%/+19.9%
vs IVH) — reboot did not change anything material. 3/3 rounds consistent, no early-stop triggered.

## 6. Bottom line, as of this write-up

- **Solid, reproduced across a reboot**: PV/IVH/IVH+adaptivespin at loop_spin=600000 is a clean,
  stable, reproducible result (+99.6% / +139.3% / +19.9% increment).
- **Still open**: whether the adaptive lock's own contribution (vs. the CS-shortening fix baked
  into the same `NHextend-full` binary) is real at the magnitude claimed. The escape-hatch-vs-no-
  escape-hatch effect (+287-305%) is real and enormous but measures something narrower than
  originally intended, due to an apparent `pause`-vs-`lfence` spin-instruction difference in the
  disabled control path.
- **Not yet re-run**: the thread-count sweep (interrupted, two suspect data points, needs a clean
  restart) and the `lfence`-fix rerun of the isolating test (the one experiment that would close
  the remaining confound).
