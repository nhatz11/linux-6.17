# `ivh_tks_idle_sub` — pre-reboot readiness, build recipe, and the `phase_pct` recalibration question

**Date:** 2026-08-12 (continues `ivh_undershoot_correction_2026-08-09.md` and `ivh_solution_search_2026-08-09.md`)
**Running kernel:** `6.17.0-rseqport72-plzwork+`, `nohz=off` on the live cmdline
**Nothing rebooted, nothing installed, no GRUB touched, no commit, no push.**

---

## 0. The four answers, up front

**(1) State of the idle-subtraction fix.** The code in the working tree is the **complete, real fix** — not a placeholder, not a blunt stand-in for something more nuanced. The blunt on/off *is* the correct correction, and this document proves it empirically (sec 3) rather than asserting it. It is **NOT live**: `/proc/sys/kernel/ivh_tks_idle_sub` **does not exist** on the running kernel and the whole `ivh_tks_idle_sub` symbol is absent from the running binary. The claim that it is "already a live sysctl wired to a real code branch" is **true of the source file and false of the running system** — `core.c:396` and `core.c:2447` are uncommitted working-tree lines. **A reboot is genuinely required; there is no live A/B to run.** One small completeness gap in `sched.h` was found and closed (sec 2.3), and the whole thing recompiles clean with byte-identical codegen (sec 2.4).

**(2) Build + reboot instructions:** sec 5, copy-pasteable, matching this project's established `sudo make` / `modules_install` / `install` / `grub-reboot` process. `nohz=off` is already persistent in `/etc/default/grub`, so the new kernel inherits it with no GRUB action.

**(3) Will this require re-calibrating `ivh_tks_phase_pct`?** **Very likely no for the value, definitely yes for the validation.** The `phase_pct=100` result was measured on continuously-runnable load where the idle delta is **identically, measurably zero** (sec 4.1: `didle == 0` on every contended vCPU in every block) — so the fix is a provable byte-for-byte no-op on exactly the workload that produced that number, and the "1.000 tick per event" finding is **not** conditional on the idle subtraction after all. What *does* change is the estimator's **domain**: idle-heavy load, where `phase_pct` has never been validated because the idle debt swamped it. There, pre-reboot emulation puts `phase_pct=100` at ratio **1.03–1.06** (a ~4 % overshoot, per-event deficit 0.87 tick instead of 1.00) rather than exactly 1.000. Full reasoning and numbers in sec 4.

**(4) Post-reboot validation checklist:** sec 6. It re-measures the deficit/event ratio in **both** regimes rather than assuming `phase_pct=100` carries over, and it checks the one failure mode that could make `idle_sub=0` worse than the disease (phantom steal on clean vCPUs) **first**, before anything else is trusted.

---

## 1. Exact current state, verified rather than assumed

| check | result |
|---|---|
| `ls /proc/sys/kernel/ivh_tks_idle_sub` | **No such file or directory** |
| `ls /proc/sys/kernel/ | grep ivh` | 59 knobs, `ivh_tks_deadband_ns` / `phase_pct` / `carry_ticks` present, **no `idle_sub`** |
| `uname -r` | `6.17.0-rseqport72-plzwork+` |
| `/proc/cmdline` | `... quiet splash nohz=off crashkernel=...` — `nohz=off` live |
| `ivh_tks_phase_pct` live value | **0** — the recommended `100` has **not** been applied by anyone since the last report |
| `/etc/default/grub` | `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash nohz=off"` — persistent, inherited by any new kernel |
| working tree | `core.c`, `cputime.c`, `fair.c`, `sched.h`, `MY_ivh_atc.bpf.c` modified, uncommitted |
| host corunner | still running (cpu0-7 accrue steal, cpu8-15 do not) |
| daemons | one `MY_ivh_atc`, one `vcap_probe -p 200 -s 5000` |

So the framing to carry into the reboot is: **the sysctl exists in the source and only in the source.** Anything that reads `core.c` will see it; nothing that reads the running kernel will.

---

## 2. Is the committed-to-tree code the complete fix, or a stand-in?

### 2.1 What the reports said was written

`ivh_undershoot_correction_2026-08-09.md` sec 7 describes exactly three pieces: a new sysctl definition with rationale comment; three lines in `ivh_tick_steal_accumulate()` that still compute the delta and still seed `prev_idle_ns` so the modes are runtime-switchable without a discontinuity; and a `ctl_table` entry deliberately without `.extra1`/`.extra2`.

### 2.2 What is actually in `core.c`

All three, and nothing missing:

- `core.c:363-396` — the rationale block and `unsigned long ivh_tks_idle_sub = 1UL;`
- `core.c:1671-1686` — the `ctl_table` entry, `proc_doulongvec_minmax`, no extras, with the `int *` vs `unsigned long *` hazard documented in place
- `core.c:2436-2449` — the consumer:

```c
d_idle_c = ivh_tsc_ns_to_cycles(idle_ns > rq->ivh_tks_prev_idle_ns
                                ? idle_ns - rq->ivh_tks_prev_idle_ns : 0);
if (!READ_ONCE(ivh_tks_idle_sub))
        d_idle_c = 0;

avail_c  = now - rq->ivh_tks_prev_tsc;
avail_c  = (avail_c > d_idle_c) ? avail_c - d_idle_c : 0;
```

`ivh_idle_ns()` is still called unconditionally and `seed:` still writes `rq->ivh_tks_prev_idle_ns = idle_ns` on both paths, so flipping the knob at runtime cannot produce a step in the series. That is precisely the described behaviour.

### 2.3 Is a *blunt* on/off the right shape, or does a scaled/partial subtraction still need writing?

This was the substantive question, and the answer is that **the correct scale factor is exactly zero under `nohz=off`**, so a blunt drop is not a coarse approximation of the right fix — it *is* the right fix. The arithmetic, per tick interval, with an absolute periodic tick and host delivery delay `S`:

| interval type | raw gap | idle delta (kcpustat, steal-net) | `idle_sub=1` excess | `idle_sub=0` excess | truth |
|---|---|---|---|---|---|
| fully busy | `TICK + S` | `0` | `S` | `S` | `S` |
| fully idle, `S=0` | `TICK` | `TICK` | **`−TICK`** (a whole tick of debt) | `0` | `0` |
| fully idle, `S>0` | `TICK + S` | `TICK − S` | `2S − TICK` (still debt) | `S` | `S` |
| mixed, idle `i` | `TICK + S` | `i` | `S − i` | `S` | `S` |

`idle_sub=0` is exact on every row; `idle_sub=1` is exact only on the first. There is no row on which a *partial* subtraction beats zero, so a scaled variant would be strictly worse and there is nothing further to write. `ivh_idle_ns()`'s own header (`core.c:616-623`) independently confirms the "steal-net" property used in rows 3 and 4: `steal_account_process_time()` takes steal out of the tick's budget **before** the idle/user/system split, so a partially-stolen idle tick books less than `TICK_NSEC` of idle.

**Empirically** (sec 3), the blunt version lands at 1.00–1.06 of the hypervisor's own number on the load where the estimator is currently at 0.48–0.64. Nothing more nuanced is warranted.

### 2.4 What I changed, and why

One genuine gap: **`sched.h` had not been finished.** Every other `ivh_tks_*` knob is `extern`-declared and documented in the `sched.h` block that enumerates the tks knob set; `ivh_tks_idle_sub` was not, so the header's knob list was silently incomplete. The same block also still carried the **overturned** `phase_pct` rationale ("the theoretically unbiased 50 is NOT the default") — `core.c`'s copy was corrected by the previous session, `sched.h`'s was not, and `sched.h` is what other `kernel/sched/*.c` readers see.

Fixed in `kernel/sched/sched.h` (~line 248): the `phase_pct` doc now records that the measured-correct value on this kernel is **100** and why (absolute periodic hrtimer ⇒ one *whole* tick per burst), and `ivh_tks_idle_sub` is documented and `extern`-declared alongside its three siblings.

**No functional change.** Verified:

```
CC      kernel/sched/core.o     (zero warnings, zero errors)
CC      kernel/sched/fair.o
AR      kernel/sched/built-in.a
```

and the codegen is byte-identical to the disassembly the previous report recorded:

```
cf72:   mov    0x0(%rip),%rdx        # R_X86_64_PC32  ivh_tks_idle_sub-0x4
cf79:   test   %rdx,%rdx
cf7c:   cmovne %rax,%rdx
```

Branchless, correct in both directions, `mul`/`div` `TICK_NSEC` conversion and its `< 1000` sanity floor intact, no return of the `OPTIMIZER_HIDE_VAR` miscompile class.

### 2.5 Default posture: correct as shipped

`1UL` (subtract idle) = byte-for-byte the historical estimator, matching the project's shadow-first posture (`ivh_ref_method`, `ivh_steal_source=2`, `ivh_ka_enabled`). It must stay `1` in-tree: `0` is only sound with `nohz=off`, and a default of `0` would silently reinstate the unbounded-gap phantom on any tickless boot. **The fix is "boot the new kernel, then set `ivh_tks_idle_sub=0` at runtime"** — the reboot is needed because the *knob and its branch* are new code that has never been in a running binary, not because the correct value requires a boot parameter.

### 2.6 One downstream coupling the reports did not spell out — deliberate, not a bug

`ivh_uc_tick()` (the capacity publisher) keeps its **own, unconditional** idle subtraction (`avail_c = d_elapsed_c − d_idle_c`) and then clamps `d_steal_c = min(d_steal_c, avail_c)` per tick. So steal that `idle_sub=0` newly recovers **during idle intervals will be clamped away before it reaches `ivh_uc_capacity`**.

That is semantically right and worth keeping: capacity means "of the time this vCPU *wanted* the CPU, what fraction did it get", and time stolen while the vCPU was halted did not deny it work it wanted. Two consequences to carry into the test:

- **Safety:** `idle_sub=0` **cannot** collapse `ivh_uc_capacity` on idle vCPUs. The clamp is a structural guard.
- **Expectation:** the fix improves the **exported steal number** (`rq->ivh_tks_steal_ns`, i.e. `get_steal_and_preemptions()` → NHextend, and the paper's accuracy claim). Do **not** expect a large capacity/gate change. Measure it (sec 6.5), do not predict it.

Changing `ivh_uc_tick()` to match was considered and **rejected**: "idle leaves the ratio entirely" is a deliberate, calibrated property (it is what makes the signal invariant to whether vcap's spinners are present), and altering it would invalidate `ivh_capacity_threshold=1010` and the entire shadow comparison.

---

## 3. What could be tested live, and what it predicts

`ivh_tks_idle_sub=0` cannot be exercised on this kernel. But the algorithm it produces can be **emulated exactly**, out-of-kernel, and that emulation can be **validated against the kernel's own live output** on a regime where the two must agree. That is what was done.

### 3.1 The emulator

`scratchpad/idlesub_emul.bt` hooks `kprobe:account_process_tick` — the same function `ivh_tick_steal_accumulate()` is called from (`cputime.c:587`, inside `account_process_tick()` at `cputime.c:518`) — and reproduces the estimator on **raw inter-tick gaps only**, i.e. exactly what the kernel does with `d_idle_c` forced to 0: `TICK = 1 ms`, deadband 50 µs, phase added only to intervals that clear the deadband, signed carry drained when positive and floored at −8 ticks. Two independent carries run in one pass so `phase_pct=0` and `phase_pct=100` are measured simultaneously. Ground truth is the `kvm_steal_time` page read per-CPU via `scratchpad/snap.bt`, in windows bracketing the emulation.

Known limits, stated rather than hidden: it reads `nsecs` (ktime, TSC-derived) not `ivh_raw_tsc()`; the kprobe fires a microsecond or two before the real hook (a constant offset that cancels in a gap); it does not reproduce `ivh_tsc_ns_to_cycles()` rounding; and the emulation window is a few percent shorter than the steal-page window, so every ratio below is corrected by the measured tick count of an unpreempted vCPU (this correction is itself validated in 3.2, where it returns exactly 1.000 on a case with a known answer).

### 3.2 Emulator validation — continuously runnable load (8 spinners, corunner on)

| | live kernel (`idle_sub=1`, `pct=0`) | emulator (`idle_sub=0`, `pct=0`) | emulator (`pct=100`) | steal page |
|---|---|---|---|---|
| cpu0 | 7079.9 ms | 6779.8 ms | 10328.8 ms | 10799.6 ms |
| window | 20707 ms | 19810 ms | — | 20707 ms |

Scaled to the common window: live `pct=0` → **6775 ms** vs emulated **6779.8 ms** — a **0.07 % match**. That is the load where `didle ≡ 0`, so the emulator and the live kernel are computing the identical quantity, and they agree to within a rounding error. At `pct=100` the emulator reads **0.999** of the steal page, reproducing the previous report's 0.998–1.001 independently.

**The emulator is trustworthy**, and this doubles as a fresh, independent reproduction of the `phase_pct=100` result.

### 3.3 The prediction — idle-heavy load (guest ~96 % idle, corunner on)

Same 8 contended vCPUs, but with no guest load at all, so the idle term dominates. Window 22.71 s (steal page) / 21.88 s (emulator), corrected by tick count:

| cpu | steal page | **live today** (`idle_sub=1`, `pct=0`) | emul `idle_sub=0`, `pct=0` | emul `idle_sub=0`, `pct=100` | ratio at `pct=100` |
|---|---|---|---|---|---|
| 0 | 529.5 ms | 325.8 (0.59) | 380.6 | 559.6 | **1.057** |
| 1 | 591.5 | 323.8 (0.53) | 410.6 | 615.6 | **1.041** |
| 2 | 509.2 | 308.8 (0.58) | 358.6 | 534.6 | **1.050** |
| 3 | 559.0 | 317.8 (0.55) | 382.6 | 575.6 | **1.030** |
| 4 | 500.2 | 321.8 (0.62) | 359.6 | 524.6 | **1.049** |
| 5 | 642.9 | 319.3 (0.48) | 431.6 | 663.6 | **1.032** |
| 6 | 500.8 | 324.8 (0.62) | 360.6 | 521.6 | **1.042** |
| 7 | 492.5 | 325.3 (0.64) | 357.6 | 516.6 | **1.049** |

**0.48–0.64 today → 1.03–1.06 predicted**, mean 1.044. The idle-subtraction fix is predicted to do what sec 4.2 of the previous report said it would, with a modest overshoot at `phase_pct=100`.

### 3.4 The failure mode that would have killed it — ruled out in emulation

The obvious risk of removing the subtraction is that an idle-but-unstolen vCPU's halt-exit latency starts clearing the 50 µs deadband and books phantom steal, which is the exact failure `nohz=off` was spent to eliminate. Measured on the clean, uncontended vCPUs (cpu8-15) in the same idle-heavy run:

| | emulated events, 21.9 s | emulated `pct=0` | emulated `pct=100` | **real** steal-page steal |
|---|---|---|---|---|
| cpu8-15 | **0–1** | 0.0–3.9 ms | 0.0–4.8 ms | 5.9–7.4 ms |

The emulated output on clean vCPUs is **below their genuine steal**, not above it. Under a genuinely runnable load (sec 3.2 conditions) the clean vCPUs emulate 0–27 events and 0.0–6.9 ms against 17–23 ms of real steal — again an undercount, not a phantom. **No phantom-inflation risk detected pre-reboot.** This still gets re-checked first post-reboot (sec 6.1), because an emulation is not an execution.

---

## 4. `phase_pct` recalibration — the direct answer

**The user's reasoning, restated:** `phase_pct` corrects a bias in `excess_c`; `excess_c = avail_c − idle_c − tick_c`; `idle_c` is what the fix touches; therefore the distribution of `excess_c` entering the correction may shift and the "1.000 tick per event" invariant — measured *with* the current idle behaviour — may not survive.

**That is exactly the right thing to worry about, and it happens not to bite, for a specific and checkable reason.**

### 4.1 Why the measured value is almost certainly unchanged

The `phase_pct=100` calibration was taken on **continuously-runnable** load, and on that load the idle delta is not merely small, it is **exactly zero**. From the raw per-CPU blocks of the previous session (`n_100_8.txt`, `n_0_8.txt`, `didle` column, ms):

```
N8 0 6502.7 3309.0 3307.7 1.0004 1144 4337 0 3190 3310
                                              ^ didle = 0
```

`didle == 0` on every contended vCPU, in every block, at every `phase_pct`. When `d_idle_c` is already 0, the line `if (!READ_ONCE(ivh_tks_idle_sub)) d_idle_c = 0;` is a **no-op** — literally the same arithmetic, the same `excess_c`, the same event set, the same 1.000. The finding is therefore **not** conditional on the idle subtraction, and sec 3.2 confirms this by re-deriving 0.999 through an emulator that has no idle term at all.

The mechanism also survives inspection: the 1-tick deficit comes from the **absolute periodic hrtimer** skipping missed expiries via `hrtimer_forward()` during a preemption burst, which is a property of the tick, not of the idle accounting. Nothing in `idle_sub` touches it.

**So: the number does not need re-fitting for the regime it was fitted on.**

### 4.2 Why re-validation is nevertheless mandatory

The fix does not change `phase_pct`'s value in the old regime; it changes **which regimes the estimator produces output in**. Two second-order effects are real and were measured in emulation:

1. **Event-count inflation.** `phase_pct` adds one whole tick *per event*, and events are intervals whose excess clears the deadband. With `idle_sub=1`, an idle interval's excess is pushed a full tick negative and can never be an event; with `idle_sub=0` it can. If idle intervals became events, `pct=100` would add 1 ms each and overshoot badly. **Measured in emulation: they do not.** Contended vCPUs emulate 159–232 events vs 147–219 counted live in the same window; clean idle vCPUs emulate 0–1. Inflation is negligible — but this is a *measured* result on one host under one corunner, and it is the first thing that could differ post-reboot.
2. **A different per-event deficit in the idle regime.** With `idle_sub=0` on idle-heavy load the per-event deficit emulates at **0.87 tick** (range 0.83–0.91 across the 8 vCPUs), not 1.00. Hence the ~4 % overshoot in sec 3.3. Plausible mechanism: on an idling vCPU a burst does not always straddle a tick deadline, so it does not always cost a full skipped tick.

**Verdict:** **likely NO recalibration of the value; definitely YES to re-validation, and a genuine possibility that the ideal value is regime-dependent** (exactly 100 on runnable load, ~85–90 on idle-heavy load). If the post-reboot numbers reproduce the emulation, the honest operating choice is to keep `phase_pct=100` and state the residual as "exact on runnable load, +3–6 % on idle-heavy load" — a 4 % overshoot is an enormously better error bar than today's 40 % undershoot, and a regime-dependent knob would be unjustifiable complexity for it.

**What must not happen:** assuming `phase_pct=100` still holds because it held before. It held before *for a reason that has been checked* (sec 4.1) — but the regime it now has to hold in is new, and sec 6.2 measures it rather than inheriting it.

---

## 5. Build, install, reboot — copy-pasteable

Matches this project's established process (`sudo make` / `modules_install` / `install`, from shell history and `experiment_plan.md` sec 13). **Run these yourself. Nothing below has been executed.**

### 5.1 Bump the local version (so the running kernel is identifiable and the old one stays bootable)

```bash
cd /home/nick/kernels/linux-6.17-rseqport
./scripts/config --set-str LOCALVERSION "-rseqport73-idlesub"
grep '^CONFIG_LOCALVERSION=' .config          # expect: CONFIG_LOCALVERSION="-rseqport73-idlesub"
```

### 5.2 Build

```bash
cd /home/nick/kernels/linux-6.17-rseqport
sudo make -j$(nproc) 2>&1 | tee /home/nick/build.log
grep -i "error:" /home/nick/build.log | head -20     # expect: no output
```

(≈66 MB of modules and ~12 GB free on `/`; space is fine. The `kernel/sched/*.o` objects are currently owned by `nick` from the incremental compile checks while `vmlinux` is owned by `root` from previous `sudo make` runs — that mix is normal here and `sudo make` relinks over it without complaint.)

### 5.3 Install

```bash
cd /home/nick/kernels/linux-6.17-rseqport
sudo make modules_install
sudo make install
```

`make install` writes `/boot/vmlinuz-6.17.0-rseqport73-idlesub+` + initrd and regenerates the GRUB menu itself. **No manual GRUB editing is needed or wanted:** `/etc/default/grub` already carries `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash nohz=off"`, so the new kernel inherits `nohz=off` — which `ivh_tks_idle_sub=0` **requires**.

### 5.4 Verify before rebooting

```bash
ls -la /boot/vmlinuz-6.17.0-rseqport73-idlesub+ /boot/initrd.img-6.17.0-rseqport73-idlesub+
sudo grep -c "6.17.0-rseqport73-idlesub" /boot/grub/grub.cfg      # expect >= 1
grep GRUB_CMDLINE_LINUX_DEFAULT /etc/default/grub                 # must still contain nohz=off
```

### 5.5 Select it for one boot and reboot

```bash
sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 6.17.0-rseqport73-idlesub+"
sudo reboot
```

### 5.6 First four commands after it comes up

```bash
uname -r                                              # 6.17.0-rseqport73-idlesub+
cat /proc/cmdline | tr ' ' '\n' | grep nohz           # nohz=off
cat /proc/sys/kernel/ivh_tks_idle_sub                 # 1   <-- the whole point of the reboot
ls /proc/sys/kernel/ | grep -c ivh                    # 60 (was 59)
```

If `ivh_tks_idle_sub` is still missing, the new kernel did not boot — check `uname -r` before doing anything else.

### 5.7 Bring IVH up, then the two operating-point changes

```bash
pkill -f MY_ivh_atc ; sleep 1                         # never run two ATCs (see MEMORY.md)
nohup /home/nick/IVH >/tmp/ivh_launch.log 2>&1 &
sleep 20 ; pgrep -a -f 'MY_ivh_atc|vcap_probe'
```

`/home/nick/IVH` sets `ivh_tks_phase_pct 0` (line 93, with a comment this project has since overturned) and knows nothing about `idle_sub`. Apply the operating point by hand for the test, and only edit the script once sec 6 has confirmed it:

```bash
sudo sysctl -w kernel.ivh_tks_phase_pct=100
sudo sysctl -w kernel.ivh_tks_idle_sub=0        # ONLY valid together with nohz=off
```

---

## 6. Post-reboot validation checklist

Tooling is already in place in
`/tmp/claude-1000/-home-nick-kernels-linux-6-17-rseqport/4ab2961e-3163-49c1-b957-e10d703a5ef1/scratchpad/`:
`snap.bt` / `snap.sh` (per-CPU `ivh_tks_*` + `kvm_steal_time` page), `delta.sh` (window differencing; columns: *label cpu window_ms tks_ms realsteal_ms ratio events samples idle_ms busy_ms statsteal_ms*), `spin.sh`, `meas_coarse.sh`, `hbsweep.sh`, `idlesub_emul.bt`. Confirm the host corunner is still contending cpu0-7 (`corun_check.sh`) before trusting any block.

**6.1 — Clean-vCPU phantom, at `idle_sub=0`. Do this FIRST.** Idle guest, corunner on, 20 s, `idle_sub` 1 → 0 and `phase_pct` 0 → 100 (4 cells). Read cpu8-15. **Pass:** `tks_ms` on cpu8-15 stays at or below their steal-page steal (emulation predicts 0.0–4.8 ms against 5.9–7.4 ms real, 0–1 events). **Fail:** phantom appears — do not proceed; the lever is `ivh_tks_deadband_ns` (raise it), **not** `phase_pct`. This is the failure mode `nohz=off` was spent to remove and it outranks every accuracy result below.

**6.2 — Re-measure the deficit/event ratio in BOTH regimes. Do not inherit `phase_pct=100`.** Use the identical methodology of `ivh_undershoot_correction_2026-08-09.md` sec 3: bracket a window with `snap.sh`, difference with `delta.sh`, compute `(real_steal − tks) / events` in ticks.
  - **(a) Continuously runnable** (`meas_coarse.sh`, 8 spinners, ≥3 reps): with `idle_sub=0` this must still read **1.000 tick/event** and `phase_pct=100` must still read **0.998–1.001**. Confirm `didle == 0` in the output — that is the guarantee that the two `idle_sub` settings are computing the same thing, and if `didle != 0` the sec 4.1 argument does not apply to that block and the block should be discarded.
  - **(b) Idle-heavy** (no guest load, corunner on, ≥3 reps): expect deficit/event ≈ **0.87 tick** and `phase_pct=100` ≈ **1.03–1.06**. If it lands materially above ~1.10, `phase_pct` is over-correcting in this regime and the event count is the thing to look at (compare `events` at `idle_sub=1` vs `0` in the same window — emulation says they barely differ).
  - **(c)** Only if (a) and (b) disagree by more than the ±0.05 that sec 3.3's window-scaling error can explain should a `phase_pct` other than 100 be considered, and then the honest report is a regime-dependent residual, not a re-tuned constant.

**6.3 — The headline claim: hackbench recovery.** `hbsweep.sh`, ≥3 reps, `idle_sub` 1 vs 0 at `phase_pct=100`. The prediction on record is 0.24–0.44 → toward 1.0. Capture `idle_ms` per window and re-check the previous report's residual identity: at `idle_sub=1` the residual should still be 0.76–0.91 × idle; at `idle_sub=0` that term should be **gone**, not merely smaller. That is the direct test of the mechanism, and it is the one measurement that converts "predicted, untested" into a result.

**6.4 — Runtime switchability / no discontinuity.** Flip `idle_sub` 1 ↔ 0 mid-window under steady load and confirm `rq->ivh_tks_steal_ns` stays monotonic with no step or backward move (`get_steal_and_preemptions()`'s contract — a single backward step underflows a `u64` in NHextend's userspace delta). The seeding of `prev_idle_ns` on both paths is what should make this hold; verify it rather than trusting it.

**6.5 — Gate quality, measured not predicted.** `gate.sh` at `idle_sub` 1 vs 0: `ivh_uc_capacity` distribution on clean vs contended vCPUs, disagreement rate against the 1010 threshold, and IVH-on hackbench wall time. Per sec 2.6 the effect should be **small**, because `ivh_uc_tick()`'s own idle subtraction and its `min(d_steal_c, avail_c)` clamp stand between the fix and the capacity signal. A large change here is a surprise worth chasing, not a win to bank.

**6.6 — Sanity floor.** `rq->ivh_tks_skipped` should not grow after boot (it would mean the `tick_c < 1000` miscompile floor is firing), and cpu8-15 should tick ~1000/s under `nohz=off`.

---

## 7. Confidence

| claim | verdict | confidence |
|---|---|---|
| `ivh_tks_idle_sub` is NOT live on `…rseqport72-plzwork+`; reboot genuinely required | **True** | **High** — `/proc/sys/kernel/ivh_tks_idle_sub` absent, symbol absent from running binary |
| The tree code is the complete fix described in sec 7 of the previous report | **True** | **High** — all three pieces present, disassembly byte-identical after recompile |
| A blunt `d_idle_c = 0` is the correct shape; no scaled/partial variant is needed | **True** | **High** — exact on all four interval types, and emulation lands at 1.00–1.06 |
| `sched.h` was incomplete (missing extern, stale `phase_pct` doc) and is now finished | **True** | **High** — compiled, no codegen change |
| `phase_pct=100` is unaffected on continuously-runnable load | **True** | **High** — `didle ≡ 0` in the source blocks, so the fix is a provable no-op there; independently re-derived at 0.999 by an emulator with no idle term |
| `idle_sub=0` takes idle-heavy recovery from ~0.5 to ~1.0 | **Predicted** | **Moderate-high** — emulator validated to 0.07 % against the live kernel on the regime where they must agree; still an emulation, not an execution |
| `phase_pct=100` slightly overshoots (~4 %) on idle-heavy load | **Predicted** | **Moderate** — 8 vCPUs, one window, and a 3.8 % window-scaling correction sits inside the effect |
| `idle_sub=0` does not reintroduce clean-vCPU phantom | **Predicted** | **Moderate** — emulated output is *below* real steal on clean vCPUs in two loads; must be re-checked first post-reboot |
| The fix will not move `ivh_uc_capacity` much | **Predicted** | **Moderate** — follows from the `min(d_steal_c, avail_c)` clamp by inspection; not measured |

---

## 8. Exact state left on the machine

- **No reboot, no `make install`, no `make modules_install`, no GRUB change, no commit, no push.** Running kernel is still `6.17.0-rseqport72-plzwork+`.
- **`kernel/sched/sched.h` edited** (sec 2.4): `extern unsigned long ivh_tks_idle_sub;` + doc, and the stale `phase_pct` doc block corrected. `kernel/sched/core.c` **not** modified by this session. `make kernel/sched/` clean; `core.o`/`fair.o`/`built-in.a` rebuilt and now newer than the installed kernel (normal, inert for the running system).
- **Sysctls unchanged**, still the `/home/nick/IVH` canonical set: `ivh_tks_phase_pct=0`, `carry_ticks=8`, `deadband_ns=50000`, `steal_source=2`, `cap_source=3`, `universal_eligible=1`, `capacity_threshold=1010`. **`phase_pct=100` was again deliberately NOT left set.**
- **Daemons untouched:** the pre-existing `MY_ivh_atc` and `vcap_probe -p 200 -s 5000` were running at session start and still are. Transient load created and cleaned up: eight `spin.sh` instances (32 s) on cpu0-7, all exited.
- **`/home/nick/IVH`, `/home/nick/ivh_mode.sh`, `/home/nick/ivh_verify.sh`, `/etc/default/grub`, `tools/bpf/MY_ivh_atc.bpf.c` not touched.**
- New scratchpad tool: `idlesub_emul.bt` (sec 3.1), plus raw blocks `idle{0,1}.txt`, `e{0,1}.txt`, `r{0,1}.txt`, `emul_idle.txt`, `emul_run.txt`.
