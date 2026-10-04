# Benchmark suite -- `git pull` + one script

```sh
cd tools/ivh/bench && ./setup_benchmarks.sh
```

| workload | where it comes from | notes |
|---|---|---|
| hackbench | **apt** `rt-tests` | `hackbench -T -g1 -f8 -l150000` |
| dbench | **apt** `dbench` | `dbench -t 15 16 -D /root/dbench_test` — **NO `-F`** |
| fs_mark | **apt** `fsmark` | must use `-n 30000`, not `-n 2000` (0.48s run) |
| sysbench | **apt** `sysbench` | |
| stress-ng | **apt** `stress-ng` | |
| **ebizzy** | **vendored here** (`ebizzy-0.3/`) | NOT packaged anywhere — it ships inside the rt-tests *source* tree, so apt gives you `hackbench` but no `ebizzy`. Needs a DISCARDED WARMUP per arm. |
| **NHextend-csmin** | **vendored here** | ours. csmin + pre-acquire stamp, AFL off. Wait metric is its own `Total wait time`. |
| memtier_benchmark | fetched (RedisLabs/memtier_benchmark) | server needs `hashpower>=15` at `-t 16` |
| PARSEC dedup/vips/bodytrack/canneal | fetched (cirosantilli/parsec-benchmark) | **17 GB** with native inputs — far too large to commit |

## Why these three categories

`ebizzy` is the one that bites people: it is absent from every distro, and the
binary everyone passes around came from building `utils/benchmark/ebizzy-0.3`
inside rt-tests. It is 36 KB of source, so it is vendored here and a pull plus
the setup script is enough.

PARSEC is 17 GB (14 GB of packages, 6.5 GB of native inputs). That can only be
a fetch. memtier is 40 MB of C++ with autotools — also a fetch, but fast.

## Per-workload traps

See `tools/ivh/ple/RUNBOOK.md` section 6 for the full list. The short version:
ebizzy needs a per-arm warmup (without it +52% reads as +0.57%), fsmark needs
`-n 30000`, dbench must drop `-F`, dedup needs a per-run `drop_caches` plus a
discarded warmup, vips is bimodal and needs ~50 pairs, and NHextend's wait must
come from its own userlock counter rather than `ivh_slowpath_wait_ns`.
