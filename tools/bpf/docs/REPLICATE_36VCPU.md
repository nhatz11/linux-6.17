# Replicating the lock-skipping win at 36 vCPU

Written 2026-09-28 before a reboot from 16 -> 36 vCPU. The 16-vCPU result
(below) is a NULL; the recorded win was at 36 vCPU, so the point of the reboot
is to find out whether scale is the explanation.

## What the 16-vCPU run found (G-LOCK-48, uniform contention)

| contrast | dbench | hackbench |
|---|---|---|
| tier1 OFF: skip vs no-skip | -0.22% (t=+0.02) ns | +2.73% (t=-3.17) WORSE |
| tier1 ON:  skip vs no-skip | +0.90% (t=-0.10) ns | +1.80% (t=-2.16) WORSE |

Eviction firing rate, per million queued acquisitions:
dbench 487/396, hackbench 182/148, dentry 272/229 (tier1 off/on).
Tier 1 suppresses evictions by 16-19% -- real, but far too small to explain
the null. The wall is the firing rate itself: 0.015-0.049% of acquisitions.

## What must be restored after the reboot, in order

1. **`/root/linux-6.17/cvm_setup/goto_mode.sh`** -- calibration sysctls do NOT
   persist, and boot defaults flatten the steal estimator to zero. Nothing
   below is valid until this has run.
2. **Host contention.** The 16-vCPU run used UNIFORM contention (all vCPUs
   equally loaded). The recorded 36-vCPU result's contention pattern is not
   documented -- decide and record it, because migration verdicts are known to
   reverse with it.
3. **The TD pid changes every reboot.** Any host-side schedstat reading against
   a stale pid silently returns zeros.
4. `ivh_pv_tas` / `ivh_pv_allow` are BOOT parameters -- `spin_mode` refuses
   modes that the boot config does not allow. Check `spin_mode h` output.

## The trap: workload sizing

The registry (`/root/ivh_tools/ivh_benchmarks.sh`) is sized for **16 vCPUs**.
The recorded 36-vCPU experiments used different configurations -- the only one
documented is hackbench, rescaled to `-T -g4 -f8 -l100000` (vs the registry's
`-T -g1 -f8 -l150000`). dbench's 36-vCPU client count is NOT recorded.

**Re-running with 16-vCPU sizing on a 36-vCPU box does not replicate anything.**
A null under wrong sizing is uninformative. Either recover the original configs
or state plainly that the sizing differs.

## Harness to use

`/root/ivh_tools/skipfair.sh` -- the 2x2 already built and validated:
`{tier1 off, tier1 on} x {skip, no skip}` + stock PV, n=8, scored on
per-acquisition WALL wait (`ivh_slowpath_wait_ns / _events`), which is the
recorded metric and the one fair to eviction.

Override sizing via BENCHES + a registry edit; set REPS=8.

**Do NOT score eviction on on-CPU wait** (`wall - halted`). Eviction creates no
halts, so that metric charges its overhead and never credits its mechanism. It
is the right metric for tier 2 and the wrong one for skipping.

## Original harnesses are GONE

`waitskip.sh`, `stepcombowl.sh`, `RUNCOMBO.sh` no longer exist on this box. The
design is reconstructed from memory notes only. The second spin-threshold arm
of the original (`ivh_pv_spin_threshold=1048576`, ~37 ms, waiters spin instead
of halting) was NOT reproduced in `skipfair.sh`; the recorded win appeared at
both 32768 and 1048576, so re-adding that arm would strengthen a replication.
