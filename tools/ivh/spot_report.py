#!/usr/bin/env python3
"""
spot_report.py <spot_*.tsv> [--bt <btdir>] [--comm NAME]

For one workload, computes the six quantities asked for on 2026-10-02:
migrations, cost, delay, syscall cost, saved, and cost/saved.

"TOTAL/run" is the per-RUN SUM of X, averaged over reps. That is what every
ratio below is built from -- totals over totals, never per-migration figures.
The "per migration" column is shown alongside for interpretation only.

SAVED is the one with a judgement in it. Both arms run for a FIXED wall time,
so the IVH arm does more work in the same seconds; comparing raw spin totals
would credit migration for work it also caused. PV's spin is therefore
normalised to the work the IVH arm actually did:

    saved = spin_pv * (ops_ivh / ops_pv)  -  spin_ivh

identical to (spin-per-op_pv - spin-per-op_ivh) * ops_ivh. The raw difference
is printed alongside, because for a saturated lock the two differ enormously:
total wait is pinned near (N-1)*duration by the run length, so the raw
difference is ~0 while the per-operation difference is large. That is exactly
why this workload wins in throughput without winning in total wait time.

Which counter is "the" spin time depends on where the lock is:
  ebizzy_mmap   kernel qspinlock slowpath, ivh_slowpath_wait_ns - _halt_ns.
                CAVEAT: QSPINLOCKS ONLY. ebizzy -m contends on mmap_lock, an
                rwsem, which this counter cannot see.
  nhextend_fin  its own userspace AFL lock, the benchmark's "Total wait time".
                With IVH_AFL_DISABLE=1 that lock pure-spins, so the field is
                pure spin with no sleep in it. The kernel counter is reported
                too, and is ~4 orders of magnitude smaller.
"""
import sys, os, re, glob, statistics as st


def parse_tsv(path):
    rows = []
    with open(path) as f:
        hdr = f.readline().rstrip("\n").split("\t")
        for line in f:
            if line.strip():
                rows.append(dict(zip(hdr, line.rstrip("\n").split("\t"))))
    return rows


def bt_scalar(txt, key):
    m = re.search(rf'^@{re.escape(key)}: (\d+)\s*$', txt, re.M)
    return int(m.group(1)) if m else None


def bt_keyed(txt, key, want):
    m = re.search(rf'^@{re.escape(key)}\[{re.escape(want)}\]: (\d+)\s*$', txt, re.M)
    return int(m.group(1)) if m else 0


def bt_map(txt, key):
    return {m.group(1): int(m.group(2))
            for m in re.finditer(rf'^@{re.escape(key)}\[([^\]]+)\]: (\d+)\s*$', txt, re.M)}


def nh_counters(btdir, m):
    """NHextend-fin prints its own mid-spin counters -- the userspace view of
    the same events, and an independent check on the bpftrace numbers."""
    sc, sns, mv, th, pl, dg, ck = [], [], [], [], [], [], []
    for f in sorted(glob.glob(os.path.join(btdir, "rep*_mig.out"))):
        t = open(f).read()
        v = re.search(r"syscalls made\s+: (\d+)\s+avg (\d+) ns\s+total ([0-9.]+) s", t)
        if v: sc.append(int(v.group(1))); sns.append(float(v.group(3)))
        v = re.search(r"\.\.\.CPU actually moved : (\d+)\s+\(([0-9.]+)%\)\s+to cpu>=8: (\d+)", t)
        if v: mv.append(int(v.group(1))); th.append(int(v.group(3)))
        v = re.search(r"polls\s+: (\d+)", t)
        if v: pl.append(int(v.group(1)))
        v = re.search(r"DANGER bit set\s+: (\d+)", t)
        if v: dg.append(int(v.group(1)))
        v = re.search(r"syscalls AVOIDED by filters: (\d+)", t)
        if v: ck.append(int(v.group(1)))
    if not sc:
        return None
    print()
    print("  NHextend-fin's own mid-spin counters (userspace view)")
    print(f"    polls                 {m(pl):12.0f}   DANGER bit set {m(dg):10.0f}"
          f"  ({100*m(dg)/m(pl):.2f}% of polls)")
    print(f"    syscalls SUPPRESSED by the 1 ms cooldown {m(ck):12.0f}"
          f"  ({100*m(ck)/max(m(dg),1e-9):.1f}% of danger hits)")
    print(f"    syscalls MADE         {m(sc):12.1f}   total {m(sns):8.4f} s"
          f"   per syscall {1e6*m(sns)/m(sc):8.2f} us   <-- avg total syscall cost")
    print(f"    ...CPU actually moved {m(mv):12.1f}   ({100*m(mv)/m(sc):.1f}% of syscalls landed a move)"
          f"   to cpu>=8: {m(th):.0f}")
    return dict(sys_total=m(sns), sys_n=m(sc), moved=m(mv))


def main():
    tsv = sys.argv[1]
    btdir, comm = None, None
    a = sys.argv[2:]
    for i, x in enumerate(a):
        if x == "--bt":   btdir = a[i + 1]
        if x == "--comm": comm = a[i + 1]

    rows = parse_tsv(tsv)
    W = rows[0]["workload"]
    if comm is None:
        comm = "ebizzy" if W == "ebizzy_mmap" else "NHextend-fin"
    userspace = (W == "nhextend_fin")

    pv  = [r for r in rows if r["arm"] == "pv"]
    mig = [r for r in rows if r["arm"] == "mig"]
    ops_pv  = [float(r["metric"]) for r in pv]
    ops_mig = [float(r["metric"]) for r in mig]

    if userspace:
        spin_pv  = [float(r["uwait_s"]) for r in pv]
        spin_mig = [float(r["uwait_s"]) for r in mig]
        basis = "userspace AFL spin (benchmark's own Total wait time)"
    else:
        spin_pv  = [int(r["spin_ns"]) / 1e9 for r in pv]
        spin_mig = [int(r["spin_ns"]) / 1e9 for r in mig]
        basis = "kernel qspinlock spin (slowpath wait - halt)"
    kspin_pv  = [int(r["spin_ns"]) / 1e9 for r in pv]
    kspin_mig = [int(r["spin_ns"]) / 1e9 for r in mig]
    migdone   = [int(r["migdone"]) for r in mig]

    m  = st.mean
    sd = lambda v: st.stdev(v) if len(v) > 1 else 0.0
    bene = 100 * (m(ops_mig) - m(ops_pv)) / m(ops_pv)

    print(f"================ {W}  ({os.path.basename(tsv)}) ================")
    print(f"reps={len(pv)}   comm={comm}   spin basis: {basis}")
    print()
    print(f"  throughput   PV  {m(ops_pv):12.1f}  (CV {100*sd(ops_pv)/m(ops_pv):5.2f}%)  {ops_pv}")
    print(f"               MIG {m(ops_mig):12.1f}  (CV {100*sd(ops_mig)/m(ops_mig):5.2f}%)  {ops_mig}")
    print(f"               benefit {bene:+.2f}%")
    print()

    norm      = m(ops_mig) / m(ops_pv)
    saved     = m(spin_pv) * norm - m(spin_mig)
    saved_raw = m(spin_pv) - m(spin_mig)
    print(f"  SAVED")
    print(f"    spin PV            {m(spin_pv):12.4f} s   ({1e3*m(spin_pv)/m(ops_pv):.4f} ms/op)")
    print(f"    spin MIG           {m(spin_mig):12.4f} s   ({1e3*m(spin_mig)/m(ops_mig):.4f} ms/op)")
    print(f"    PV normalised x{norm:.4f} -> {m(spin_pv)*norm:.4f} s")
    print(f"    SAVED (normalised) {saved:12.4f} s   <-- TOTAL/run; the ratio denominator")
    print(f"    saved, raw diff    {saved_raw:12.4f} s   (unnormalised)")
    if userspace:
        print(f"    kernel qspinlock spin, same runs: PV {m(kspin_pv):.4f} s  MIG {m(kspin_mig):.4f} s")
    else:
        print(f"    [qspinlock only; mmap_lock is an rwsem and is NOT counted here]")

    nh = nh_counters(btdir, m) if btdir else None

    if not btdir:
        print("\n  (no --bt dir: migration cost/delay unavailable)")
        return
    def rwsem(arm):
        w, r, wn, rn = [], [], [], []
        for f in sorted(glob.glob(os.path.join(btdir, f"rep*_{arm}.txt"))):
            t = open(f).read()
            w.append(bt_keyed(t, "rww_ns", comm) / 1e9)
            r.append(bt_keyed(t, "rwr_ns", comm) / 1e9)
            wn.append(bt_keyed(t, "rww_n", comm))
            rn.append(bt_keyed(t, "rwr_n", comm))
        return (w, r, wn, rn) if any(w) or any(r) else None

    rsp, rsm = rwsem("pv"), rwsem("mig")
    if rsp and rsm:
        tp = m([a + b for a, b in zip(rsp[0], rsp[1])])
        tm = m([a + b for a, b in zip(rsm[0], rsm[1])])
        rs_saved = tp * norm - tm
        print()
        print(f"  rwsem slowpath wait of {comm} -- the lock the qspinlock counter cannot see")
        print(f"    PV  write {m(rsp[0]):8.4f} s / {m(rsp[2]):9.0f} calls"
              f"   read {m(rsp[1]):8.4f} s / {m(rsp[3]):9.0f} calls   total {tp:8.4f} s")
        print(f"    MIG write {m(rsm[0]):8.4f} s / {m(rsm[2]):9.0f} calls"
              f"   read {m(rsm[1]):8.4f} s / {m(rsm[3]):9.0f} calls   total {tm:8.4f} s")
        print(f"    rwsem is {tm/max(m(kspin_mig),1e-12):6.1f}x the qspinlock figure")
        print(f"    SAVED on rwsem (normalised x{norm:.4f}) = {rs_saved:+.4f} s")
        if not userspace:
            print(f"    qspinlock+rwsem SAVED = {saved + rs_saved:+.4f} s   <-- the complete lock-wait picture")

    files = (sorted(glob.glob(os.path.join(btdir, "rep*_mig.txt")))
             or sorted(glob.glob(os.path.join(btdir, "rep*.txt"))))
    per = []
    for f in files:
        t = open(f).read()
        n = bt_keyed(t, "c_n", comm)
        if not n:
            continue
        per.append(dict(
            n=n, light=(bt_keyed(t, "c_cost", comm) == 0),
            cost=bt_keyed(t, "c_cost", comm), sel=bt_keyed(t, "c_sel", comm),
            mech=bt_keyed(t, "c_mech", comm), delay=bt_keyed(t, "c_delay", comm),
            calls=bt_scalar(t, "calls") or 0,
            sysn=bt_keyed(t, "c_sys_n", comm) or (bt_scalar(t, "sys_n") or 0),
            sysns=bt_keyed(t, "c_sys_ns", comm) or (bt_scalar(t, "sys_ns") or 0),
            trapn=bt_scalar(t, "sys_trap_n") or 0, trapns=bt_scalar(t, "sys_trap_ns") or 0,
            mign=bt_scalar(t, "sys_mig_n") or 0,  migns=bt_scalar(t, "sys_mig_ns") or 0,
            dest=bt_map(t, "dest_cpu")))
    if not per:
        print(f"\n  (bpftrace saw no migrations for comm={comm})")
        return

    AN    = m([p["n"] for p in per])
    LIGHT = per[0]["light"]
    ACT   = m([p["cost"]  for p in per]) / 1e9
    ASE   = m([p["sel"]   for p in per]) / 1e9
    AME   = m([p["mech"]  for p in per]) / 1e9
    ADL   = m([p["delay"] for p in per]) / 1e9
    ASY   = m([p["sysns"] for p in per]) / 1e9
    ASN   = m([p["sysn"]  for p in per])
    AC    = m([p["calls"] for p in per])

    print()
    print(f"  MIGRATIONS of {comm} threads, per run")
    print(f"    migrations TOTAL/run      {AN:12.1f}   (CV {100*sd([p['n'] for p in per])/AN:.2f}%)")
    print(f"    ivh_migrations_done delta {m(migdone):12.1f}   (global, all tasks -- cross-check)")
    if AC:
        print(f"    pre_lock_migrate calls    {AC:12.1f}   "
              f"-> {100*AN/AC:.3f}% of calls migrate")

    print()
    if LIGHT:
        ACT = AME
        print(f"  COST  committing the move -> on the target vCPU's runqueue")
        print(f"    cost  TOTAL/run           {ACT:12.4f} s   (per migration {1e6*ACT/AN:9.2f} us)")
        print(f"    [low-perturbation instrument: MECH only. The prologue (4 gates,")
        print(f"     my_spinlock trylock) and the cfs_select walk are excluded --")
        print(f"     probing those is what suppressed migrations ~3x.]")
    else:
        print(f"  COST  entering migration fn -> on the target vCPU's runqueue   [PERTURBED]")
        print(f"    cost  TOTAL/run           {ACT:12.4f} s   (per migration {1e6*ACT/AN:9.2f} us)")
        print(f"      SELECT (gates + cfs_select)       {1e6*ASE/AN:9.2f} us")
        print(f"      MECH   (sca -> set_task_cpu)      {1e6*AME/AN:9.2f} us")
        print(f"      identity residual {100*abs(ACT-(ASE+AME))/max(ACT,1e-12):.4f}%")

    print()
    print(f"  DELAY on the target runqueue -> running there")
    print(f"    delay TOTAL/run           {ADL:12.4f} s   (per migration {1e6*ADL/AN:9.2f} us)")

    ATN  = m([p["trapn"]  for p in per]); ATNS = m([p["trapns"] for p in per]) / 1e9
    AMN  = m([p["mign"]   for p in per]); AMNS = m([p["migns"]  for p in per]) / 1e9
    print()
    print(f"  SYSCALL ivh_cs_enter -- SPLIT (the whole-call duration is NOT the trap)")
    if not ASN:
        print(f"    ZERO syscalls made (no ivh_cs_enter call site reached)")
        TRAP_TOTAL = 0.0
    else:
        trap_each = ATNS/ATN if ATN else 0.0
        TRAP_TOTAL = trap_each * ASN
        print(f"    TRAP-only calls (no migration) {ATN:9.1f}   {ATNS:8.4f} s"
              f"   per call {1e6*trap_each:7.2f} us   <-- THE TRAP")
        print(f"    calls that BLOCKED thru a migration {AMN:7.1f}   {AMNS:8.4f} s"
              f"   per call {1e6*AMNS/AMN if AMN else 0:7.1f} us   (contains cost+delay)")
        print(f"    all calls {ASN:.1f}; trap paid by every call -> TRAP TOTAL/run {TRAP_TOTAL:.4f} s"
              f" ({100*TRAP_TOTAL/ASY if ASY else 0:.2f}% of whole-syscall time {ASY:.4f} s)")

    print()
    print(f"  ---- the ratio ----")
    print(f"    cost / saved  (TOTAL/TOTAL)= {ACT:.4f} s / {saved:.4f} s = {ACT/saved:+.4f}"
          f"   ({100*ACT/saved:+.2f}%)   <-- THE ratio")
    print(f"    [DO NOT SUM THESE -- they overlap. The ivh_cs_enter syscall BLOCKS in")
    print(f"     affine_move_task()'s wait_for_completion THROUGH the migration, so its")
    print(f"     duration already contains cost+delay (99.99% of syscall time is in the")
    print(f"     >16us mode; the true trap is 0.63us/call). And delay is not a system")
    print(f"     cost for a thread that was already waiting on the lock.]")
    print(f"    (cost+TRAP) / saved        = {(ACT+TRAP_TOTAL)/saved:+.4f}"
          f"   ({100*(ACT+TRAP_TOTAL)/saved:+.2f}%)   <-- legitimate: trap does NOT overlap cost")
    print(f"    (cost+delay) / saved       = {(ACT+ADL)/saved:+.4f}   <-- delay is not a system cost; do not quote")
    print(f"    (cost+delay+whole syscall) = {(ACT+ADL+ASY)/saved:+.4f}   <-- triple-counts; do not quote")
    if saved <= 0:
        print(f"    *** SAVED is <= 0: migration did not reduce spin time on this")
        print(f"        workload in this sitting, so the ratio has no meaning. ***")

    d = {}
    for p in per:
        for k, v in p["dest"].items():
            d[k] = d.get(k, 0) + v
    if d:
        tt = sum(d.values())
        hi = sum(v for k, v in d.items() if int(k) >= 8)
        print()
        print(f"  destination vCPUs: {dict(sorted(d.items(), key=lambda x: -x[1]))}")
        print(f"    to the top half (vCPU>=8): {hi}/{tt} = {100*hi/tt:.1f}%")


main()
