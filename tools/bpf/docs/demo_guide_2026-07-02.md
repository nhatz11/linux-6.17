# IVH demo guide — 2026-07-02

## 1. Explaining the current workflow (say this, in order)

Three stages, one hook each:

```
ivh_pre_lock()                    kernel/locking/spinlock.c:46
  fires on every spin_lock() variant, before the lock is attempted
  → cheap early-exit gates (is IVH loaded, is this task eligible,
    is my last critical section long enough to bother, am I actually
    running, has enough time passed since my vCPU was last checked)
  → if all pass: calls bpf_sched_pre_lock_migrate()

bpf_sched_pre_lock_migrate()      kernel/sched/fair.c
  "should I migrate right now?"
  → Gate 1: is my source vCPU actually throttled (capacity)?
  → Gate 2: is a steal imminent (estimated time left in this burst)?
  → Gate 3: am I even allowed to move (cpumask weight > 1)?
  → Gate 4: are too many threads already mid-migration (concurrency cap)?
  → if all pass: calls into the BPF hook to pick a destination

process_cpu()                     tools/bpf/MY_ivh_atc.bpf.c
  "where should I go?" — scans candidate CPUs in ring order, applies
  compile-time gates (lockholder, capacity floor, preempted-heartbeat,
  burst-order, runqueue depth), picks the first that passes
  → back in bpf_sched_pre_lock_migrate(): one more live check
    (is_cpu_preempted on the chosen target) before actually committing
  → ivh_migrate_self() does the actual move (direct stop_one_cpu()
    dispatch, no set_cpus_allowed_ptr() overhead)
```

## 2. Seeing current threshold settings

```bash
sudo sysctl kernel.ivh_capacity_threshold      # source vCPU ceiling (0-1024 scale)
sudo sysctl kernel.ivh_time_left_threshold_ns  # Gate 2: min "burst time left" to skip migrating
sudo sysctl kernel.ivh_min_cs_ns               # last_cs_ns prefilter — skip IVH if own last CS was shorter than this
sudo sysctl kernel.ivh_eval_cooldown_ns        # per-vCPU rate limit on full evaluations
sudo sysctl kernel.ivh_max_concurrent          # concurrency cap (Gate 4)
sudo sysctl kernel.ivh_migration_timeout_ns    # watchdog (currently informational only)
sudo sysctl kernel.ivh_sched_timeout_ms        # legacy, from an earlier migration mechanism

# or all at once:
sudo sysctl -a 2>/dev/null | grep ivh_
```

To change any of them live, no rebuild needed:
```bash
sudo sysctl -w kernel.ivh_min_cs_ns=1000
```

## 3. Seeing live counters — `/proc/ivh_debug`

```bash
cat /proc/ivh_debug
```

Key fields to point at:
- `ivh_migrations_done` — cumulative completed migrations since boot
- `ivh_trylock_misses` — how often the destination-selection trylock lost
  a race (added this session — this counter existed but was silently dead
  before being wired up)
- `ivh_veto_count`, `ivh_veto_target_cap_avg`,
  `ivh_veto_target_still_capacity_healthy_pct` — the commit-time veto
  instrumentation added this session; the last field being ~100% is the
  live evidence for the tickless-idle false-positive bug that got fixed
- `ivh_prelock_calls` / `ivh_prelock_evaluated` / `ivh_prelock_skip_pct` —
  shows how much the cooldown + prefilter are actually cutting evaluation
  volume in real time

## 4. Seeing the rejection list — `reject_reasons` BPF map

This is the live breakdown of *why* `process_cpu()` rejected each
candidate CPU it considered, per gate:

```bash
sudo bpftool map dump name reject_reasons -j | \
  jq '[.[] | {key: .formatted.key, total: ([.formatted.values[].value] | add)}]'
```

Key mapping (from `tools/bpf/MY_ivh_atc.bpf.c`):
```
0  REJ_CPUMASK        not in task's own cpus_ptr
1  REJ_CLAIMED         already claimed by another IVH thread this tick
2  REJ_LOCKHOLDER      target's curr is inside a critical section
3  REJ_SPINNER         (currently unused gate)
4  REJ_CAPACITY_LOW    target capacity below the destination floor
5  REJ_NOT_BETTER      (currently unused gate)
6  REJ_PREEMPTED       target heartbeat stale
7  REJ_BURST_ORDER     target's active burst started earlier than source's
8  REJ_BURST_BUDGET    (currently unused gate)
9  ACC_TIER1_IDLE      accepted: genuinely empty runqueue
10 ACC_TIER2_NR1       accepted (fallback): exactly one runnable task on target
11 REJ_NR_RUNNING      target runqueue too deep (>=2), rejected
```

Also check which gates are compiled in right now:
```bash
grep "^#define GATE_" /home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc.bpf.c
```

And whether `test3`/`process_cpu` is actually running at all (useful sanity
check before trusting any of the above):
```bash
sudo bpftool prog show name test3
# look for "run_cnt" in the output — requires:
sudo sysctl kernel.bpf_stats_enabled=1   # once, if not already set
```

## 5. Running workloads

Standard pattern throughout this project: run once plain (baseline), once
through `ivh_exec` (IVH-enabled), compare. Always confirm real host
contention is present first (see step 6) — an uncontended comparison is
meaningless.

```bash
# hackbench — most LHP-sensitive invocation found this session
/usr/bin/hackbench -T -g 1 -f 8 -l 400000                    # baseline
/home/nick/ivh_exec /usr/bin/hackbench -T -g 1 -f 8 -l 400000 # IVH

# NHextend — project's own long-CS spinlock benchmark (needs -n or IVH
# never fires, needs -l to print the wait-time stats that matter)
/home/nick/NHextend -n -l                     # baseline
/home/nick/ivh_exec /home/nick/NHextend -n -l  # IVH
# key output lines: "Total wait time:" and "max wait:"

# dbench — filesystem/VFS lock contention
cd /home/nick/dbench_scratch
dbench -t 8 4                       # baseline
/home/nick/ivh_exec dbench -t 8 4    # IVH
# key output: "Throughput ... MB/sec", "max_latency="

# PARSEC — via parsecmgmt's -s (submit command) hook
/home/nick/parsec-benchmark/bin/parsecmgmt -a run -p dedup -i native -n 16 -c gcc -s "time"
/home/nick/parsec-benchmark/bin/parsecmgmt -a run -p dedup -i native -n 16 -c gcc -s "/home/nick/ivh_exec time"
```

**Always pair them** — alternate baseline/IVH runs, don't run a batch of
one then a batch of the other. Contention on the co-tenant VM drifts
significantly even within a single ~90s window; unpaired comparisons from
this project were repeatedly shown to be unreliable.

## 6. Confirming real contention before any demo run

```bash
sudo timeout 3 bpftrace -e '
kprobe:update_rq_clock
{
    @cap[cpu] = ((struct rq *)arg0)->cpu_capacity;
}
interval:s:2 { exit(); }' 2>&1 | sort -t'[' -k2 -n
```

Look for a spread — some CPUs well below 1024 (the healthy ceiling).
All-CPUs-near-1024 means the co-tenant sysbench isn't generating load
right now; results from that state aren't meaningful either way.
(If flat, restart `sysbench` on the co-tenant VM before demoing.)

## 7. Restarting IVH cleanly (needed after any BPF source edit)

```bash
ps -ef | grep -iE "ivh|vcap" | grep -v grep     # find current PIDs
sudo kill -9 <all the PIDs listed>               # kill by exact PID, not pkill
sleep 2
cd /home/nick && nohup ./IVH > /tmp/ivh.log 2>&1 &
disown
sleep 5
# then re-set any sysctls — they reset to compiled defaults on restart
```
