# IVH command runbook — every workload, on/off, HP%, CS length

Copy-pasteable reference for reproducing every measurement in this session's
reports. All commands assume: repo built, `ivh_exec` compiled at repo root,
`vcap` and `MY_ivh_atc` daemons running, standard sysctls set (below).

## 0. One-time setup per boot / per session

```bash
# Daemons (restart protocol: always pkill any existing instance first)
pkill -f MY_ivh_atc; pkill -f "vcap -p"
cd /home/nick/kernels/linux-6.17-rseqport/tools/bpf
nohup sudo /home/nick/vsched_main/vcapacity/vcap -p 200 -s 5000 \
  > /home/nick/ivh_logs/vcap.log 2>&1 &
disown
nohup sudo ./MY_ivh_atc > /home/nick/ivh_logs/ivh_atc.log 2>&1 &
disown

# Standard validated sysctls (as of 2026-07-20; now also the compiled
# defaults in kernel/sched/bpf_sched.c -- a rebuild will pick these up,
# but they can always be set live without one):
echo 1010    | sudo tee /proc/sys/kernel/ivh_capacity_threshold
echo 4000000 | sudo tee /proc/sys/kernel/ivh_time_left_threshold_ns   # 4ms
echo 8       | sudo tee /proc/sys/kernel/ivh_max_concurrent
echo 1       | sudo tee /proc/sys/kernel/ivh_time_left_source
echo 0       | sudo tee /proc/sys/kernel/ivh_selection_trylock        # blocking lock
echo 0       | sudo tee /proc/sys/kernel/ivh_migrate_mechanism         # set_cpus_allowed_ptr, no schedule()
echo 0       | sudo tee /proc/sys/kernel/ivh_hot_threads_enabled       # gate off, ungated wins in every test
```

## 1. Turning IVH on/off for one workload

`ivh_exec` (repo root) wraps a command with the per-process bits. It needs no
sudo and does not require BPF.

```bash
echo 1 | sudo tee /proc/sys/kernel/ivh_universal_eligible   # global switch ON

./ivh_exec -v    <cmd> [args...]   # IVH ON  for this process, with HP% stats
./ivh_exec -v -n <cmd> [args...]   # IVH OFF for this process (ivh_exclude), with HP% stats
./ivh_exec       <cmd> [args...]   # IVH ON,  no stats (same as unwrapped once universal=1)
./ivh_exec -n    <cmd> [args...]   # IVH OFF, no stats

echo 0 | sudo tee /proc/sys/kernel/ivh_universal_eligible   # restore after testing
```

`-v`'s stats block reports `Host-preempted CS cycles : stolen / total (pct%)`
— this is HP% for exactly the wrapped process (kernel's own `ivh_observe`
per-task counters, `/proc/ivh_debug`'s `ivh_obs_total_holds` /
`ivh_obs_stolen_holds`, not a separate BPF trampoline). **Caveat**: these are
global per-CPU counters, not TGID-scoped — run one measured workload at a
time.

## 2. Per-benchmark commands (exact invocations used in every report)

| benchmark | exact command | metric parsed | notes |
|---|---|---|---|
| ebizzy mmap | `ebizzy -S 20 -t 16 -m` | `records/s` (1st line) | `-m` = mmap/munmap mode, real `mmap_lock` exposure |
| ebizzy malloc | `ebizzy -S 20 -t 16` | `records/s` | no `-m`, userspace malloc/memcpy, ~no kernel lock exposure |
| hackbench g4 | `hackbench -g 4 -l 30000` | `Time:` (last line) | low oversubscription, real slack |
| hackbench g20 | `hackbench -g 20 -l 8000` | `Time:` | 50x oversubscription, protection backfires here |
| dbench fsync 16c | `dbench -F -t 20 -D /home/nick/dbench_work -c /usr/share/dbench/client.txt 16` | `Throughput ... MB/sec` | `-F`=fsync each op, real disk-backed dir |
| dbench fsync 8c | same, trailing arg `8` | MB/sec | fewer clients, less collateral |
| dbench tmpfs 16c | `dbench -t 20 -D /dev/shm/dbench_work -c /usr/share/dbench/client.txt 16` | MB/sec | no `-F`, tmpfs-backed — regime flips sign, see report |
| swaptions | `swaptions -ns 256 -sm 500000 -nt 16` (parsec-benchmark `apps/swaptions/inst/.../bin/swaptions`) | wall time | negligible kernel-lock exposure — not really an LHP case |
| dedup | `dedup -c -p -v -t 16 -i <FC-6-x86_64-disc1.iso> -o /home/nick/dedup_out.ddp` (parsec `kernels/dedup`, native input) | wall time | pthread mutex + condvar queues, bimodal CS (see §3) |
| vips | `cd .../vips/run && vips im_benchmark orion_18000x18000.v output.v` (parsec `apps/vips`, native input, **must run from its own `run/` dir**) | wall time | pthread mutex, tight short CS |
| memtier/redis | `redis-server --daemonize yes --logfile /tmp/redis-memtier.log && sleep 1 && memtier_benchmark --server=localhost --port=6379 --protocol=redis --threads=16 --clients=10 --test-time=30` then `sudo pkill redis-server \|\| true` | `ops/sec` (Totals line) | server is single-threaded — protecting client threads doesn't fix the real bottleneck |
| pbzip2 | `pbzip2 -c -p8 <compressible_input> > /dev/null` | wall time | neutral/noisy, near-zero lock exposure |

Wrap any of the above in `ivh_exec -v` / `ivh_exec -v -n` per §1 for a
paired HP%+throughput measurement. Always run **≥3 rounds**, and prefer
≥20s per round — `vcap`'s steal/capacity refresh is ~5-6s; shorter or
single-round tests have produced real, confirmed outlier results multiple
times this session.

## 3. Measuring real critical-section (lock hold) duration, not just HP%

HP% (host-preempted holds) tells you how often IVH's protection fires. It
does **not** tell you how long the underlying lock is normally held — that
needs separate, per-workload dynamic tracing (`bpftrace`, no kernel rebuild,
no code changes). The exact primitive differs per workload; commands below
match what was actually measured for the CS-length report:

```bash
# ebizzy mmap -- mmap_lock hold time via the kernel's own tracepoints
sudo bpftrace -e '
tracepoint:mmap_lock:mmap_lock_acquire_returned
/comm == "ebizzy" && args->success/ { @start[tid] = nsecs; }
tracepoint:mmap_lock:mmap_lock_released
/comm == "ebizzy"/ {
  if (@start[tid]) {
    @hold = hist(nsecs - @start[tid]); @cnt = count(); @sum = sum(nsecs - @start[tid]);
    delete(@start[tid]);
  }
}'

# dedup / vips -- pthread_mutex hold time, with pthread_cond_wait time
# subtracted (both use condvars; cond_wait atomically drops the mutex while
# blocked, so a naive lock->unlock span overcounts hold time)
LIBC=/lib/x86_64-linux-gnu/libc.so.6
sudo bpftrace -e '
uretprobe:'"$LIBC"':pthread_mutex_lock
/comm == "dedup" && retval == 0/ { @start[tid] = nsecs; @cw[tid] = (uint64)0; }
uprobe:'"$LIBC"':pthread_cond_wait
/comm == "dedup" && @start[tid]/ { @cwstart[tid] = nsecs; }
uretprobe:'"$LIBC"':pthread_cond_wait
/comm == "dedup" && @cwstart[tid]/ { @cw[tid] += (nsecs - @cwstart[tid]); delete(@cwstart[tid]); }
uprobe:'"$LIBC"':pthread_mutex_unlock
/comm == "dedup" && @start[tid]/ {
  $held = (nsecs - @start[tid]) - @cw[tid];
  @hold = hist($held); @cnt = count(); @sum = sum($held);
  delete(@start[tid]); delete(@cw[tid]);
}'
# (swap comm == "dedup" -> "vips" for vips)

# dbench -- fsync syscall duration (proxy for the held-lock/blocking window;
# not an exact single-lock hold time, but the tightest available signal)
sudo bpftrace -e '
tracepoint:syscalls:sys_enter_fsync
/comm == "dbench"/ { @start[tid] = nsecs; }
tracepoint:syscalls:sys_exit_fsync
/comm == "dbench" && @start[tid]/ {
  @hold = hist(nsecs - @start[tid]); @cnt = count(); @sum = sum(nsecs - @start[tid]);
  delete(@start[tid]);
}'

# hackbench -- kernel mutex hold time (dominated by AF_UNIX stream socket
# u->iolock in net/unix/af_unix.c's unix_stream_sendmsg/recvmsg)
sudo bpftrace -e '
kprobe:mutex_lock
/comm == "hackbench"/ { @start[tid] = nsecs; }
kprobe:mutex_unlock
/comm == "hackbench" && @start[tid]/ {
  @hold = hist(nsecs - @start[tid]); @cnt = count(); @sum = sum(nsecs - @start[tid]);
  delete(@start[tid]);
}'
```

Run the matching workload (unwrapped, no `ivh_exec` needed — this measures
the lock itself, not IVH's effect on it) concurrently or right after starting
each trace; `@sum / @cnt` from the printed output (or `bpftrace`'s own
auto-printed maps on exit/`END`) gives the mean; the histogram gives the
distribution shape (median, tail weight).

Real numbers from this session (2026-07-20): ebizzy mmap ~7.47µs mean,
dbench fsync ~1.20ms mean, hackbench g4 mutex ~2.32µs mean, vips mutex
~1.72µs mean, dedup mutex ~278.7µs mean (heavy right tail off a ~5-6µs
median — see the CS-duration addendum in the state-of-the-art doc for the
full analysis and why this **disproves** a clean "winners have longer CS"
story).

## 4. Quick sanity checks before trusting any run

```bash
cat /proc/sys/kernel/ivh_universal_eligible   # 1 during a protected run, 0 otherwise
cat /proc/ivh_debug | grep ivh_obs             # counters should move during -v runs
ps -ef | grep -E "MY_ivh_atc|vcap" | grep -v grep   # daemons alive, no duplicates
```
