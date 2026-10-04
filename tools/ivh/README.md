# IVH benchmark harnesses

Measurement tooling for the IVH lock-holder-preemption work: arming scripts,
A/B harnesses, bpftrace instruments, and the measured data they produced.

## Porting to a new machine

1. `cp hostenv.sh.example hostenv.sh` and fill it in. `hostenv.sh` is gitignored
   because it holds the hypervisor password. Scripts that reach the host or the
   co-runner VM expect `IVH_HOST`, `IVH_HOST_PASS`, `IVH_CORUNNER` from it.
2. Paths that are still machine-specific and need editing:
   - `/home/nick/Desktop/ebizzy` — the ebizzy binary
   - `/root/parsec-benchmark` — PARSEC install (dedup, vips, …)
   - `/root/memtier_benchmark/memtier_benchmark`
   - `/root/linux-6.17/NHextend-*` — the extend-sched binaries
   - `/root/kernels/linux-6.17-vanilla` — the kernel tree
3. Pin the guest's vCPUs to ONE NUMA node on the host. Spanning two nodes was
   measured to inflate spin CV from 6.3% to 17.7% and flip verdicts. Pinning
   does NOT survive a guest reboot — `postboot.sh` checks and repairs it.
4. Run `postboot.sh` after every boot. Calibration sysctls do not persist, and
   boot defaults flatten the steal estimator to zero.

## The config authority

`campaign/benchmarks.tsv` is the source of truth for workload invocations.
Taking an invocation from any other script has produced wrong configs before.

## Key harnesses

| script | what it does |
|---|---|
| `pvbase.sh` | stock-PV baseline: tier1 on, tier2/evict/skip off, no migration |
| `p7v2_arm.sh <thresh_ns>` | migration arm. NOTE: leaves tier2 ON despite its comment |
| `postboot.sh` | post-reboot state restore + pinning repair + write-readback sysctl probe |
| `parsec_ab.sh` | PARSEC A/B. Drops the page cache before EVERY run and discards a warmup |
| `waitcost2.sh` | performance vs wait-cost table, pv+t1 vs mig+t1 |
| `migcost_light.bt` | migration MECH/DELAY decomposition, low perturbation |
| `ivh_state.sh` | snapshot/restore/verify all sysctls + behavioural fingerprint |

## Measurement traps that cost real time

- **`migcost.bt` perturbs what it measures.** Its kprobe on
  `bpf_sched_pre_lock_migrate` (~48k/s) moved migrations/run 1071 -> 399 and the
  headline +17.5% -> +7.0%. Use `migcost_light.bt` for mechanism terms and take
  headlines from uninstrumented runs.
- **Never sum migration cost + delay + syscall time.** A migrating
  `ivh_cs_enter` blocks through the move, so its duration already contains
  cost+delay.
- **`ivh_slowpath_wait_ns` counts kernel qspinlocks only**, and only outside
  interrupt context. It is the wrong instrument for ebizzy (rwsem), NHextend
  (its own userspace lock — reads literally 0) and partly memtier (softirq).
- **ebizzy needs a discarded warmup after every arm switch.** Without one,
  migrations drop ~3x and a +52% win reads as +0.57%.
- **Ratio-of-means, never mean-of-ratios**, on any workload whose baseline
  spreads (dedup 17-194s, vips 256x).
- **A sysctl that reads fine can still reject every write.** Write and read back.
