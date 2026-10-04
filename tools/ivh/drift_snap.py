#!/usr/bin/env python3
"""Snapshot for the sustained-load drift investigation (2026-09-15).
Prints one JSON line: time, per-CPU ivh_uc_capacity and cpu_capacity (struct rq
via /proc/kcore), reject_reasons totals (MY_ivh_atc BPF map), migration count."""
import json, subprocess, struct, sys, time
sys.path.insert(0, "/root/ivh_tools")
import read_vact_rq as r

NAMES = ["CPUMASK","CLAIMED","LOCKHOLDER","SPINNER","CAP_LOW","NOT_BETTER","PREEMPTED",
         "BURST_ORDER","BURST_BUDGET","ACC_T1_ACTIVE","ACC_T2_IDLE","USER_LOCKHOLDER"]
RQ = {"uc_cap": 3824, "cpu_cap": 2960}   # pahole -C rq vmlinux, G-LOCK-28/29

def bpf(name):
    out = subprocess.run(["bpftool", "map", "dump", "name", name], capture_output=True, text=True).stdout
    return json.loads(out)

def main():
    snap = {"t": time.time()}
    sym = r.load_kallsyms(); cpus = r.online_cpus()
    with open(r.KCORE, "rb") as f:
        ph = r.read_phdrs(f)
        offs = [r.read_u64(f, ph, sym["__per_cpu_offset"] + 8*c) for c in cpus]
        for k, o in RQ.items():
            snap[k] = [r.read_u64(f, ph, sym["runqueues"] + o + off) for off in offs]
    rej = {}
    for e in bpf("reject_reasons"):
        rej[NAMES[e["key"]]] = sum(v["value"] for v in e["values"])
    snap["rej"] = rej
    mig = 0
    for e in bpf("last_migration"):
        for v in e["values"]:
            mig += v["value"]["count"]
    snap["mig"] = mig
    busy = []
    for line in open("/proc/stat"):
        if line.startswith("cpu") and line[3].isdigit():
            v = list(map(int, line.split()[1:]))
            busy.append((sum(v) - v[3] - v[4], sum(v)))   # (non-idle jiffies, total)
    snap["stat"] = busy
    if len(sys.argv) > 1:
        snap["tag"] = sys.argv[1]
    print(json.dumps(snap), flush=True)

if __name__ == "__main__":
    main()
