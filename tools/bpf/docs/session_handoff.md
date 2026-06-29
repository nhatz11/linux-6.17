# IVH Verification Session Handoff

## Kernel State
- Running: `6.17.0-rseqport19-TRY+` (has trylock + debug instrumentation)
- Source: `linux-6.17-rseqport`, branch `rseq-port`
- Rebuild needed: **YES** — `kernel/sched/fair.c` was modified this session
  - `make -j$(nproc)` then install/reboot

---

## What Was Accomplished (Tasks 1–5)

### Task 3 — DONE ✓
`prove_wait_counter.sh` — kernel reads `wait_counter=0x4` with `depth=1` at tick time.

### Task 4 — DONE ✓
`rseq-state-check.c` — all 4 rseq ABI checks pass.

### Task 1 — DONE ✓ (userspace migration stamp of approval)
Root cause: NHextend pinned threads 1:1 via `sched_setaffinity` → `nr_cpus_allowed=1` → gate 3
blocked all IVH.

Fix: `-n` flag added to `/home/nick/NHextend.c`. With `-n`, migrations confirmed in
`/var/log/ivh_migrations.log`.

### Task 2 — DONE (measurement system verified, IVH comparison partial)
Dead code found: `lhp_last_class` has no writers — always zero. Script `verify_spinners_cstime.sh`
is stale.

Working approach: `NHextend -n -l 16` with `-l` flag prints per-thread CS stats.

**Key invariant confirmed**: `overall_ns >= active_ns` always holds.
- `overall_ns` (rseq+40) = CLOCK_MONOTONIC delta across lock hold
- `active_ns` (rseq+48) = CLOCK_THREAD_CPUTIME_ID delta across lock hold
- `overall - active` = off-CPU penalty = the LHP signal

WITHOUT IVH (heavy sysbench contention):
```
Global max overall : 27604057 ns  (27.6 ms)
Global max active  :  4127054 ns  (4.1 ms)
Max offcpu penalty : 23477003 ns  (23.5 ms — preemption mid-CS)
```

WITH IVH result: incomplete — see "Open Issue" below.

### Task 5 — NOT DONE
Migration latency profiling not run. Command ready:
```bash
sudo bpftrace -e '
kprobe:bpf_sched_pre_lock_migrate { @t[tid]=nsecs; @c[tid]=cpu; }
kretprobe:bpf_sched_pre_lock_migrate /@t[tid]/ {
    if (cpu != @c[tid]) @lat = lhist(nsecs-@t[tid], 0, 5000000, 50000);
    delete(@t[tid]); delete(@c[tid]);
}
interval:s:15 { print(@lat); exit(); }
'
```

---

## Open Issue: NHextend Hangs with 16 Threads + IVH Under Steal

### Symptom
`timeout 12 /home/nick/NHextend -n -l 16` never completes when IVH is active and
sysbench is running on another VM contending the same host cores.
4 threads sometimes completes, sometimes hangs.

### What Was Tried
1. Added `source_capacity` gate in `process_cpu()` — prevents migration to equally-throttled
   CPUs. Did not fix the hang.
2. Changed `raw_spin_lock_irqsave(&my_spinlock)` to `raw_spin_trylock_irqsave` — did not fix
   the hang. Reverted (trylock may not be the root cause).

### Current Hypothesis
Threads may be stuck in `schedule()` after `set_cpus_allowed_ptr(current, cpumask_of(target))`.
If the target CPU is being heavily stolen by sysbench, the migrated thread never gets a vCPU
slice and `schedule()` never returns.

### Debug Instrumentation Added (needs rebuild to activate)
`kernel/sched/fair.c` now has:

**Three exported atomic counters** (readable via bpftrace `kaddr` or `/proc/ivh_debug`):
```
ivh_in_schedule     — threads currently inside schedule() in bpf_sched_pre_lock_migrate
ivh_trylock_misses  — (unused now, trylock reverted)
ivh_migrations_done — completed migrations since boot
```

**`/proc/ivh_debug`** — single cat to read all counters + interpretation guide.

**`trace_printk` at key points**:
- `ivh_selected`  — after BPF picks a target (or -1): shows src, dst, capacity
- `ivh_pre_sched` — just before `schedule()`: shows how many threads are already in schedule()
- `ivh_post_sched`— after `schedule()` returns: shows which CPU we actually landed on

**How to use during a hang**:
```bash
# start NHextend with IVH, let it hang, then:
cat /proc/ivh_debug

# read the trace ring buffer:
sudo cat /sys/kernel/debug/tracing/trace | grep ivh | tail -50

# check if threads are stuck in schedule():
# if ivh_in_schedule > 0 for a long time → threads waiting for vCPU on target
# if ivh_in_schedule == 0 → hang is elsewhere (lock, TAS spin, etc.)
```

### Next Step to Diagnose
1. Boot the new kernel (rebuild needed)
2. Run `/home/nick/IVH` to start vcap + MY_ivh_atc
3. Enable tracing: `echo 1 > /sys/kernel/debug/tracing/tracing_on`
4. Run `timeout 12 /home/nick/NHextend -n -l 16` (let it hang)
5. In another shell: `cat /proc/ivh_debug` and `sudo cat /sys/kernel/debug/tracing/trace | grep ivh`
6. If `ivh_in_schedule` is stuck > 0: threads are in `schedule()` waiting for stolen vCPU
   → fix: add a `schedule_timeout` or check target CPU capacity is STILL good right before migrating
7. If `ivh_in_schedule == 0`: hang is in the TAS lock or elsewhere, IVH is not the cause

---

## Key Code Changes This Branch

| File | Change |
|------|--------|
| `/home/nick/NHextend.c` | `-n` flag (no CPU pinning), `last_cs_active_ns` via CLOCK_THREAD_CPUTIME_ID, max/avg CS tracking, `-l` stats output |
| `kernel/sched/fair.c` | `bpf_sched_pre_lock_migrate()`: removed gate 4 (`preempt_migrate_locked`); added `src_cpu` local; added `trace_printk` at 3 points; added 3 atomic debug counters; added `/proc/ivh_debug` proc file; deleted `running_migration()` (dead code) |
| `kernel/sched/sched.h` | Removed `extern running_migration` declaration |
| `tools/bpf/MY_ivh_atc.bpf.c` | Added `source_capacity` to `task_ctx`; EDWARDS-style good-enough gate in `process_cpu()`: `capacity > average_capacity OR capacity > 500` AND `capacity > source_capacity` |
| `tools/bpf/scripts/verify_cs_measurement.sh` | New script — verifies `overall >= active` for both userspace and kernel CS paths |

---

## IVH Runtime Notes
- Start: `sudo /home/nick/IVH` (starts vcap + MY_ivh_atc, rebuilds custom module if needed)
- Stop IVH only: `sudo kill $(ps aux | grep MY_ivh_atc | grep -v grep | awk '{print $2}')`
- Check running: `ps aux | grep -E "vcap|MY_ivh"` (don't use pgrep — hangs under load)
- Migration log: `/var/log/ivh_migrations.log`
- NHextend binary: `/home/nick/NHextend` (use `-n -l 16` for unpinned 16-thread CS stats)

---

## Key Invariant
```
overall_ns (CLOCK_MONOTONIC, wall-clock CS duration)
  >= active_ns (CLOCK_THREAD_CPUTIME_ID, on-CPU CS duration)

overall - active = off-CPU penalty during lock hold = the LHP signal

Without IVH: max can reach 20-25ms under host contention (catastrophic)
With IVH:    threads pre-migrate to healthy vCPU → max should approach 0
             BUT: hang observed at 16 threads — root cause not yet confirmed
```
