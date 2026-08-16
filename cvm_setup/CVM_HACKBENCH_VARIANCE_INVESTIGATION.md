# Handoff: investigate hackbench overhead/variance on this CVM

You're a fresh Claude Code instance running directly on a TDX confidential
VM guest (hostname `tdx-guest`), inside the `linux-6.17-rseqport` repo,
branch `kernel-43-clean`. This doc is a handoff from a Claude session that
just spent the night getting IVH bootstrapped and running correctly on this
exact machine, written so you have full context without needing it
re-explained. Read this fully before touching anything.

## What IVH is (one paragraph)

IVH is a guest-side kernel scheduler mechanism that mitigates
lock-holder-preemption (LHP) under virtualization: a BPF hook fires
*before* a thread enters a spinlock-protected critical section
(`ivh_cs_enter()` / `bpf_sched_pre_lock_migrate()`), and if the thread's
current vCPU looks likely to be preempted by the host soon, IVH migrates
the thread to a healthier vCPU first — pre-emptively, not reactively. The
whole mechanism is a research kernel fork (this repo) plus a BPF program
(`tools/bpf/MY_ivh_atc.bpf.c` + daemon `MY_ivh_atc`) plus a userspace
capacity-probing daemon (`vcap_probe`, from the sibling repo
`vsched_main/vcapacity`).

## Machine topology (three machines involved tonight)

- **`bench-18c-2`** — the reference/validation machine (NOT this one). This
  is where every number in this repo's docs (`tools/bpf/docs/*.md`) was
  measured, including the headline **~11.4s mean hackbench result**
  (`hackbench -T -g 1 -f 8 -l 400000` via `ivh_exec -v`) that
  `cvm_setup/setup_cvm.sh test`'s pass/fail check is calibrated against.
- **`mars`** — the physical/hypervisor host. Runs this CVM as a libvirt
  guest.
- **`tdx-guest`** — this machine, the actual TDX confidential VM. Brought
  up tonight from scratch by cloning this repo + copying
  `vsched_main/vcapacity` over manually (see "why not just clone
  everything" below).

## Why this machine needed manual fixes tonight (context, already fixed)

Bringing this CVM up from a fresh clone surfaced **five real bugs in this
repo**, all now fixed and pushed to `kernel-43-clean` (check `git log
--oneline -10` if you want the exact commits): `custom_modules/` and
`setup.sh` were incorrectly gitignored (a fresh clone had no module source
and no module-loader script at all), `install_module.sh` relied on a
kbuild wrapper file that only existed on the reference machine,
`/home/nick/IVH` (the script with every validated sysctl) was never in git
anywhere and is now tracked as `cvm_setup/IVH_start.sh`, `tools/bpf/vmlinux.h`
needed regenerating from this exact kernel's live BTF (correctly
gitignored — machine-specific — but nothing was regenerating it), and
`ivh_exec` needed a symlink from `/home/nick/ivh_exec` (a separate
hardcoded path several scripts use) to where the build actually puts it.

**One thing still NOT fixed, by design, not oversight**: `vsched_main`'s
`vcapacity` submodule points at `vSched/vcapacity` on GitHub, a different
org's repo that the user doesn't have push access to. `vcap_probe.cpp` —
the file the running daemon is built from — has never been pushed there.
So `vsched_main` can't be `git clone`d; it was copied onto this machine by
hand (`bench-18c-2` → `mars` → here, via `rsync`). If you ever need to
re-sync `vsched_main/vcapacity`, that's why `git clone` won't work for it.

## Current state on this machine, as of the handoff

- Kernel `6.17.0-rseqport73+` built and booted (matches `bench-18c-2`).
- `vsched_module.ko` loaded, `/proc/vcap_info` and `/proc/ivh_debug`
  present.
- `MY_ivh_atc` and `vcap_probe` daemons running (verify: `pgrep -xa
  MY_ivh_atc`, `pgrep -xa vcap_probe` — should be exactly 1 each).
- All 7 mechanism sysctls and 3 BPF gate constants verified correct via
  `./cvm_setup/setup_cvm.sh test` sections 3–4 (all `[PASS]`) — i.e. **the
  CVM-safe TSC-only pipeline is confirmed wired correctly**:
  `ivh_universal_eligible=1`, `ivh_ref_steal_enabled=0`,
  `ivh_steal_source=2`, `ivh_uc_used_source=0`, `ivh_cap_source=3`,
  `ivh_uc_min_steal_ns=500000`, `ivh_capacity_threshold=1010`,
  `IVH_CAP_HARDFLOOR=880`, `IVH_CAP_TOPBAND=50`, `IVH_CAP_MARGIN=20`.
- All build artifacts present (`MY_ivh_atc`, `ivh_exec`, `NHextend3`,
  `NHextend4`, `vcap_probe`, `ebizzy`, `dbench`, `hackbench`, `sysbench`,
  `pbzip2`).

So sections 1–4 of `./cvm_setup/setup_cvm.sh test` are clean. **Section 5
(the timed hackbench batch) is the open problem.**

## The actual problem: your investigation target

`./cvm_setup/setup_cvm.sh test` runs `hackbench -T -g 1 -f 8 -l 400000`
(via `/home/nick/ivh_exec -v`, the IVH-eligible wrapper) 6 times, 5s apart.
On this CVM, one batch produced:

```
run 1: 29.532s
run 2: 46.343s
run 3: 37.484s
run 4: 35.876s
run 5: 38.158s
```

Two concerns, both open:

1. **Absolute time is ~3x the reference machine's ~11.4s**, not the
   "roughly 2x" that was informally guessed mid-session. Some slowdown
   relative to `bench-18c-2` is *expected* — this is a TDX confidential VM
   with real memory-encryption/isolation overhead on top of whatever the
   underlying hardware costs, and `bench-18c-2`'s ~11.4s was never a
   portable target, only a same-machine-over-time one. But the magnitude
   here hasn't actually been explained, only guessed at.
2. **Run-to-run variance is large and non-monotonic**: 29.5 → 46.3 → 37.5
   → 35.9 → 38.2. If this were pure EMA-convergence settling (the
   published `ivh_uc_capacity` needs time to reach steady state after the
   daemons start — ~130s half-life on `bench-18c-2`, see
   `tools/bpf/docs/ivh_final_tsc_only_build_2026-08-08.md` §6 for the
   mechanism), you'd expect a roughly *monotonic* trend as it settles, not
   run 2 being the worst of the batch. That non-monotonicity is the
   specific thing that hasn't been explained yet and is worth chasing.

## Concrete things to check, roughly in order of how cheap they are

1. **EMA convergence state** — the daemons were only just started when
   this batch ran. Check:
   ```
   grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '{n=$2+0; if(n<8){a+=$5;c++}else{b+=$5;d++}} END{printf "cap_cont=%.0f cap_clean=%.0f\n", a/c, b/d}'
   ```
   Clean (uncontended) vCPUs should read ~1023 once settled. If they're
   still depressed, `sleep` (genuinely idle, not a busy loop — a busy wait
   is itself load and holds the EMA down) and re-check before drawing any
   conclusion from timing.

2. **Corunner presence/consistency** — the reference-machine methodology
   assumes a *real, running* corunner VM contending for host cores (that's
   the whole point of the benchmark). Confirm something is actually
   generating host contention right now (the user mentioned a sysbench
   corunner earlier tonight — confirm it's still running and check whether
   its own load is itself variable, which would directly explain erratic
   hackbench times independent of any IVH problem).

3. **vCPU count vs. the reference machine** — `bench-18c-2` is a 16-vCPU
   box; the hackbench parameters (`-g 1 -f 8`) and the IVH sysctls
   (`ivh_max_concurrent=8`, the hardcoded `cpu < 8` contended/clean split
   in `setup_cvm.sh`'s corunner check) were tuned against that. Run
   `nproc` here — if this CVM has a different vCPU count, several
   assumptions baked into the validated numbers don't transfer directly,
   and that's worth knowing before chasing variance as a "bug."

4. **IVH-off baseline for comparison** — toggle `echo 0 | sudo tee
   /proc/sys/kernel/ivh_universal_eligible` and run the same batch. If
   IVH-*off* is *also* highly variable on this CVM, the variance is a
   property of the CVM/corunner environment, not something IVH is doing —
   important to rule in/out early rather than assume it's an IVH problem.
   (Remember to set it back to `1` afterward — it's the sole gate; the
   whole IVH pipeline no-ops silently at `0`.)

5. **A lighter, faster iteration loop** — you don't need the full `-l
   400000` (n=6) batch for every experiment while investigating; it takes
   minutes per batch here. `cvm_setup/setup_cvm.sh test` now supports
   `HACKBENCH_LOOPS` and `HACKBENCH_RUNS` env var overrides (e.g.
   `HACKBENCH_LOOPS=100000 HACKBENCH_RUNS=3 ./cvm_setup/setup_cvm.sh
   test`) for cheaper smoke-test iterations — use a smaller size while
   narrowing down the cause, then confirm on the real `-l 400000` size once
   you have a hypothesis. Note: the absolute-time pass/fail check only
   applies at the exact reference size (`-l 400000`); at any other size
   the script reports timing/variance stats without a target to compare
   against.

6. **TDX-specific overhead sources**, if the above rule out the mundane
   explanations — worth researching directly rather than assuming: does
   memory encryption (TME/MKTME or TDX's own private-memory encryption)
   introduce *variable* per-access latency depending on cache/TLB state in
   a way that would show up exactly like this? Does the TDX module's own
   VMM-guest transition path (`#VE` handling, `TDCALL`s) have different
   characteristics than normal KVM under load? This is genuinely open —
   don't assume TDX explains it without evidence, but don't rule it out
   either.

## What NOT to do

- Don't touch `vsched_main/vcapacity` via git (see above — it'll fail, and
  isn't the fix anyway).
- Don't change the *shipped* sysctls in `cvm_setup/IVH_start.sh` /
  `/home/nick/IVH` to chase this without very strong evidence — those are
  the exact values validated on `bench-18c-2` across dozens of batches
  this project cycle, and changing them changes what's being tested, not
  just this CVM's behavior.
- Don't conflate "different absolute time than the reference machine"
  with "broken" — some gap is structurally expected. The question is
  whether the *specific* gap and the *variance* are explained, not whether
  they can be made to disappear.

## Where things live

- `cvm_setup/setup_cvm.sh` — the bring-up/test script (`build`,
  `post-reboot`, `test`, `finish` subcommands).
- `/home/nick/IVH` (symlink-equivalent content tracked as
  `cvm_setup/IVH_start.sh`) — sysctl values + daemon start, heavily
  commented with the reasoning/measurements behind each one.
- `tools/bpf/docs/*.md` — the whole project's measurement history; several
  of these are directly relevant (`ivh_final_tsc_only_build_2026-08-08.md`
  for the EMA convergence mechanism, `ivh_script_reproduction_audit_2026-08-08.md`
  for another instance of "silently wrong sysctl caused erratic results,"
  which is worth reading given tonight's symptom rhymes with it).
- `/proc/ivh_debug`, `/proc/vcap_info` — live diagnostic state.
- `/proc/sys/kernel/ivh_*` — every sysctl, live-tunable.

Report back with what you find — especially whether IVH-off is *also*
erratic on this box (step 4 above), since that one observation determines
whether this is an IVH investigation at all or an environment one.
