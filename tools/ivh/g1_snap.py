#!/usr/bin/env python3
"""
G-LOCK-50 / G1: snapshot the Gate-2 diagnostic counters as JSON.

Counters are cumulative from boot, so the G1 question ("what does Gate 2's
input distribution look like UNDER LOAD") needs a before/after delta. A
single read is dominated by whatever the machine did since boot -- including
the postboot ebizzy canary and the multi-minute capacity settling wait, both
of which are idle-ish and would drag the histogram toward the probe's own
burst profile.

Usage:  g1_snap.py out.json          # take a snapshot
        g1_snap.py --diff a.json b.json   # print b-a with the G1 verdict
"""
import importlib.util, json, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "ric", os.path.join(HERE, "read_ivh_counters.py"))
ric = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ric)

# kallsyms per-CPU symbols only. NOT struct rq fields -- ivh_vact_jumps and
# friends live in struct rq and are read by read_vact_rq.py via pahole
# offsets, not by this path; listing one here resolves to nothing and
# (before this fix) tripped the "wrong kernel" abort on a correct kernel.
COUNTERS = ["ivh_act_hist", "ivh_g2_act_hist", "ivh_g2_eval",
            "ivh_g2_zero_input", "ivh_steal_imminent_capacity_reject",
            "ivh_steal_imminent_time_left_reject", "ivh_prelock_calls",
            "ivh_prelock_cooldown_skipped"]

# Only these four decide "is this a G-LOCK-50 kernel". Anything else absent
# is a missing optional counter, not a wrong boot.
REQUIRED = ["ivh_act_hist", "ivh_g2_act_hist", "ivh_g2_eval",
            "ivh_g2_zero_input"]


def snap():
    out, sym = {}, ric.load_kallsyms()
    cpus = ric.online_cpus()
    missing = []
    # Reopen /proc/kcore per snapshot -- a held-open fd serves frozen,
    # page-cached values and has faked a reading in this project twice.
    with open(ric.KCORE, "rb") as f:
        phdrs = ric.read_phdrs(f)
        base_off = sym["__per_cpu_offset"]
        offs = [ric.read_u64(f, phdrs, base_off + 8 * c) for c in cpus]
        for name in COUNTERS:
            if name not in sym:
                missing.append(name)
                continue
            if name in ric.ARRAY_COUNTERS:
                out[name] = ric.read_array_counter(
                    f, phdrs, sym[name], offs, ric.ARRAY_COUNTERS[name])
            else:
                out[name] = sum(ric.read_u64(f, phdrs, sym[name] + o)
                                for o in offs)
    if missing:
        sys.stderr.write("note: absent from kallsyms: %s\n" % ", ".join(missing))
    req = [m for m in missing if m in REQUIRED]
    if req:
        sys.stderr.write("FATAL: G-LOCK-50 counters absent: %s\n"
                         % ", ".join(req))
        sys.exit(2)
    return out


def sub(a, b):
    d = {}
    for k, v in b.items():
        if k not in a:
            d[k] = v
        elif isinstance(v, list):
            d[k] = [x - y for x, y in zip(v, a[k])]
        else:
            d[k] = v - a[k]
    return d


def main():
    if sys.argv[1:2] == ["--diff"]:
        a = json.load(open(sys.argv[2]))
        b = json.load(open(sys.argv[3]))
        d = sub(a, b)
        for name in ("ivh_act_hist", "ivh_g2_act_hist"):
            if name in d:
                ric.print_age_hist(name, d[name])
                print()
        for k, v in d.items():
            if not isinstance(v, list):
                print(f"{k:38s} = {v}")
        ric.print_g50_summary(d)
    else:
        data = snap()
        json.dump(data, open(sys.argv[1], "w"))
        print(f"snapshot -> {sys.argv[1]}")


if __name__ == "__main__":
    main()
