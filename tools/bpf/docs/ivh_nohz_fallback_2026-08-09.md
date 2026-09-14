# The `nohz=off` companion fix: kcpustat idle fallback written and compiling, boot parameter checked safe for this exact build

**Date:** 2026-08-09 (session ~03:20–04:10 wall, immediately following `ivh_solution_search_2026-08-09.md`)
**Kernel tree:** `/home/nick/kernels/linux-6.17-rseqport`, branch `kernel-43-clean`, base commit `6c3874293`
**Kernel live during this work:** `6.17.0-rseqport71-byeunhalt+` (unchanged — no reboot, no rebuild, no install)
**Status:** code written, compiles clean, **not executed**. Config safety check performed against the real files on this machine. Nothing committed, nothing pushed, no GRUB file edited.

---

## 0. The four answers, up front

### (1) The fallback is written and it compiles

`kernel/sched/core.c`, working-tree edit, uncommitted. Five hunks:

| # | new line | function | what changed |
|---|---|---|---|
| — | **`core.c:507–606`** | **`ivh_idle_ns(int cpu)`** — **new** | Single shared idle source for all four estimators. Returns `get_cpu_idle_time_us() + get_cpu_iowait_time_us()` scaled to ns when `tick_nohz_active`; returns `kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE] + [CPUTIME_IOWAIT]` when it is not. ~90 lines of that range is the rationale comment. |
| 1 | `core.c:904–917` | `ivh_ref_accumulate()` | was `core.c:827` `rq->ivh_ref_skipped++; return;` → now `idle_ns = ivh_idle_ns(cpu);`. Dropped now-unused `idle_us`/`iowait_us` from the declaration at `core.c:830`. |
| 2 | `core.c:1896–1927` | `ivh_vact_idle_delta_c()` | was `core.c:1845` `return U64_MAX;` → now `idle_ns = ivh_idle_ns(cpu_of(rq));`. `U64_MAX` kept as a contract but is now unreachable; the caller's disarm handling at `core.c:1994` is deliberately retained. |
| 3 | `core.c:2321–2335` | `ivh_tick_steal_accumulate()` | was `core.c:2244` `rq->ivh_tks_skipped++; return;` → now `idle_ns = ivh_idle_ns(cpu);`. **This is the site the whole exercise is for.** |
| 4 | `core.c:2690–2700` | `ivh_uc_tick()` | was `core.c:2610` `rq->ivh_uc_skipped++; return;` → now `idle_ns = ivh_idle_ns(cpu);`. |

Plus one comment-only correction in `kernel/sched/sched.h:1675–1690` (the `ivh_vact_prev_idle_ns` field comment named `get_cpu_idle_time_us` as "the source" and asserted it is microseconds; it is now either source, and the field's nanosecond unit is what makes them interchangeable).

**The task's list of three was not complete.** There were **four** `!tick_nohz_active` bail-outs, not three: `ivh_ref_accumulate()` at `core.c:827` was the missed one. I swept the whole tree — `grep -rn 'get_cpu_idle_time_us\|get_cpu_iowait_time_us\|tick_nohz_active' kernel/sched/ arch/x86/kernel/` — and after the change the only remaining hits outside `ivh_idle_ns()` are prose in comments (`fair.c:14510`, `sched.h:1681`). There are no others.

Compile check (incremental, against this tree, `make kernel/sched/`):

```
CC      kernel/sched/core.o
CC      kernel/sched/fair.o
CC      kernel/sched/cputime.o
CC      kernel/sched/bpf_sched.o
CC      kernel/sched/build_policy.o
CC      kernel/sched/build_utility.o
AR      kernel/sched/built-in.a
```

Zero warnings, zero errors. **Disassembly-verified as well**, because this project has been bitten by a miscompile on exactly this kind of arithmetic before (`OPTIMIZER_HIDE_VAR` in `<asm/ivh_tsc_beat.h>`):

- `objdump` of `ivh_idle_ns` shows both arms present. Fast arm: two calls, two `cmp $-1`, `add`, `imul $0x3e8` (×1000, µs→ns). Fallback arm: `__per_cpu_offset[cpu]` load then **`0x28(%r14)` + `0x30(%r14)`** — byte offsets 40 and 48, i.e. `cpustat[5]` and `cpustat[6]`, i.e. exactly `CPUTIME_IDLE` and `CPUTIME_IOWAIT` per `enum cpu_usage_stat` in `include/linux/kernel_stat.h`. Correct indices, no aliasing.
- `objdump` of `ivh_tick_steal_accumulate` shows `call ivh_idle_ns` in place and the `TICK_NSEC` conversion still compiling to `mov $0xf4240,%r10d / mul %rdi / div %r10` with the `cmp $0x3e7` sanity floor intact — the old miscompile has **not** returned.

### (2) Safety verdict on `nohz=off` for this exact build: **SAFE, with two behavioural caveats that are not safety issues**

Checked against `/boot/config-6.17.0-rseqport71-byeunhalt+`, which I confirmed is **byte-identical to the tree's `.config`** (`diff` returned 0).

| option | actual value | consequence |
|---|---|---|
| `CONFIG_NO_HZ_COMMON` | **=y** | `nohz=` is a live, supported parameter (`__setup("nohz=", setup_tick_nohz)`, `kernel/time/tick-sched.c:683`). |
| `CONFIG_NO_HZ_IDLE` | **not set** | **The report's premise needs correcting on a detail.** This kernel is *not* `NO_HZ_IDLE`. |
| `CONFIG_NO_HZ_FULL` | **=y** | This is the tick mode actually compiled in. |
| `CONFIG_NO_HZ` | =y | legacy alias, informational. |
| `CONFIG_HZ_PERIODIC` | not set | as expected. |
| `CONFIG_HZ` / `CONFIG_HZ_1000` | **1000** / =y | unchanged by `nohz=off` — see below. |
| `CONFIG_HIGH_RES_TIMERS` | =y | fine — see below. |
| `CONFIG_TICK_ONESHOT` | =y | fine. |
| `CONFIG_RCU_NOCB_CPU` | =y | inert: `CONFIG_RCU_NOCB_CPU_DEFAULT_ALL` is **not set** and there is no `rcu_nocbs=` on the cmdline. |
| `CONFIG_IRQ_TIME_ACCOUNTING` | **not set** | so `account_process_tick()`'s `irqtime_account_process_tick()` branch is dead and the plain `steal → user/system/idle` split is the live one. Relevant: it is the path the fallback's exactness argument depends on. |
| `CONFIG_VIRT_CPU_ACCOUNTING_GEN` | =y | selected by `NO_HZ_FULL`; inert because no CPU is nohz_full, so `vtime_accounting_enabled_this_cpu()` is false everywhere. |
| `CONFIG_PARAVIRT_TIME_ACCOUNTING` | **not set** | only gates `paravirt_steal_rq_enabled`; `paravirt_steal_enabled` is inc'd unconditionally by `arch/x86/kernel/kvm.c:1043` when `has_steal_clock`, which is why `/proc/stat` steal works. |

**`NO_HZ_FULL=y` instead of `NO_HZ_IDLE` does not change the verdict, and I checked why rather than assuming.** `/proc/cmdline` on this machine is:

```
BOOT_IMAGE=/boot/vmlinuz-6.17.0-rseqport71-byeunhalt+ root=UUID=9180341d-… ro quiet splash crashkernel=2G-4G:320M,… vt.handoff=7
```

There is **no `nohz_full=`, no `isolcpus=`, no `rcu_nocbs=`, and no existing `nohz=`.** With no `nohz_full=`, `tick_nohz_full_running` stays false, `tick_nohz_init()` returns immediately, and the full-dynticks machinery is entirely dormant — runtime behaviour is identical to `NO_HZ_IDLE`. So the mechanism `ivh_solution_search` characterised is the right mechanism; only the config symbol name in that report is off. And the one place the two features could collide, `tick_sched_do_timer()`'s `WARN_ON_ONCE(tick_nohz_full_running)` at `tick-sched.c:224`, is doubly unreachable here: it needs `tick_do_timer_cpu == TICK_DO_TIMER_NONE`, which is only ever assigned when a tick is *stopped*, which `nohz=off` prevents outright.

**`nohz=off` is a real override on this build, not a no-op.** Traced end to end: `setup_tick_nohz()` clears `tick_nohz_enabled` → `tick_nohz_activate()` (`tick-sched.c:1491`) returns early → `TS_FLAG_NOHZ` is never set on any CPU and `tick_nohz_active` stays 0 → `can_stop_idle_tick()` (`tick-sched.c:1173`) returns false unconditionally → the tick is never stopped, on any CPU, ever. `tick_nohz_switch_to_nohz()` (the low-res path) also returns early at `tick-sched.c:1506`.

**`CONFIG_HIGH_RES_TIMERS=y` does not defeat it.** `tick_setup_sched_timer()` arms `ts->sched_timer` as an `HRTIMER_MODE_ABS_PINNED_HARD` hrtimer forwarded by exactly `TICK_NSEC`, and only *then* calls `tick_nohz_activate()`. With `nohz=off` the timer is armed and the activation is skipped, so what you get is a genuine periodic 1000 Hz tick delivered by hrtimer — including on idle CPUs, which is the entire point.

**`CONFIG_HZ` is untouched and nothing downstream shifts.** `nohz=off` gates *tick stopping*, never the tick *rate*; `HZ` and `TICK_NSEC` are compile-time constants at 1000 / 1 000 000 ns either way. I grepped every `TICK_NSEC` consumer in the IVH code: `core.c:1344/1381/1391` (sysctl range floors), `core.c:1996` and `core.c:2269+` (the `ivh_tsc_ns_to_cycles(TICK_NSEC)` tick-period constants in `ivh_vact_tick()` and `ivh_tick_steal_accumulate()`), plus `core.c:359`'s `ivh_ka_probe_ns` comment. All are HZ-derived compile-time values; none of them changes. The report's ladder measurements and the estimator's per-tick expectation stay valid.

**Two behavioural caveats — real, not safety problems, but they mean a post-reboot A/B is not a single-variable comparison:**

1. **`is_cpu_preempted()`'s idle blind spot closes.** `account_process_tick()`'s own comment (`cputime.c`) records that it "does not run on a CPU in nohz idle … so an idle vCPU's stamp ages without bound and reads as preempted", and `tools/bpf/MY_ivh_atc.bpf.c:185` carries the `max(last_idle_tp, clock_preempt)` workaround for it. Under `nohz=off` both `clock_preempt` and `last_idle_tp` refresh every millisecond on idle CPUs, so idle vCPUs stop reading as preempted. That is an *improvement*, but it changes gate behaviour independently of the estimator fix.
2. **~1000 timer interrupts/s per idle vCPU × 16.** The vCPU still `HLT`s between ticks — unlike the `SCHED_IDLE` filler of `ivh_solution_search` sec 4.1/4.2, `idle_cpu()` stays true and `select_idle_sibling()`'s fast path is untouched, which is precisely why this is the cheap cure — but the maximum halt residency drops from unbounded to ~1 ms. Whether the host's sleeper bonus survives that is **unknown and unmeasurable from inside the guest**; it is the main empirical risk of spending the reboot.

### (3) Exact GRUB commands for this machine

Ubuntu 25.04. `/etc/default/grub` currently has `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"` and `GRUB_CMDLINE_LINUX=""`; `/etc/default/grub.d/kdump-tools.cfg` appends the `crashkernel=` string, which is why `/proc/cmdline` shows it. `GRUB_DEFAULT=saved` with `GRUB_SAVEDEFAULT=true`, and `/boot/grub/grubenv` has `saved_entry=…gnulinux-6.17.0-rseqport71-byeunhalt+-advanced-…`, so a plain reboot returns to the same kernel entry.

> **DO THE REBUILD FIRST.** The installed `6.17.0-rseqport71-byeunhalt+` binary does **not** contain the fallback. Booting *that* binary with `nohz=off` is the exact silent-failure this document exists to prevent: all four estimators bail on every tick, `ivh_uc_capacity` freezes, and IVH quietly stops working. Rebuild + install, then add the parameter — or do both before one reboot.

**Option A — one-shot test, nothing on disk changes (recommended for the first boot).** At the GRUB menu press `e` on the `6.17.0-rseqport71…` entry, find the line starting `linux /boot/vmlinuz-…`, append ` nohz=off` at its end, then `Ctrl-X` to boot. Any subsequent reboot reverts automatically. No file is edited, nothing to undo.

**Option B — persistent.** Edit `/etc/default/grub` (**your action, not mine — I did not touch it**), changing exactly one line:

```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
```
to
```
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash nohz=off"
```

then:

```sh
sudo update-grub
grep -m1 nohz=off /boot/grub/grub.cfg && echo "OK: nohz=off is in grub.cfg"   # needs sudo to read grub.cfg
sudo reboot
```

(`update-grub` is present at `/usr/sbin/update-grub`; it is the Ubuntu wrapper for `grub-mkconfig -o /boot/grub/grub.cfg`, also present at `/usr/sbin/grub-mkconfig`. Use `update-grub` — it is what `/etc/default/grub`'s own header comment instructs.)

Note that `GRUB_CMDLINE_LINUX_DEFAULT` applies to **every** kernel entry, so a fallback boot into an older kernel would also get `nohz=off` — and no older kernel has the fallback code, so IVH would be silently inert there. Reverting is the same edit backwards plus `sudo update-grub`.

**To undo without editing anything:** at the GRUB menu press `e` and delete ` nohz=off` from the `linux` line for that one boot.

### (4) What remains unverified until the reboot — stated plainly

This is preparation, not proof. Specifically **not** established:

- **The fallback code has never executed.** Not one instruction of the `kcpustat` arm has run. It compiles and disassembles correctly; that is all. On the current NOHZ boot the fast arm is taken on every tick and the fallback is dead code.
- **The composite has never been run.** `ivh_solution_search` sec 11 rates "`nohz=off` + the `kcpustat` idle fallback would fix the estimator" as *predicted, untested, moderate confidence*. Nothing here raises that. The mechanism is established and sec 4.1 proved the "ticks ⇒ no phantom" half empirically; the other half — that a periodic tick plus a kcpustat idle source produces a clean signal at acceptable throughput cost — is exactly what the reboot is for.
- **Reading a config file is not the same as booting.** I traced `nohz=off` through `tick-sched.c` and confirmed every gate it passes through, but a boot parameter that is correct on paper can still surprise on a VM.
- **The throughput cost of 16 kHz of extra guest timer interrupts is unmeasured**, as is whether losing unbounded halt residency costs the host's sleeper bonus. Caveat 2 above.
- **`ivh_solution_search` sec 9 still stands and is the larger problem.** The host's contention character moved the IVH-off baseline from 14.715 s to 25.092 s within one session while the corunner check moved <5 points, with the uncontended-only control flat at 10.877 → 10.854 s. **Any before/after comparison across this reboot is subject to that same confound**, and it was larger than the effect under study. Take the IVH-off baseline and the `taskset -c 8-15` guest-health control *inside the same post-reboot batch*, not against tonight's numbers.
- **Two behavioural changes ride along** (caveats 1 and 2 in sec 2 above), so a post-reboot A/B is not single-variable.
- **`ivh_tks_skipped` is not observable.** `get_tks_compare()` still has no in-tree caller, so there is no `/proc/ivh_debug` line for it. Verification has to go through `ivh_uc_cpu:`.

---

## 1. Why the fallback is *better* than the NOHZ source once the tick is periodic — re-derived, not taken on trust

The task said to re-verify the steal-taint finding rather than accept it. I did, from source, and it holds — with one addition the original trace did not mention.

**`account_steal_time()` (`kernel/sched/cputime.c:213`) writes `cpustat[CPUTIME_STEAL]` and nothing else.** Three lines, no branches. **`account_idle_time()` (`cputime.c:222`) has no `CONFIG_PARAVIRT` guard anywhere in its body** — it splits into `CPUTIME_IOWAIT` or `CPUTIME_IDLE` on `rq->nr_iowait`, updates `rq->last_idle_tp` and `rq->ivh_vact_idle_exit_tsc`, and returns. Neither function can put steal into the idle buckets. Confirmed.

**The addition:** `steal_account_process_time()` runs *before* the idle/user/system split in `account_process_tick()` and consumes the tick's budget —

```c
cputime = TICK_NSEC;
steal = steal_account_process_time(ULONG_MAX);
if (steal >= cputime) return;
cputime -= steal;
…
else account_idle_time(cputime);
```

So a partially-stolen idle tick books **less than `TICK_NSEC`** of idle. That is not taint; it is the correct direction, and it is what makes the fallback *exact* rather than merely adequate: **idle + user + system + steal sums to exactly `TICK_NSEC` per tick, with no residue**, because every tick charges exactly one `TICK_NSEC` and `account_process_tick()` runs on every CPU on every tick once the tick is periodic. The 28 %-of-wall accounting hole measured in `ivh_solution_search` sec 3.3 — 7 200 ms accounted out of a 10 025 ms window — is a NOHZ artefact of `account_idle_ticks()`'s catch-up path, and it cannot exist when there is no catch-up path. The "Trap 2" objection to `kcpustat[CPUTIME_IDLE]` that `ivh_ref_accumulate()` has carried since the TSC work is a statement about NOHZ, and it expires with NOHZ.

Two further properties the callers actually depend on, both of which the fallback has and the NOHZ source does not:

- **Monotonic.** `kcpustat` counters are only ever incremented. Every caller's `idle_ns > prev ? idle_ns - prev : 0` clamp therefore **never fires** on this source. That matters more than it looks: those clamps exist because `get_cpu_idle_time_us()`'s own kerneldoc admits to backward steps, and an asymmetrically-firing clamp is precisely the rectifier that `ivh_ref_carry` and `ivh_tks_carry_c` were built to undo (`ivh_ref_accumulate()`'s comment, and `ivh_solution_search` sec 5.3's measurement that the bounded carry is load-bearing). **This change introduces no new rectifier — it removes the possibility of one on the idle term.**
- **In-phase with `used_ns`.** In `ivh_uc_tick()` the WALL numerator's idle term and the ACCT numerator's `used_ns` become two `kcpustat` accumulators read at the same instant on the same tick, carrying the identical one-tick lag, for the first time.

### The one honest imperfection: a bounded one-tick phase error

All four hooks run at the **top** of `account_process_tick()`, before it books the current tick, so the value read at tick *n* reflects charges through tick *n−1* while the `rdtsc()` taken alongside it is current. Under a periodic tick this is a pure one-tick shift of a series sampled at the same rate: consecutive deltas have identical length and the shift cancels at every window boundary except the first and last tick. It is the same lag `ivh_uc_tick()` already documents for `used_ns` and calls "a documented, self-cancelling 0.5 % phase error at the default 200-tick window". It does **not** accumulate and it does **not** bias.

Sanity-check on the sign, since this is where a bias would hide. With a periodic tick and no steal, `elapsed_i = TICK`, so

```
excess_i = avail_i − TICK = (TICK − d_idle_i) − TICK = −d_idle_i  ≤ 0
```

for every interval. The estimator's excess is non-positive on any unpreempted vCPU regardless of load shape — which is exactly the structural property `ivh_solution_search` sec 8 predicted `nohz=off` would restore, and it is what makes the phantom impossible rather than merely small. Steal appears only as `elapsed_i = TICK + S_i`, giving `excess_i = S_i − d_idle_i`. The bounded negative carry (`ivh_tks_carry_ticks=8`, 8 ms) behaves as it does today.

Moving the hooks later to remove the lag is **not** an option and the existing comments say why: it would put them after `vtime_accounting_enabled_this_cpu()`'s early return, which is the NOHZ hole the placement exists to avoid.

### The one source transition, and why it is not a problem

`tick_nohz_active` goes 0 → 1 exactly once per boot (in `tick_nohz_activate()`, when the tick device goes high-res) and never back; under `nohz=off` it never moves at all. On a *normal* NOHZ boot the series therefore switches once, from the `kcpustat` value (which has been accumulating boot idle) to the NOHZ value (which starts near zero) — i.e. it steps **backward**, i.e. every caller's non-negative clamp turns it into one delta of zero. Cost: one tick, on one CPU, whose idle is not subtracted, at a point in boot where the tick is still periodic so that tick's raw-TSC gap is one nominal tick and the resulting excess is ≈0. Bounded to one tick, once, per CPU, per boot, absorbed by the signed carry.

And it replaces the old behaviour, which was to produce **no signal at all** until that moment. On the `nohz=off` boot this transition does not occur.

---

## 2. Post-reboot verification recipe

Run these **before** trusting any benchmark number, in this order.

**Step 1 — the parameter took.**
```sh
grep -o 'nohz=off' /proc/cmdline || echo "FAIL: nohz=off not on the cmdline"
```

**Step 2 — the tick is genuinely periodic on idle CPUs.** This is the decisive check, and it is the same instrument `ivh_solution_search` sec 3.3 used. With the guest idle:
```sh
a=$(awk '/^LOC/{for(i=2;i<=NF;i++) s+=$i; print s}' /proc/interrupts); sleep 10
b=$(awk '/^LOC/{for(i=2;i<=NF;i++) s+=$i; print s}' /proc/interrupts)
echo "LOC/s across all 16 vCPUs: $(( (b-a)/10 ))    # expect ~16000, i.e. ~1000/cpu"
```
On the current NOHZ boot this reads far below 16 000 at idle. If it does not jump to ~16 000, `nohz=off` did not take effect and nothing below is meaningful.

**Step 3 — the fallback is being *taken*, not bailed past.** `ivh_uc_cpu:`'s legend (`/proc/ivh_debug` line 135) is
`cpu vcap_custom vcap_cpu_capacity uc_capacity uc_wall uc_acct raw_wall raw_acct win_avail_c win_stolen_c windows extended skipped vact_capacity`,
so `skipped` is awk `$14` and `windows` is `$12`:
```sh
grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '{print $2, $12, $14}' > /tmp/uc.a; sleep 10
grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '{print $2, $12, $14}' > /tmp/uc.b
paste /tmp/uc.a /tmp/uc.b | awk '{printf "cpu%-3s windows +%-8d skipped +%d\n", $1, $5-$2, $6-$3}'
```
**`windows` must climb and `skipped` must not.** A climbing `skipped` with a frozen `windows` is the exact silent-failure mode — it means the running binary lacks the fallback (wrong kernel booted).

**Step 4 — the actual result the reboot is for.** Repeat `ivh_solution_search` sec 3.1 row D: full unpinned hackbench, sampling `raw_wall` (awk `$8`) on the clean vCPUs 8–15.
```sh
/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000 &
for i in $(seq 40); do
  grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '$2>=8 {s+=$8; n++} END{printf "clean raw_wall %.1f\n", s/n}'
  sleep 0.1
done
```
Baseline to beat: **666.7 – 709.2** under NOHZ. The prediction is **≥1000**, i.e. the separation from the contended population moving from ~155 to the ~530 that sec 4.1 achieved with the (unshippable) continuous filler. Anything in between is a partial result and should be reported as one.

**Step 5 — guard against sec 9.** Take the IVH-off baseline and the `taskset -c 8-15` guest-health control **inside the same batch**, never against tonight's numbers:
```sh
taskset -c 8-15 /home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l 400000   # guest health, expect ~10.9 s
```

If step 4 succeeds and step 5's control is ~10.9 s, that is the first reproduced fix this line of work has produced. If step 4 fails while steps 1–3 pass, the mechanism in `ivh_solution_search` sec 3 is wrong or incomplete, and that is a real finding too.

---

## 3. Exact state left on the machine

- **No reboot. No rebuild of the full kernel, no `make modules_install`, no `make install`.** The live kernel is still `6.17.0-rseqport71-byeunhalt+` and its binary is unchanged.
- **`kernel/sched/core.c` and `kernel/sched/sched.h`:** modified in the working tree, uncommitted, alongside the pre-existing uncommitted work from earlier sessions. **Nothing committed, nothing pushed.**
- **`/etc/default/grub`, `/etc/default/grub.d/*`, `/boot/grub/grubenv`: not touched.** Inspected read-only. No `update-grub` was run.
- **`tools/bpf/MY_ivh_atc.bpf.c`: not touched.** No BPF rebuild, no reload.
- **`/home/nick/IVH`, `/home/nick/ivh_verify.sh`: not touched.**
- **Sysctls: not touched.** No process started or killed.
- Object files under `kernel/sched/` are newer than the installed kernel, as a consequence of the compile check. That is the normal state after an incremental verification and has no effect on the running system.
- New file: this document.
