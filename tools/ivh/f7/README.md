# F7 — performance vs wait cost, PV+T1 vs MIG+T1

Arms: **tier1 ON in both**, tier2/evict/skip OFF in both, `spin_threshold=32768`
in both. Migration is the only difference: `ivh_time_left_threshold_ns=2500000`,
`ivh_cs_gate2_reference=1` (csmin), `rcu_guard=0`, `preempt_event_source=2`.
n=4 per arm (nhextend n=5), warm-up discarded after every arm switch, page cache
dropped before every dedup run, block order alternated.

## Definitions

    spin      = ivh_slowpath_wait_ns - ivh_slowpath_halt_ns
    mig cost  = landed migrations x MECH        (MECH = t_onrq - t_commit)
    wait cost = spin + mig cost

`halt` is a strict subset of `wait` (its gate is the flag `wait_begin()` sets),
so the subtraction is sound — same gate, same clock (G-LOCK-53).
MECH is measured with `migcost_light.bt`. **Never** add DELAY or the
`ivh_cs_enter` duration: a migrating syscall blocks through the move, so its
duration already contains cost+delay.

## BOTH normalisations are stored; they answer different questions

| scheme | question | columns |
|---|---|---|
| **PER-ACQ** (headline) | does each contended acquisition cost less, including the migration paid for it? | `PERACQ_*` |
| **MIXED** (kept, not used) | did we burn less CPU in total for this job? fixed-work subtract, fixed-time normalise to mig's work | `MIXED_*` |

They agree on every FIXEDTIME workload and diverge only on dedup and fsmark,
whose acquisition counts explode under migration (+60% and +5,473%). PER-ACQ is
positive on all six (+15.7% to +91.8%); MIXED is positive on four of six.

**Do not present PER-ACQ as if it were total spin.** On fsmark total spin rises
122.7 -> 553.3 ms and on dedup 201.3 -> 103.1 ms with a 167.6 ms migration bill.
Both are under 3% of CPU time, and both workloads gain 193% / 89% throughput
from a mechanism that is not lock-wait reduction.

## Instrument per workload — NOT uniform, and that is deliberate

`ivh_slowpath_wait_ns` counts kernel qspinlocks only, outside interrupt context.
- hackbench, fsmark, dedup — correct
- **ebizzy** — LOWER BOUND; it serialises on `mmap_lock`, an rwsem the counter cannot see (1,281 ms qspinlock vs 148,776 ms rwsem)
- **memtier** — undercounts; its halts come from softirq, excluded by `!in_interrupt()`
- **nhextend** — the kernel counter reads 118 events / 0.7 ms (noise, and it points the wrong way). Uses the benchmark's own `Total wait time` instead.

## A second result worth reporting

Migration does not merely shorten spinning — it stops waiters blocking at all.
Halt share of wait, PV -> MIG: hackbench 25.4% -> 0.0%, memtier 37.0% -> 0.7%,
fsmark 15.6% -> 0.0%, dedup 5.9% -> 0.0%.

## Files

| file | what |
|---|---|
| `f7_full_data.csv` | every workload, both schemes, all raw terms |
| `raw_spinuni.tsv` | the uniform spin run (wait, halt, events, migrations per rep) |
| `raw_nhextend_spot.tsv` | nhextend's userspace-lock run (#9's source) |
| `raw_waitcost2_superseded.tsv` | earlier pass that measured `wait` without subtracting halt — SUPERSEDED |
| `mech.log`, `mech2.log` | migcost_light MECH measurements, all six workloads |
