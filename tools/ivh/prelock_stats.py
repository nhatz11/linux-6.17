#!/usr/bin/env python3
"""Read IVH's per-CPU prelock/gate counters from /proc/kcore.

These are `static DEFINE_PER_CPU` in kernel/sched/fair.c with no reader
anywhere in the tree, but static symbols still land in kallsyms as 'd', and a
per-CPU symbol's kallsyms address is its offset within the percpu section --
so real address = __per_cpu_offset[cpu] + sym. Same trick capacity_line() in
run_campaign.sh uses for rq->ivh_uc_capacity. No kernel change, no reboot.

Answers: is ivh_eval_cooldown_ns (50us/vCPU) the binding constraint on
migration, i.e. is adding more lock entrances pointless?

  usage: prelock_stats.py            one-shot absolute
         prelock_stats.py <seconds>  delta over a window
"""
import sys, time
sys.path.insert(0, "/root/ivh_tools")
import read_vact_rq as r

COUNTERS = ["ivh_prelock_calls", "ivh_prelock_cooldown_skipped",
            "ivh_steal_imminent_capacity_reject",
            "ivh_steal_imminent_time_left_reject"]

def snap(f, ph, sym, offs):
    out = {}
    for name in COUNTERS:
        if name not in sym:
            out[name] = None; continue
        out[name] = sum(r.read_u64(f, ph, sym[name] + o) for o in offs)
    return out

def main():
    sym = r.load_kallsyms(); cpus = r.online_cpus()
    with open(r.KCORE, "rb") as f:
        ph = r.read_phdrs(f)
        offs = [r.read_u64(f, ph, sym["__per_cpu_offset"] + 8*c) for c in cpus]
        a = snap(f, ph, sym, offs)
        dur = float(sys.argv[1]) if len(sys.argv) > 1 else 0
        if dur:
            time.sleep(dur)
            b = snap(f, ph, sym, offs)
            d = {k: (b[k] - a[k]) if (a[k] is not None and b[k] is not None) else None
                 for k in a}
        else:
            d, dur = a, 1.0
    calls = d.get("ivh_prelock_calls") or 0
    skip  = d.get("ivh_prelock_cooldown_skipped") or 0
    print(f"window {dur:g}s   (summed over {len(cpus)} vCPUs)")
    for k in COUNTERS:
        v = d.get(k)
        print(f"  {k:38} {'n/a' if v is None else f'{v:14d}  {v/dur:12.0f}/s'}")
    if calls:
        pct = 100.0*skip/calls
        print(f"\n  cooldown_skipped / calls = {pct:.2f}%")
        passed = calls - skip
        print(f"  evaluations that PASSED the cooldown: {passed} ({passed/dur:.0f}/s)")
        cap = 20000*len(cpus)
        print(f"  theoretical cooldown cap at 50us/vCPU: {cap}/s")
        if pct > 80:
            print("\n  => COOLDOWN IS THE BINDING CONSTRAINT. Adding lock entrances")
            print("     cannot add migrations; it only changes which acquisition")
            print("     wins the slot. Widening coverage is pointless.")
        elif pct < 30:
            print("\n  => cooldown is NOT binding; coverage could matter.")
        else:
            print("\n  => partially binding; coverage has limited headroom.")
    else:
        print("\n  no prelock calls in window -- is ivh_universal_eligible=1 and a load running?")

main()
