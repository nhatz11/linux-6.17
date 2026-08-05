# vcap retirement — design and build plan for an in-kernel `used/(used+stolen)` capacity signal

**Date:** 2026-08-03
**Tree:** `/home/nick/kernels/linux-6.17-rseqport`, branch `kernel-43-clean`, HEAD `a0bfc3784`
**Status:** PLAN ONLY. No source touched, nothing built, no git operations. Awaiting greenlight.
**Supersedes/refines:** §6.5 (P0a–P3) of `ivh_tsc_final_state_report_2026-08-02.md`.

---

## 0. Executive summary

The third and last real-steal-time consumer is the capacity input to Gate 1
(`ivh_steal_imminent()`, `kernel/sched/fair.c:13365`) and to the BPF destination scan
(`process_cpu()`, `tools/bpf/MY_ivh_atc.bpf.c:304`). Both read a number that today comes from the
`vcap` daemon.

Neither of vcap's two inputs is information the kernel lacks. `stolen_time` is already
`rq->ivh_ref_steal_ns` (TSC/REF_TSC-derived, `kernel/sched/core.c:590-836`). `used_time` is already
kcpustat, and the wall-clock/idle series it is derived from is already read twice per tick by
`ivh_ref_accumulate()`. The only thing vcap owns is the **transformation**, and — this is the finding
that shapes the whole plan — one *semantic* property of how it takes its measurement that is easy to
miss from the formula alone.

This plan adds **one new per-rq signal, `rq->ivh_uc_capacity`** (uc = *used capacity*), computed by a
new `ivh_uc_tick()` in `kernel/sched/core.c`, hooked from `account_process_tick()`. It is a
**continuously accumulated, idle-excluded, EMA-smoothed** ratio on the 1024 scale, deliberately built
to land on vcap's existing distribution so `IVH_CAP_FLOOR = 850` and `ivh_capacity_threshold = 1010`
transfer unchanged.

It is **not** `rq->ivh_vact_capacity` and does not touch it. Part C stays exactly as it is.

**One kernel build + reboot. One BPF recompile (after that boot). Everything else is sysctl and
`bpftool map update`.**

---

## 1. What vcap actually does — verified against the source, with two corrections to the briefing

Read: `/home/nick/vsched_main/vcapacity/main.cpp` (902 lines), and the live launch line in
`/home/nick/IVH:52`.

### 1.1 The launch parameters are not the defaults

```
cd /home/nick/vsched_main/vcapacity && sudo ./vcap -p 200 -s 5000 &
```

`-p` is `profile_time` and `-s` is `sleep_length` (`main.cpp:285-286`). So the live cadence is:

| quantity | code | live value |
|---|---|---|
| measurement window | `profile_time` | **200 ms** |
| idle gap between windows | `sleep_length` | **5000 ms** |
| full loop period | sum | **~5.2 s** |
| EMA decay length | `decay_length` (`-d`, default) | **2 samples** → half-life ≈ **10.4 s** |

**Correction 1 to the briefing.** vcap does *not* "poll every 200 ms and accumulate over repeated
5-second windows". It takes **one 200 ms snapshot every 5.2 s** and throws the other 5 s away
(`do_profile()`, `main.cpp:659-751`: `sleep_for(sleep_length)` → snapshot begin → `sleep_for(profile_time)`
→ snapshot end). This matters: the kernel replacement has no reason to duty-cycle, and if it does
not, it will have **26× more data per unit time** than vcap. That is a strict improvement but it is
also a source of structured, expected divergence in the shadow comparison, and the plan must be able
to reproduce vcap's duty cycle on demand to tell "different estimator" apart from "different sample
count". Hence the `ivh_uc_duty_ns` knob in §3.4.

### 1.2 The formula, exactly

`get_finalized_data()`, `main.cpp:362-428`:

```cpp
u64    stolen_pass = data_end[i].steal_time - data_begin[i].steal_time;   // ns
double used_time   = (data_end[i].use_time - data_begin[i].use_time) * 10000000;  // USER_HZ ticks -> ns
double capacity_perc = used_time / (used_time + stolen_pass);
if (stolen_pass < 10000)          capacity_perc = 1.0;    // <10 us stolen -> perfect
if (vtop_banlist[i])              capacity_perc = 0.5;
if (capacity_perc < 0.001)        capacity_perc = 0.001;
```

then `calculate_ema(decay_length=2, ...)` (`main.cpp:124-131`) and, in `give_to_kernel()`
(`main.cpp:491-498`):

```cpp
round(result_arr[i].capacity_perc_ema * 1024)  ->  /proc/vcapacity_write
```

`calculate_ema()` is a **bias-corrected EWMA**: it maintains an accumulating weight `ema_help`
(`A_n = 1 + d·A_{n-1}`, `d = 0.5^(1/decay_length) = 0.70711`) so the first sample is used verbatim
rather than being blended with a zero seed. Its steady state is a plain EWMA with
`α = 1 − d = 0.29289` per sample.

Sources of the two terms:
- `steal_time` ← `/proc/vcap_info` (`main.cpp:213-230`) ← `get_steal_and_preemptions()`
  (`kernel/sched/core.c:261`), which at the live `ivh_steal_source=1` returns
  `rq->ivh_ref_steal_ns` — **already TSC-derived, no PV steal clock**.
- `use_time` ← `/proc/stat` columns 1+2+3 = user+nice+system (`main.cpp:232-252`). Note what this
  **excludes**: idle, iowait, irq, softirq, steal, guest.

### 1.3 The thing that is easy to miss: vcap is an ACTIVE probe, not a passive reader

**Correction 2, and it is the single most important fact in this document.**

vcap creates one worker thread per CPU, pins it (`stick_this_thread_to_core()`, `main.cpp:154`), puts
it in `SCHED_IDLE` (`move_thread_to_low_prio()`, `main.cpp:166-179`), and during each 200 ms window
every worker runs

```cpp
while (chrono::high_resolution_clock::now() < endtime) { addition_calculator += 1; }   // main.cpp:882
```

Verified live on this machine right now:

```
  13563   13563  RR     99  10 vcap
  13563   13564 IDL      0   0 vcap
  ... 16 IDL threads, one per CPU (psr 0..15) ...
```

**Consequence.** During the measurement window there is always a runnable task on every vCPU, so
`CPUTIME_IDLE` is ~0 and `use_time ≈ window − stolen − irq/softirq`. Therefore

```
vcap's capacity_perc  ≈  (window − stolen) / window  =  1 − steal_fraction_of_WALL_time
```

This is not `used/(used+stolen)` in the naive passive sense. It is **"of the wall time this vCPU
wanted the CPU, what fraction did it get"** — and the reason it means that is the spinner, not the
formula.

A naive in-kernel `Δkcpustat_used / (Δkcpustat_used + Δsteal)` would agree with vcap **only on CPUs
that happen to be busy anyway**, and would collapse toward 0 on partially-idle CPUs — a 40%-idle,
5%-stolen vCPU would read 0.857 → **877** where vcap reads ~1017. That is right at
`IVH_CAP_FLOOR = 850`. This is exactly the class of scale error that produced the previous
regression, arrived at by a different route.

**Design consequence (§3.2):** the production formula must remove idle from *both* sides of the
ratio, which reproduces the spinner's semantics analytically and is invariant to whether the spinner
is present. **Validation consequence (§5.4):** because vcap must stay alive as the comparator
baseline, the spinner is present throughout shadow mode — so shadow mode alone **cannot** detect a
spinner-dependent design error. That is the biggest risk in this plan and it gets its own gate, G3.

### 1.4 Live reference distribution (sampled from `/proc/ivh_debug` during this pass)

`ivh_vact_cpu:` columns 8 and 9 are `vcap_capacity` (`rq->cpu_capacity`) and `vact_capacity`:

| CPU group | vcap `cpu_capacity` | Part C `vact_capacity` (with `ivh_vact_residual=1`) |
|---|---:|---:|
| cpu0–7 (heavy corunner contention) | 368–462 | 369–477 |
| cpu8–15 (light) | **1017–1018** | **475–717** |

Two things follow, and both are load-bearing for §7:

1. **vcap is a near-perfect binary classifier at the 850 floor.** cpu8–15 all pass (1017 > 850),
   cpu0–7 all fail (≤462). Eight clean destinations, eight clean non-destinations.
2. **`ivh_vact_capacity` currently fails the floor on all sixteen CPUs.** At `ivh_cap_source=2` plus
   the BPF edit, `GATE_CAPACITY_LOW` would reject **every** candidate and the destination set would
   be empty — a *different* failure from the migration storm previously observed (that one was
   measured at `ivh_vact_residual=0`, where vact read 1024 everywhere). This is fresh confirmation
   that Part C is not on vcap's scale in either residual mode, and it is why §7 makes
   **destination-set-empty rate a first-class go/no-go metric**, which the existing comparator does
   not measure.

---

## 2. Exact in-kernel source values (requirement 2)

Every term the new signal needs already exists, at tick cadence, on the owning CPU.

| term | in-kernel source | file:line | cadence | notes |
|---|---|---|---|---|
| **stolen** | `rq->ivh_ref_steal_ns` when `ivh_steal_source==1`, else `paravirt_steal_clock(cpu)` | `kernel/sched/core.c:261-283` (`get_steal_and_preemptions()`), producer `core.c:590-836` | **every tick** (`account_process_tick()` → `ivh_ref_accumulate()`, `kernel/sched/cputime.c:566`) | cumulative ns, **monotonic non-decreasing by construction** (`core.c:699-716`) — safe to delta |
| **wall elapsed** | `ivh_raw_tsc()` = `rdtsc()` | `arch/x86/include/asm/ivh_tsc_beat.h:221` | on demand | same source Part C's stamp uses |
| **idle** | `get_cpu_idle_time_us(cpu,NULL) + get_cpu_iowait_time_us(cpu,NULL)` | pattern at `core.c:670-676` | continuous (ktime-based at idle entry/exit, **not** tick-quantised) | **both** are required: `tick_nohz_stop_idle()` files each episode in exactly one. Return `(u64)-1` if `!tick_nohz_active` |
| **used (accounting variant)** | `kcpustat_this_cpu->cpustat[CPUTIME_USER] + [CPUTIME_NICE] + [CPUTIME_SYSTEM]` | written by `account_user_time()` / `account_system_time()`, `kernel/sched/cputime.c:131-210` | every tick | in **ns** already; `/proc/stat` divides down to USER_HZ, so the kernel value is *finer* than what vcap sees |
| **tsc_khz** | `tsc_khz` via `ivh_tsc_ns_to_cycles()` / `ivh_tsc_cycles_to_ns()` | `ivh_tsc_beat.h:227-249` | — | the **only** sanctioned conversions; both carry the `OPTIMIZER_HIDE_VAR()` Bug-4 repair |

### 2.1 Cadence is sufficient — and better than the daemon's

- Ticks are 1 ms (`CONFIG_HZ=1000`, verified in `.config`). A 200 ms window is **200 samples**,
  versus vcap's **single** begin/end pair per window. Every accumulator is a per-tick sum, so the
  200 ms window is fully covered.
- vcap's `use_time` is quantised to USER_HZ = 10 ms, i.e. only **0–20 counts of resolution** across
  its whole 200 ms window (≈5% granularity). The kernel path has ns-resolution steal and
  cycle-resolution wall time. The new signal is strictly better-resolved than the thing it replaces.
- `CONFIG_IRQ_TIME_ACCOUNTING is not set` (verified in `.config:146`), so `irqtime_enabled()` is a
  compile-time false and `account_process_tick()` takes the plain path at `cputime.c:596-609`. Hard
  IRQ / softirq ticks still land in `CPUTIME_IRQ`/`CPUTIME_SOFTIRQ` (not in user+nice+system), so the
  *accounting* variant sees exactly the three fields vcap sees. Good — the replica is faithful.
- `CONFIG_VIRT_CPU_ACCOUNTING_GEN=y` but no `nohz_full=` on the cmdline (verified: `/proc/cmdline`
  has none, `/sys/devices/system/cpu/nohz_full` is `(null)`), so `vtime_accounting_enabled_this_cpu()`
  is false everywhere and the tick path is live on all 16 CPUs. The new hook is nonetheless placed
  **before** that early return, matching `ivh_ref_accumulate()` and `ivh_vact_tick()`
  (`cputime.c:559-589`), so a future config change cannot silently punch holes in the series.
- **NOHZ blind spot, inherited deliberately:** an idle vCPU stops ticking, so its window does not
  close and `ivh_uc_capacity` freezes at its last value. That is the same blind spot
  `clock_preempt`, the TSC heartbeat and Part C all have. It is benign here because the frozen value
  describes a CPU that is doing nothing, and because the first post-idle tick sees a `d_elapsed_c`
  and a `d_idle_c` that both span the gap, so `avail_c ≈ 0` and nothing spurious is booked.
  Belt-and-braces: the `avail_c` clamp in §3.2 and the minimum-`avail` window guard in §3.5.

### 2.2 One-tick lag, stated so it is not rediscovered as a bug

`ivh_uc_tick()` runs at `cputime.c:~587`, **before** `steal_account_process_time()` (line 597) and
before `account_user_time()`/`account_system_time()` (604-607). So on any given tick, both the steal
delta and the kcpustat delta it reads are one tick stale. Over a 200-tick window this is a 0.5%
phase error and it cancels across window boundaries because every window uses the same
`prev_*`-delta discipline. Do not "fix" it by moving the call after the accounting — that would put
it after the `vtime_accounting_enabled_this_cpu()` early return and reintroduce the hole §2.1 exists
to avoid.

---

## 3. The computation (requirement 1)

### 3.1 Where the state lives

**`struct rq`**, alongside `ivh_ref_*` (`kernel/sched/sched.h:1470-1490`) and `ivh_vact_*`
(`sched.h:1532-1586`). Not a `DEFINE_PER_CPU` variable, and the reason is hard:

> The BPF program reads capacity as `select_rq->cpu_capacity` where
> `select_rq = bpf_per_cpu_ptr(&runqueues, cpu)` (`MY_ivh_atc.bpf.c:330,342,424,434`). A per-CPU
> variable is not reachable that way. A `struct rq` field is, via CO-RE, exactly like the field it
> replaces. This is also why the value must NOT be routed through `rq->cpu_capacity_custom`:
> that field only reaches `rq->cpu_capacity` inside `update_cpu_capacity()`
> (`fair.c:10102-10121`) at load-balance cadence, which would throw away the signal's resolution —
> the same reasoning `fair.c:13248-13259` already records for Part C.

Proposed fields (names prefixed `ivh_uc_`, one contiguous block with its own comment header):

```
/* delta state, owning CPU only, written from ivh_uc_tick() */
u64  ivh_uc_prev_tsc;          /* rdtsc() at last tick; 0 == unseeded            */
u64  ivh_uc_prev_steal_ns;     /* cumulative steal at last tick                  */
u64  ivh_uc_prev_idle_ns;      /* idle+iowait ns at last tick                    */
u64  ivh_uc_prev_used_ns;      /* kcpustat user+nice+system at last tick         */

/* window accumulators, all CYCLES, all continuous sums (no thresholds anywhere) */
u64  ivh_uc_win_start_tsc;
u64  ivh_uc_win_avail_c;       /* elapsed - idle                                  */
u64  ivh_uc_win_stolen_c;      /* steal within avail                              */
u64  ivh_uc_win_used_c;        /* avail - stolen        (WALL variant numerator)  */
u64  ivh_uc_win_acct_c;        /* kcpustat used         (ACCT variant numerator)  */

/* EMA state, Q16 fixed point on the 1024 scale (see 3.3) */
u64  ivh_uc_ema_wall_q;
u64  ivh_uc_ema_acct_q;

/* THE OUTPUTS */
unsigned long ivh_uc_capacity;        /* published, 1..1024; == the selected variant */
unsigned long ivh_uc_capacity_wall;   /* always published, diagnostics + G3          */
unsigned long ivh_uc_capacity_acct;   /* always published, diagnostics + G3          */

/* diagnostics / validation */
u64  ivh_uc_windows;           /* windows closed                                  */
u64  ivh_uc_skipped;           /* ticks bailed (unusable idle / no tsc_khz)       */
u64  ivh_uc_extended;          /* windows held open by the min-avail guard        */
u64  ivh_uc_raw_wall;          /* last pre-EMA sample, wall  (1..1024)            */
u64  ivh_uc_raw_acct;          /* last pre-EMA sample, acct  (1..1024)            */
u64  ivh_uc_vcap_at_close;     /* rq->cpu_capacity_custom snapshotted at close    */
```

Init in `sched_init()` beside `rq->ivh_vact_capacity = SCHED_CAPACITY_SCALE`
(`kernel/sched/core.c:10356`), for the identical reason that comment gives:

```
rq->ivh_uc_capacity = rq->ivh_uc_capacity_wall = rq->ivh_uc_capacity_acct = SCHED_CAPACITY_SCALE;
rq->ivh_uc_ema_wall_q = rq->ivh_uc_ema_acct_q = (u64)SCHED_CAPACITY_SCALE << 16;
```

### 3.2 Per-tick accumulation — the exact arithmetic

`ivh_uc_tick()` in `kernel/sched/core.c`, immediately after `ivh_vact_tick()`; call added at
`kernel/sched/cputime.c:~587`.

```c
void ivh_uc_tick(void)
{
    struct rq *rq = this_rq();
    int cpu = smp_processor_id();
    u64 now, idle_us, iowait_us, idle_ns, steal_ns, used_ns;
    u64 d_elapsed_c, d_idle_c, d_steal_c, d_used_c, avail_c, used_c;

    if (unlikely(!READ_ONCE(ivh_uc_enabled)))
        return;
    if (unlikely(!tsc_khz)) { rq->ivh_uc_skipped++; return; }

    now = ivh_raw_tsc();

    idle_us   = get_cpu_idle_time_us(cpu, NULL);
    iowait_us = get_cpu_iowait_time_us(cpu, NULL);
    if (unlikely(idle_us == (u64)-1 || iowait_us == (u64)-1)) {
        rq->ivh_uc_skipped++;              /* !tick_nohz_active: idle unobtainable */
        return;                            /* do NOT touch prev_* -- next sample spans the gap */
    }
    idle_ns  = (idle_us + iowait_us) * NSEC_PER_USEC;
    steal_ns = ivh_uc_steal_ns(cpu);       /* honours ivh_steal_source, see 3.6 */
    used_ns  = kcpustat_this_cpu->cpustat[CPUTIME_USER]
             + kcpustat_this_cpu->cpustat[CPUTIME_NICE]
             + kcpustat_this_cpu->cpustat[CPUTIME_SYSTEM];

    if (unlikely(!rq->ivh_uc_prev_tsc)) {
        rq->ivh_uc_skipped++;
        rq->ivh_uc_win_start_tsc = now;
        goto seed;                          /* first tick: no delta yet */
    }

    d_elapsed_c = now - rq->ivh_uc_prev_tsc;

    /* Every delta is clamped non-negative -- but note carefully that unlike
     * ivh_ref_accumulate() this is NOT a rectifier, because nothing here is a
     * DIFFERENCE of two independently-jittering series that could legitimately
     * go negative.  steal_ns is monotonic by construction (core.c:699-716);
     * used_ns is a monotonic accumulator; idle_ns is documented as
     * occasionally non-monotonic and that is the only clamp that ever fires.
     * There is therefore no residual to carry and no ivh_ref_carry analogue.  */
    d_idle_c  = ivh_tsc_ns_to_cycles(idle_ns  > rq->ivh_uc_prev_idle_ns
                                     ? idle_ns  - rq->ivh_uc_prev_idle_ns : 0);
    d_steal_c = ivh_tsc_ns_to_cycles(steal_ns > rq->ivh_uc_prev_steal_ns
                                     ? steal_ns - rq->ivh_uc_prev_steal_ns : 0);
    d_used_c  = ivh_tsc_ns_to_cycles(used_ns  > rq->ivh_uc_prev_used_ns
                                     ? used_ns  - rq->ivh_uc_prev_used_ns : 0);

    /* avail = wall time this vCPU WANTED the CPU.  Idle leaves the ratio
     * entirely -- it is neither used nor stolen.  This is the analytic
     * equivalent of vcap's SCHED_IDLE spinner (see sec 1.3) and is what makes
     * the signal invariant to whether that spinner exists. */
    avail_c = (d_elapsed_c > d_idle_c) ? d_elapsed_c - d_idle_c : 0;
    d_steal_c = min(d_steal_c, avail_c);
    used_c  = avail_c - d_steal_c;

    rq->ivh_uc_win_avail_c  += avail_c;
    rq->ivh_uc_win_stolen_c += d_steal_c;
    rq->ivh_uc_win_used_c   += used_c;
    rq->ivh_uc_win_acct_c   += min(d_used_c, avail_c);

    ivh_uc_maybe_close_window(rq, now);     /* see 3.4 */
seed:
    rq->ivh_uc_prev_tsc      = now;
    rq->ivh_uc_prev_idle_ns  = idle_ns;
    rq->ivh_uc_prev_steal_ns = steal_ns;
    rq->ivh_uc_prev_used_ns  = used_ns;
}
```

**Both numerators are accumulated on every tick, unconditionally.** `ivh_uc_used_source` selects only
which one is *published* into `ivh_uc_capacity`. This costs four extra adds per tick and it is the
single most important over-provisioning decision in the plan: it makes the spinner-dependence
question of §1.3 answerable from **one boot with no rebuild** (§5.4 / G3).

### 3.3 Window close, ratio, EMA — exact fixed-point form

```c
static void ivh_uc_close(struct rq *rq, u64 now)
{
    u64 avail = rq->ivh_uc_win_avail_c;
    u64 min_steal_c = ivh_tsc_ns_to_cycles(READ_ONCE(ivh_uc_min_steal_ns));
    u32 alpha = (u32)READ_ONCE(ivh_uc_ema_alpha_q16);   /* 1..65536 */
    u64 x_wall, x_acct;

    /* vcap's `if (stolen_pass < 10000) capacity_perc = 1.0` guard, main.cpp:371 */
    if (!avail || rq->ivh_uc_win_stolen_c < min_steal_c) {
        x_wall = SCHED_CAPACITY_SCALE;
        x_acct = SCHED_CAPACITY_SCALE;
    } else {
        x_wall = div64_u64(rq->ivh_uc_win_used_c * SCHED_CAPACITY_SCALE, avail);
        /* ACCT variant reproduces vcap literally: used / (used + stolen). */
        {
            u64 den = rq->ivh_uc_win_acct_c + rq->ivh_uc_win_stolen_c;
            x_acct = den ? div64_u64(rq->ivh_uc_win_acct_c * SCHED_CAPACITY_SCALE, den)
                         : SCHED_CAPACITY_SCALE;
        }
    }
    x_wall = clamp_val(x_wall, 1ULL, (u64)SCHED_CAPACITY_SCALE);   /* vcap floors at 0.001 */
    x_acct = clamp_val(x_acct, 1ULL, (u64)SCHED_CAPACITY_SCALE);

    ivh_uc_ema(&rq->ivh_uc_ema_wall_q, x_wall, alpha);
    ivh_uc_ema(&rq->ivh_uc_ema_acct_q, x_acct, alpha);

    rq->ivh_uc_capacity_wall = (unsigned long)(rq->ivh_uc_ema_wall_q >> 16);
    rq->ivh_uc_capacity_acct = (unsigned long)(rq->ivh_uc_ema_acct_q >> 16);
    WRITE_ONCE(rq->ivh_uc_capacity,
               READ_ONCE(ivh_uc_used_source) ? rq->ivh_uc_capacity_acct
                                             : rq->ivh_uc_capacity_wall);
    /* validation taps, sec 5 */
    rq->ivh_uc_raw_wall = x_wall;  rq->ivh_uc_raw_acct = x_acct;
    rq->ivh_uc_vcap_at_close = READ_ONCE(rq->cpu_capacity_custom);
    ivh_uc_shadow_bin(rq);
    rq->ivh_uc_windows++;

    rq->ivh_uc_win_start_tsc = now;
    rq->ivh_uc_win_avail_c = rq->ivh_uc_win_stolen_c = 0;
    rq->ivh_uc_win_used_c  = rq->ivh_uc_win_acct_c   = 0;
}

static __always_inline void ivh_uc_ema(u64 *ema_q, u64 x, u32 alpha_q16)
{
    s64 cur  = (s64)*ema_q;                 /* Q16 of the 1024 scale, <= 1024<<16 = 2^26 */
    s64 diff = ((s64)x << 16) - cur;        /* |diff| <= 2^26                            */
    *ema_q = (u64)(cur + ((diff * (s64)alpha_q16) >> 16));   /* |product| <= 2^42, safe   */
}
```

**Why this EMA form.** No division, no floating point, no `pow()`, no transcendental anything in the
kernel. `α` is a **directly-set Q16 sysctl**, not derived from a half-life, precisely so no
approximation of `0.5^(w/h)` has to live in the kernel. The mapping is documented once, here:

```
α = 1 − 0.5^(window / half_life)
half_life_samples = ln(0.5) / ln(1 − α)
```

| desired half-life | window 200 ms | `ivh_uc_ema_alpha_q16` |
|---|---|---:|
| **10.4 s (matches vcap)** | 52 samples | **868** |
| 5 s | 25 samples | 1795 |
| 2 s | 10 samples | 4409 |
| 1 s | 5 samples | 8583 |
| vcap's own duty-cycled form (window 200 ms, period 5.2 s, 2 samples) | 2 samples | 19195 |

Default: **868**.

**Cold start.** `ema_q` is seeded to `1024<<16` at rq init (§3.1), i.e. "perfectly healthy", which is
the don't-act direction for Gate 1. It reaches within 1% of a step change in ~5 half-lives = ~52 s at
the default α. That is *slower* than vcap's bias-corrected form. Mitigation, and it is one line:
**on the first window close (`ivh_uc_windows == 0`), assign rather than blend** (`*ema_q = x << 16`),
which reproduces `calculate_ema()`'s first-sample behaviour exactly and removes the entire cold-start
transient. Do this; it is free.

### 3.4 Window cadence and the duty-cycle knob

```c
static void ivh_uc_maybe_close_window(struct rq *rq, u64 now)
{
    u64 win_c  = ivh_tsc_ns_to_cycles(READ_ONCE(ivh_uc_window_ns));
    u64 duty_c = ivh_tsc_ns_to_cycles(READ_ONCE(ivh_uc_duty_ns));

    if (unlikely(win_c < 1000))                       /* see 3.7 -- sanity floor */
        return;
    if ((s64)(now - rq->ivh_uc_win_start_tsc) < (s64)win_c)
        return;
    /* min-avail guard: a window that was almost entirely idle carries a
     * near-zero denominator and would publish a ratio derived from a
     * millisecond of data.  Extend rather than publish noise. */
    if (rq->ivh_uc_win_avail_c <
        div64_u64(win_c * READ_ONCE(ivh_uc_min_avail_pct), 100)) {
        rq->ivh_uc_extended++;
        return;
    }
    ivh_uc_close(rq, now);
    if (duty_c)                                        /* 0 (default) = continuous */
        rq->ivh_uc_win_start_tsc = now + duty_c;       /* skip the next duty_c of accumulation */
}
```

With `ivh_uc_duty_ns = 0` (default) the windows tile back-to-back — 26× vcap's sample count. Setting
`ivh_uc_duty_ns = 5000000000` and `ivh_uc_ema_alpha_q16 = 19195` reproduces vcap's exact duty cycle
and time constant, for the shadow comparison, **with no rebuild**. That is the entire reason the knob
exists.

Note the accumulators keep running during the duty skip in the code above only if `win_start_tsc` is
pushed forward *and* the accumulate path is suppressed. Implementation detail for the builder: gate
the four `+=` lines in §3.2 on `(s64)(now - rq->ivh_uc_win_start_tsc) >= 0`, so a future start stamp
suppresses accumulation cleanly with one already-computed comparison. Do not add a second state
variable for it.

### 3.5 How this avoids Part C's compression (failure mode 1)

Part C's `ivh_vact_gap_split()` (`core.c:1225-1244`) books steal **only when an inter-tick gap
exceeds one nominal tick after idle removal**: `ex = avail − tick_c + debt; if (ex <= 0) return 0;`.
That is a *thresholded-excess* estimator, and `ivh_tsc_final_state_report_2026-08-02.md` §6.3
establishes structurally why it under-resolves finely-distributed steal and compresses the CPUs into
a narrow band.

`ivh_uc_tick()` has **no threshold of any kind on any term.** `d_steal_c` is a delta of a counter
that was itself accumulated continuously by `ivh_ref_accumulate()`; `d_idle_c` is a delta of a
ktime-based accumulator; `d_elapsed_c` is a raw TSC delta. Every tick contributes its exact
proportion. There is no `jump_threshold`, no `tick_c` comparison, no debt carry, and no branch that
routes a sub-threshold quantity to the wrong side of the ratio.

Concretely: the failure Part C has — *"a vCPU descheduled for 300 µs three times inside one 1 ms tick
period"* — is handled here because `ivh_ref_steal_ns` already contains those 900 µs (REF_TSC stops
during all three, and the difference from TSC is booked), and `ivh_uc_tick()` simply deltas it.

**This is why the plan explicitly does not reuse or modify `ivh_vact_capacity`.** They are different
estimators of the same quantity, and Part C's shape is the documented cause of the regression. Part C
is left byte-for-byte alone, remains ungated and still produced, and remains available at
`ivh_cap_source=2` for comparison.

### 3.6 Steal source

```c
static __always_inline u64 ivh_uc_steal_ns(int cpu)
{
    if (READ_ONCE(ivh_steal_source))
        return READ_ONCE(cpu_rq(cpu)->ivh_ref_steal_ns);
#ifdef CONFIG_PARAVIRT
    return paravirt_steal_clock(cpu);
#else
    return 0;
#endif
}
```

Deliberately **no new knob**: it honours the existing global `ivh_steal_source`, so vcap and
`ivh_uc_capacity` always consume the *same* steal number. That is what makes the shadow comparison a
controlled comparison of the *transformation* rather than a comparison confounded by two different
steal sources. `get_steal_and_preemptions()` is not reused directly only because it also writes
`*preempt` and takes a `cpunum` that is always `this_cpu` here; the two must be kept
behaviourally identical and a comment at each site should say so.

`ivh_ref_steal_enabled=1` is a live prerequisite; the existing sysctl handler at `core.c:894-918`
already refuses `ivh_steal_source=1` without it, so no new guard is needed.

### 3.7 How this avoids the Bug 4 miscompile (failure mode 2)

Bug 4 (`ivh_tsc_beat.h:182-219`) required a **compile-time constant** operand — `TICK_NSEC`, which at
`CONFIG_HZ=1000` is numerically identical to the `USEC_PER_SEC` divisor — for GCC to prove aliasing
and satisfy both `mulq`'s and `divq`'s operands from `%rax`.

Rules for this code, to be written into the source as a comment block:

1. **Only ever call `ivh_tsc_ns_to_cycles()` / `ivh_tsc_cycles_to_ns()`.** Both already carry
   `OPTIMIZER_HIDE_VAR()` (`ivh_tsc_beat.h:234,246`), which makes the dividend opaque and defeats the
   aliasing proof for any operand in either direction.
2. **Never pass a compile-time constant to them.** Every argument in §3.2–3.4 is either a runtime
   delta or a `READ_ONCE()`'d sysctl (`ivh_uc_window_ns`, `ivh_uc_duty_ns`, `ivh_uc_min_steal_ns`).
   The compiler cannot prove any of them equal to `USEC_PER_SEC`. This is a structural property of
   the design, not a coincidence, and it is the primary defence.
3. **Never use `mul_u64_u64_div_u64()`.** The ratio uses `div64_u64()` only, exactly as
   `core.c:1541-1543` does.
4. **Sanity floor, belt to the above braces.** `if (win_c < 1000) return;` in
   `ivh_uc_maybe_close_window()` — a nominal window under 1000 cycles implies a sub-1 MHz TSC, which
   does not exist. `ivh_vact_tick()` (`core.c:1377-1381`) added exactly this check for exactly this
   reason. A miscompiled conversion here would not add noise, it would invert the signal.
5. **Overflow check, done explicitly.** `win_used_c * SCHED_CAPACITY_SCALE` with a 60 s window cap at
   2.2 GHz is `1.3e11 × 1024 = 1.4e14` — 5 orders below `u64` max. Sysctl handler clamps
   `ivh_uc_window_ns` to `[10 ms, 60 s]` so this bound holds by construction.
6. **Live self-check, so a future miscompile is caught in one read rather than one benchmark.** Add
   to `/proc/ivh_debug`: `ivh_uc_tick_cycles_selfcheck: <ivh_tsc_ns_to_cycles(READ_ONCE(one_ms_var))>`
   where `one_ms_var` is a `static volatile unsigned long = 1000000`. If that ever prints ~1 instead
   of ~2,200,000, the miscompile is back and it is visible without running anything.

---

## 4. Sysctls (all in `kernel/sched/bpf_sched.c` beside the Part C knobs)

| sysctl | default | range | meaning |
|---|---:|---|---|
| `ivh_uc_enabled` | **1** | 0/1 | produce the signal. **Default ON and ungated by any consumer**, matching `ivh_vact_tick()`'s stated discipline (`core.c:1314-1320`): a signal only produced once someone has decided to trust it can never be compared against what it would replace |
| `ivh_uc_window_ns` | **200000000** | 10e6 .. 60e9 | measurement window; matches vcap's `-p 200` |
| `ivh_uc_duty_ns` | **0** | 0 .. 60e9 | 0 = continuous; 5000000000 reproduces vcap's `-s 5000` duty cycle |
| `ivh_uc_ema_alpha_q16` | **868** | 1 .. 65536 | Q16 EMA coefficient; 868 ⇒ 10.4 s half-life at a 200 ms window (§3.3 table) |
| `ivh_uc_used_source` | **0** | 0/1 | 0 = WALL (`avail − stolen`)/`avail`, production; 1 = ACCT (kcpustat replica of vcap's literal formula), validation only |
| `ivh_uc_min_steal_ns` | **10000** | 0 .. 1e9 | vcap's `stolen_pass < 10000 → 1.0` guard (`main.cpp:371`) |
| `ivh_uc_min_avail_pct` | **10** | 0 .. 100 | minimum non-idle fraction of a window before it may publish; else extend |
| `ivh_uc_shadow` | **0** | 0/1 | feed the per-window comparison counters and histograms (§5) |
| `ivh_uc_avgcap_enabled` | **0** | 0/1 | run the 1 Hz worker that writes `average_capacity_all` from `ivh_uc_capacity` (§6.3) |
| `ivh_cap_source` | 0 | **0..3** | **extended**: 0 vcap `rq->cpu_capacity`, 1 shadow, 2 `rq->ivh_vact_capacity`, **3 `rq->ivh_uc_capacity`** |

**Easily-missed implementation detail.** `ivh_cap_source` and `ivh_preempt_event_source` share one
validator, `ivh_proc_source_common()` (`bpf_sched.c:393-414`), which hard-rejects `val > 2`. Extending
`ivh_cap_source` to 3 **must not** extend `ivh_preempt_event_source` to 3 — there is no Part-C-plus-one
preemption series. Add a `max` parameter to `ivh_proc_source_common()` and pass 3 / 2 respectively,
updating the `pr_err()` text per knob. Getting this wrong silently makes `ivh_preempt_event_source=3`
readable as "not 2", i.e. it would fall back to real steal with no error — the exact class of silent
misconfiguration that validator was written to prevent.

Likewise `ivh_gate_capacity()` (`fair.c:13260-13263`) must change from `bool tsc_cap` to a source
enum, and its two callers (`ivh_steal_imminent()` `fair.c:13365-13389`,
`ivh_rq_capacity_and_timeleft_ok()` `fair.c:13416-13433`) updated together — `fair.c:13400-13414`
already documents why those two must track each other exactly.

---

## 5. Validation before switch (requirement 3)

Established project pattern: `is_wait_preempted` and `is_cs_preempted` both ran in shadow with an
agreement matrix before becoming authoritative. Same discipline, three instruments, **all in the same
kernel build**, all default-off or zero-cost.

### 5.1 Per-CPU side-by-side line in `/proc/ivh_debug`

A **new** line, not an extension of `ivh_vact_cpu:` (that format is consumed by existing scripts):

```
# cpu vcap_custom vcap_cpu_capacity uc_capacity uc_wall uc_acct raw_wall raw_acct
#     win_avail_c win_stolen_c windows extended skipped vact_capacity
ivh_uc_cpu: 3 368 368 371 371 402 355 389 220038112 128554901 1043 2 0 370
```

Publishing `cpu_capacity_custom` **and** `cpu_capacity` separately matters: Gate 1 reads
`rq->cpu_capacity`, which only refreshes from `cpu_capacity_custom` at load-balance cadence inside
`update_cpu_capacity()` (`fair.c:10112-10115`). The new signal is fresh at window cadence. That is a
genuine behaviour change **even at identical estimator output**, and it must be attributable in the
data rather than mistaken for estimator divergence.

Also add an exported `get_uc_compare()` accessor next to `get_vact_compare()`
(`core.c:1568-1583`), for the reason that function's own comment gives: the module can reformat for
free, but adding an accessor later costs a rebuild. Ship it in Build A even with no in-tree caller.

### 5.2 Signed-divergence histogram (`ivh_uc_shadow=1`)

Per-CPU, binned at each window close on `(uc_capacity − vcap_cpu_capacity_custom)`, 16 signed buckets
spanning ±1024:

```
ivh_uc_div_hist: <b0> <b1> ... <b15>
#  b0: <=-512   b1: -512..-256  b2: -256..-128 ... b7: -8..0
#  b8: 0..8     ...             b15: >= +512
```

Aggregated across CPUs in `/proc/ivh_debug`, and also dumped per-CPU so a per-group (cpu0–7 vs
cpu8–15) read is possible. This is the direct analogue of `ivh_cs_age_hist_*`.

### 5.3 Threshold-crossing 2×2s — the measurement that actually matters

`fair.c:14195-14199` already states the criterion: *"agreement in the middle of the range is worth
nothing if the two disagree at the thresholds."* So bin, at each window close, per CPU:

```
ivh_uc_thr850_both / _vcap_only / _uc_only / _neither      (pass = cap >  850)
ivh_uc_thr1010_both / _vcap_only / _uc_only / _neither     (pass = cap > 1010, Gate 1 reject)
```

Zero-cost, four `this_cpu_inc()` per window close per CPU (5/s/CPU at the default window).

### 5.4 Gate-level agreement — extend the existing comparator, do not replace it

In `ivh_pre_lock()`'s `ivh_decision_shadow` block (`fair.c:13541-13567`), the existing
`ivh_cap_pass_{both,real_only,tsc_only,neither}` bins model the two BPF gates against
{vcap, Part C}. **Add a second, parallel set** modelling {vcap, uc}, leaving the existing counters
byte-for-byte untouched:

```c
pass_uc = drq->ivh_uc_capacity > IVH_BPF_CAP_FLOOR && drq->ivh_uc_capacity > src_uc;
/* -> ivh_uc_pass_both / _vcap_only / _uc_only / _neither */
```

and in `ivh_steal_imminent()` (`fair.c:13375-13383`), a third evaluation
`uc_go = !(ivh_gate_capacity(rq, SRC_UC) > ivh_capacity_threshold) && !ivh_gate_time_left_reject(...)`
binned into `ivh_dec_uc_{agree_go,agree_nogo,uc_only_go,real_only_go}`.

**And the counter the existing comparator does not have, which §1.4 shows is now the leading risk:**

```
ivh_destset_empty_vcap   /* evaluations where NO candidate CPU passed both BPF gates, modelled on vcap */
ivh_destset_empty_uc     /* same, modelled on uc                                                      */
ivh_destset_empty_tsc    /* same, modelled on Part C -- free, and would have predicted §1.4 outright   */
```

One extra `bool any_pass_*` per source across the existing `for_each_online_cpu()` walk. This directly
predicts "IVH stops migrating at all" — the failure mode that the previous attempt could not have
seen and that the current `ivh_vact_capacity` values would produce today.

### 5.5 Rank agreement — userspace, no kernel support

Sample `ivh_uc_cpu:` at 10 Hz for a full run, compute Spearman ρ of `uc_capacity` against
`vcap_custom` across the 16 CPUs per sample, report the distribution. The report's own §2.4/§6
conclusion is that *"does the new number rank the CPUs the same way vcap does"* is a sharper test than
any aggregate. Pure `awk`/Python over `/proc/ivh_debug`; nothing to build.

---

## 6. Consumer switchover (requirement 4)

There are **two** authoritative flips, and they have completely different rollback characteristics.
The previous attempt's most expensive lesson was that reverting the kernel sysctl did **not** undo
the regression, because the BPF side is a compile-time source edit
(`ivh_tsc_final_state_report_2026-08-02.md` §6.4).

### 6.1 Kernel side — Gate 1

`echo 3 > /proc/sys/kernel/ivh_cap_source`. Rollback: `echo 0`. Instant, no reload.
Touches `ivh_gate_capacity()` (`fair.c:13260`) and both its callers only.

### 6.2 BPF side — `process_cpu()`, and this is where the kill switch goes

**Problem.** Four live reads of `select_rq->cpu_capacity` in `MY_ivh_atc.bpf.c`:

| line | context | live? |
|---|---|---|
| 342 | `cap_sum` instrumentation | yes (diagnostic only) |
| **424** | `GATE_CAPACITY_LOW`: `<= IVH_CAP_FLOOR` → reject | **yes, decisive** |
| **434** | `GATE_NOT_BETTER`: `<= ctx->source_capacity` → reject | **yes, decisive** |
| **658** | `.source_capacity = rq->cpu_capacity` in `task_ctx` init | **yes, decisive** |
| 741, 749, 765 | `search_latency()` / `test32`, `SEC("sched/cfs_latency_select")` | **no** — the hook has no kernel call site |

**Plan.**

1. Add a config map to `MY_ivh_atc.bpf.c`:

```c
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 4);          /* over-provisioned: key 0 = cap_source, 1-3 reserved */
    __type(key, u32);
    __type(value, u32);
} ivh_cfg SEC(".maps");
```

2. Resolve the source **once per scan**, in `bpf_sched_cfs_select_run_cpu_spin`'s handler
   (around `:650-666`), and stash it in `task_ctx`:

```c
u32 k = 0, cap_src = 0;
u32 *v = bpf_map_lookup_elem(&ivh_cfg, &k);
if (v) cap_src = *v;
...
.cap_source     = cap_src,
.source_capacity = ivh_cap_of(rq, cap_src),
```

   Resolving once, not per candidate, does two things: it removes a map lookup from the inner
   `bpf_loop()` (verifier and cost), and it makes the source **atomic across one scan** — a mid-scan
   map write cannot produce a half-vcap, half-uc candidate set, which would be an unreproducible
   decision.

3. One helper, four call sites:

```c
static __always_inline unsigned long ivh_cap_of(struct rq *rq, u32 src)
{
    return src == 3 ? rq->ivh_uc_capacity : rq->cpu_capacity;
}
```

   Apply at `:342`, `:424`, `:434`, `:658`. **Also apply at `:741,749,765`** — those are dead today,
   but converting them costs nothing and guarantees no vcap-scale read survives if
   `sched/cfs_latency_select` ever gains a call site. Verify the "no call site" claim once more at
   build time (`grep -rn cfs_latency_select include/linux/sched_hook_defs.h kernel/`) and record the
   result in the commit message.

4. **Runtime flip:** `bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0`
   **Rollback:** `... value 0 0 0 0`. Instant. No recompile, no reload, no restart of `MY_ivh_atc`.
   This converts the riskiest flip in the system from *"edit, recompile, reload, hope"* into one map
   write, which is the report's own §6.4 recommendation.

5. **Belt-and-braces rollback:** before the first flip, copy the known-good artifacts:
   `cp tools/bpf/MY_ivh_atc.bpf.o tools/bpf/MY_ivh_atc.bpf.o.vcap-known-good` and the same for the
   binary. If the map write is somehow not enough, the recovery is `pkill MY_ivh_atc` + relaunch from
   the saved binary — and per the memory note, **always `pkill` the existing `MY_ivh_atc` before
   starting a new one**; duplicates cause D-state and double hook firing.

6. **`tools/bpf/vmlinux.h` must be regenerated** from the Build-A `vmlinux` before the BPF program
   can compile a `rq->ivh_uc_capacity` access (CO-RE needs the field in BTF). This has bitten this
   project before. Regenerate with the standard
   `bpftool btf dump file vmlinux format c > tools/bpf/vmlinux.h`, diff it, and be prepared for
   unrelated churn.

### 6.3 The third, latent consumer: `average_capacity_all`

`average_capacity_all` (`core.c:197`) is written by vcap via `/proc/vav_capacity_write` →
`set_average_capacity_all()` and passed into BPF at `fair.c:13600` as `ctx->average_capacity`. Today
it is read **only** inside `search_latency()` (dead hook), so it is harmless — but the moment vcap is
killed it becomes a frozen number that a future call site would silently consume on vcap's scale.

Ship the fix in Build A, default off: a 1 Hz `delayed_work` that walks online CPUs, averages
`ivh_uc_capacity` over those above a straggler cutoff (mirroring `main.cpp:390-393`), and calls
`set_average_capacity_all()`. Gated by `ivh_uc_avgcap_enabled` (default 0) so it changes nothing
until stage G6. This is 30 lines and it removes the last daemon dependency from the system.

---

## 7. Build and validation sequence (requirements 5 and 6)

### Build A — one kernel build, one reboot. Everything.

Nothing below this line requires a second kernel build. Files touched:

| file | change |
|---|---|
| `kernel/sched/sched.h` | `struct rq` `ivh_uc_*` block (§3.1); `extern void ivh_uc_tick(void);`; `extern` for the new sysctls; `get_uc_compare()` proto |
| `kernel/sched/core.c` | `ivh_uc_tick()`, `ivh_uc_close()`, `ivh_uc_ema()`, `ivh_uc_steal_ns()`, `ivh_uc_shadow_bin()`, `get_uc_compare()` + `EXPORT_SYMBOL_GPL`, avgcap worker, rq init seed at `:10356` |
| `kernel/sched/cputime.c` | one call to `ivh_uc_tick()` after `ivh_vact_tick()` at `:586` |
| `kernel/sched/bpf_sched.c` | 9 new sysctls; `ivh_proc_source_common()` gains a `max` param; `ivh_cap_source` max → 3 |
| `kernel/sched/fair.c` | `ivh_gate_capacity()` bool → enum + 2 callers; third shadow evaluation; `ivh_uc_pass_*`, `ivh_dec_uc_*`, `ivh_destset_empty_*` per-CPU counters; `ivh_uc_cpu:` / histogram / 2×2 output in `ivh_debug_show()` |
| `include/linux/bpf_sched.h` | externs + the doc comment block for the new knobs |

**Over-provisioning checklist — include upfront so no counter costs a reboot:**
both EMA variants always accumulated; both capacities always published; raw pre-EMA samples exposed;
`vcap_at_close` snapshot; `windows`/`skipped`/`extended`; signed-divergence histogram; both threshold
2×2s; both new gate 2×2s; all three `destset_empty` counters; the `tsc_khz` self-check line;
`get_uc_compare()` with no caller; the avgcap worker with its knob off. This mirrors why
`ivh_tsc_beat.h`'s counter set was deliberately over-provisioned.

### Build B — BPF only, after Build A boots

Regenerate `vmlinux.h`; add `ivh_cfg` map + `ivh_cap_of()` + `task_ctx.cap_source`; recompile,
reload. **Default map value 0 = vcap**, so loading Build B is a behavioural no-op and can be done and
proven safe long before any flip.

### Stage gates

Every stage is a sysctl or map write within one boot. Workload throughout: the established corunner
+ hackbench pattern, `NHextend`-based runs with `-n` (per the memory note, IVH requires it), vcap
launched exactly as `/home/nick/IVH` does (`-p 200 -s 5000`), `MY_ivh_atc` singleton.

---

**G0 — baseline re-establishment (no new code consumed). PREREQUISITE.**
Boot Build A. `ivh_uc_shadow=0`, `ivh_cap_source=0`, `ivh_decision_shadow=0`. Run 3 clean rounds.
- **GO:** migrations/round in the historical healthy band (**48–53 K**), throughput at the current
  baseline. `ivh_uc_windows` advancing on all 16 CPUs.
- **NO-GO:** if the baseline itself has moved, stop — nothing measured after this point is
  interpretable. This is the provenance discipline §0.1 of the prior report asks for.

**G1 — signal sanity (passive).**
`ivh_uc_shadow=1`. One 5-minute loaded run.
- **GO:** `ivh_uc_skipped / ivh_uc_windows < 1%` on every CPU. `ivh_uc_extended` small on loaded CPUs.
  `uc_capacity_wall` ∈ [1,1024], **not pinned at a rail on all 16 CPUs simultaneously** (that is the
  Bug-4 signature). `ivh_uc_tick_cycles_selfcheck ≈ tsc_khz` (~2,200,000), not ~1.
- **GO (the discriminating one):** `uc_capacity_wall` reproduces the bimodality — median **> 950 on
  cpu8–15** and **< 700 on cpu0–7**, i.e. a separation ratio ≥ 1.4×. Reference: vcap gives 1017 vs
  368–462 = **2.3×**; Part C at `residual=1` gives 475–717 vs 369–477 = **~1.3× with overlap**.
- **NO-GO:** separation < 1.4×, or any group overlap. Stop. Do not proceed to retuning thresholds —
  a signal that does not separate is not a threshold problem (report §6.6 point 2). Diagnose from
  `raw_wall` (pre-EMA) first: if raw separates and EMA does not, α is wrong; if raw does not, the
  accumulation is wrong.

**G2 — agreement with vcap (still passive; vcap authoritative).**
`ivh_uc_shadow=1`, `ivh_decision_shadow=1`. Zero counters. ≥ 3 loaded rounds, ≥ 10 min total.
- **GO:** Spearman ρ(uc, vcap) across 16 CPUs ≥ **0.85** median over ≥ 300 samples.
- **GO:** `ivh_uc_thr850_*` agreement (`both + neither`) ≥ **90%** of (CPU × window) samples.
  `ivh_uc_thr1010_*` agreement ≥ **90%**.
- **GO:** `ivh_uc_pass_vcap_only / ivh_uc_pass_both < 0.25` **and**
  `ivh_uc_pass_uc_only / ivh_uc_pass_both < 0.25`.
  *(Reference: the Part C attempt measured **1.42×** and **0.66×** and regressed. 0.25 is a
  deliberately much stricter bar than "small relative to".)*
- **GO:** `ivh_dec_uc_only_go` and `ivh_dec_real_only_go` each < 0.25 × `ivh_dec_uc_agree_go`.
- **GO:** `ivh_destset_empty_uc ≤ 1.2 × ivh_destset_empty_vcap`.
- **NO-GO on any of the above:** stop and diagnose. **Explicitly do NOT retune `IVH_CAP_FLOOR` or
  `ivh_capacity_threshold` to make these pass.** The entire scale design (§3.3) exists so the
  thresholds transfer; if they do not, the estimator is wrong, and moving the floor re-opens the
  destination-set-empty failure that §1.4 shows is now live. Threshold changes are a last resort and
  must be made in **both** `MY_ivh_atc.bpf.c:289` and `fair.c:121` in the same commit.
- **Diagnostic, not a gate:** compare `uc_capacity_wall` vs `uc_capacity_acct`. With vcap's spinner
  running they should be close on busy CPUs and diverge on lightly-loaded ones. If they are
  indistinguishable everywhere, the spinner is fully suppressing idle — which is the confirmation
  that §1.3's risk is real and that G3 is mandatory.
- **Optional confounder isolation, costs one run:** set `ivh_uc_duty_ns=5000000000` and
  `ivh_uc_ema_alpha_q16=19195` to reproduce vcap's exact duty cycle and time constant. If agreement
  jumps, the residual divergence was sample count, not estimator. Restore defaults after.

**G3 — spinner independence. THE CRITICAL GATE. Must precede any authoritative flip.**
Still `ivh_cap_source=0` and BPF map = 0 — nothing consumes the new signal.
1. Record per-CPU `uc_capacity_wall`, `uc_capacity_acct` and per-CPU steal deltas
   (`ivh_ref_cpu:` / `/proc/stat`) over a loaded run **with vcap running**.
2. `pkill vcap` (all 16 spinners disappear). Wait 10 s. **Re-run the identical workload.**
3. Compare.
- **GO:** per-CPU `uc_capacity_wall` stays within **±100 (on the 1024 scale)** of its
  vcap-running value on the same CPU under the same workload.
- **GO (expected, and it is confirmation not failure):** `uc_capacity_acct` **collapses** on
  lightly-loaded CPUs. That divergence appearing is positive proof the spinner semantics were real
  and that shipping the WALL variant was the correct decision.
- **Validity control:** per-CPU cumulative steal over the run must not have moved by more than
  **±20%** between the two runs. Removing 16 always-runnable `SCHED_IDLE` threads genuinely changes
  how much the host steals from this guest; if steal moved more than that, the comparison is
  confounded and must be repeated with a co-runner pattern that holds host pressure constant.
- **NO-GO:** `uc_capacity_wall` moves materially. Stop. The signal is spinner-dependent for a reason
  not yet understood, and no amount of downstream tuning is safe until it is.
4. Restart vcap exactly as `/home/nick/IVH` does before proceeding (note the script's own comment:
   vcap must be started **after** `ivh_steal_source` is set, or its first EMA samples mix sources).

**G4 — kernel-side flip only. BPF still on vcap.**
`echo 3 > /proc/sys/kernel/ivh_cap_source`. vcap still running.
- **Canary, checked first and continuously:** `ivh_migrations_done` per round. Historical healthy
  band 48–53 K; the previous regression showed **72.7 K**.
  - **GO:** ≤ **61 K** (baseline +15%).
  - **IMMEDIATE REVERT (`ivh_cap_source=0`):** > **65 K**, without waiting for the run to finish.
    Migration count moved 1.4× *before* throughput moved last time; it is the leading indicator.
- **GO:** `ivh_steal_imminent_capacity_reject` rate within **±30%** of its `ivh_cap_source=0` rate
  over the same workload.
- **GO:** throughput ≥ baseline − 3%.
- **NO-GO:** revert to 0, return to G2 with the divergence data.

**G5 — BPF-side flip. vcap still running.**
`bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0`.
- Same canary, same thresholds, same immediate-revert rule (rollback is one map write to `value 0`).
- **GO:** migrations/round ≤ 61 K **and** throughput ≥ baseline − 3% over ≥ 3 rounds.
- **NO-GO:** map write back to 0 — and confirm the revert took effect by watching migrations/round
  return to band, because the previous attempt's most confusing hour was a revert that did not
  revert.

**G6 — retire vcap.**
Set `ivh_uc_avgcap_enabled=1`. Remove the `vcap` launch line from `/home/nick/IVH` (keep it in a
comment). Reboot into the same config without vcap.
- **GO:** migrations/round and throughput both within the band established at G5, over ≥ 3 rounds.
- **GO:** `rq->cpu_capacity_custom` is now 0 on every CPU (nothing writes it), and
  `update_cpu_capacity()` (`fair.c:10112`) therefore falls through to the stock
  `scale_rt_capacity()` value — **verify this does not perturb the load balancer**, since
  `rq->cpu_capacity` was previously pinned to vcap's number and is now free. This is a genuine
  side effect of retirement, it affects CFS load balancing rather than IVH, and it must be measured
  rather than assumed. If it perturbs anything, the mitigation is to have the avgcap worker also
  write `set_custom_capacity(ivh_uc_capacity, cpu)` at 1 Hz, preserving the previous behaviour with
  no daemon. Ship that call in Build A behind the same `ivh_uc_avgcap_enabled` knob so it costs
  nothing if needed.
- **GO:** `/proc/vcap_info`, `/proc/vcapacity_write` etc. still exist (the module is unchanged) and
  nothing else in the system reads them — grep `NHextend.c`, `ivh_exec`, and the scripts under
  `/home/nick/` before declaring this done.

---

## 8. Risks, ranked

1. **[HIGHEST] Spinner-semantics blindness in shadow mode.** vcap must stay alive to be the
   comparator, and its 16 pinned `SCHED_IDLE` spinners pin `CPUTIME_IDLE` to ~0 during every
   measurement. Any design that reads `used` from accounting will therefore *validate perfectly in
   shadow and break the moment vcap is killed*, on exactly the lightly-loaded CPUs that
   `IVH_CAP_FLOOR = 850` is most sensitive to. Mitigations: the WALL formula is spinner-invariant by
   construction (§3.2); both variants are computed always so the divergence is directly observable
   (§3.2); and **G3 tests it explicitly before any flip**. This risk is the reason G3 exists and the
   reason G3 sits before G4.
2. **`GATE_NOT_BETTER` is a relative comparison.** It degrades to *arbitrary* under compression,
   which is the mechanism behind the previous migration storm (report §6.3). This plan's answer is to
   restore genuine separation rather than to drop the gate — G1's ≥ 1.4× separation bar and G2's
   `pass_*_only < 0.25` bars are what protect it. If G1 passes but G2's `pass_uc_only` is what fails,
   the report's `dest > source + δ` margin option is the cheap next move and is modellable offline
   from the shadow data with no rebuild.
3. **Destination-set-empty.** §1.4 shows this is not hypothetical: `ivh_vact_capacity` today would
   empty the set on all 16 CPUs. New counters (§5.4) make it a measured go/no-go rather than a
   surprise.
4. **Bug-4-class miscompile.** Structurally excluded (§3.7: no compile-time-constant operands
   anywhere) plus a sanity floor plus a live self-check line. Low, but it inverted a signal once and
   cost a full build cycle.
5. **`vmlinux.h` regeneration churn** for Build B. Known hazard in this project. Diff before
   committing.
6. **`ivh_proc_source_common()` max-value extension** leaking `ivh_preempt_event_source=3` as a
   silent no-op. Called out explicitly in §4 because it is a one-line mistake with no error message.

---

## 9. What this plan deliberately does not do

- It does not touch `ivh_vact_tick()`, `ivh_vact_capacity`, `ivh_vact_residual`,
  `ivh_vact_window_ns` or `ivh_vact_jump_threshold`. Part C stays available at `ivh_cap_source=2` as
  a third comparator and is otherwise untouched.
- It does not touch `ivh_ref_accumulate()`, `ivh_steal_source`, `ivh_ref_carry` or
  `ivh_ref_halt_correct`. The new signal *consumes* `ivh_ref_steal_ns`; it does not modify how it is
  produced.
- It does not touch Gate 2 (`ivh_preempt_event_source`), which is already TSC-native and working.
- It does not modify vcap or `custom_modules/vsched_module.c`. vcap stays byte-identical and
  authoritative until G6, which is what makes it a valid baseline.
- It does not retune `IVH_CAP_FLOOR` or `ivh_capacity_threshold`. Landing on vcap's scale is a design
  requirement, and G2 is the test of it.
