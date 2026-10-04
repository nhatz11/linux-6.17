#!/usr/bin/env python3
"""
Read IVH per-CPU u64 counters live from /proc/kcore, summed across all
online CPUs. Same ELF-phdr-mapping technique used earlier this session for
pv_ops verification. Re-reads /proc/kallsyms fresh every invocation, so it's
immune to KASLR changing across reboots.
"""
import struct, sys, os

KCORE = "/proc/kcore"
KALLSYMS = "/proc/kallsyms"

DEFAULT_COUNTERS = [
    "ivh_pv_wait_calls",
    "ivh_wake_hypercall",
    "ivh_wake_ipi",
    "ivh_wait_pv_halt_irqoff",
    "ivh_wait_pv_halt_irqon",
    "ivh_wait_ipi_halt_irqon",
    "ivh_wait_irqoff_nohalt",
    "ivh_wait_vanilla_nopv_spin",
    "ivh_wake_vanilla_nopv_noop",
    "ivh_beat_tier1_fired",
    # G-LOCK-50 diagnostic: Gate 2's input distribution and firing rates.
    "ivh_act_hist",
    "ivh_g2_act_hist",
    "ivh_g2_eval",
    "ivh_g2_zero_input",
    "ivh_steal_imminent_capacity_reject",
    "ivh_steal_imminent_time_left_reject",
    "ivh_beat_tier2_checked",
    "ivh_beat_tier2_fired",
    "ivh_earlybail_suppressed",
    "ivh_irqoff_halt_used",
    "ivh_slowpath_wait_ns",
    "ivh_slowpath_wait_events",
    # G-LOCK-53: halt measured with the SAME gate+clock as the wait above, so
    # spin = wait_ns - halt_ns is a subset subtraction and cannot go negative.
    "ivh_slowpath_halt_ns",
    "ivh_slowpath_halt_events",
    "ivh_tier1_confirm_checked",
    "ivh_tier1_confirm_agreed",
    "ivh_tier1_confirm_disagreed",
    "ivh_tier1_suppressed",
    "ivh_halt_from_node",
    # handoff-time rotation probe (Phase 0, detect-only)
    "ivh_rot_handoffs",
    "ivh_rot_preempted",
    "ivh_rot_no_live",
    "ivh_rot_tail_stop",
    # Phase 0b: lock idle time. The arrays ARE the primary output -- without
    # them a default run prints none of the result.
    "ivh_rot_depth_hist",
    "ivh_rot_idle_events",
    "ivh_rot_idle_cycles",
    "ivh_rot_idle_hist",
    "ivh_rot_idle_unknown",
    "ivh_rot_idle_backward",
    "ivh_rot_idle_capped",
    "ivh_rot_steals",
    # Phase 1: rotation decision outcomes. splice_ok is the TRUE addressable
    # opportunity count (stale successor + live node behind it + that live
    # node's own ->next non-NULL, i.e. the splice is legal); ivh_rot_depth_hist
    # only ever proved the first two and therefore overstates it. splice_ok
    # counts whenever the walk runs, so it is measurable under
    # ivh_pv_rot_probe alone, with ivh_pv_rot_enable still 0.
    "ivh_rot_splice_ok",
    "ivh_rot_splice_done",
    "ivh_rot_splice_blocked_tail",
    "ivh_rot_splice_blocked_starve",
    # is_cs_preempted() Stage A (detect only). See <asm/ivh_tsc_beat.h> and
    # tools/bpf/docs/ivh_is_cs_preempted_build_plan_2026-09-14.md sec 5 for
    # the two exact partitions these must satisfy.
    "ivh_cs_stamps",
    "ivh_cs_clears",
    "ivh_cs_stamp_overwrote",
    "ivh_cs_check_calls",
    "ivh_cs_abstain_noprev",
    "ivh_cs_abstain_rot",
    "ivh_cs_abstain_tag",
    "ivh_cs_abstain_skew",
    "ivh_cs_abstain_young",
    "ivh_cs_abstain_nohz",
    "ivh_cs_long_hold",
    "ivh_cs_healthy_long",
    "ivh_cs_fired",
    "ivh_cs_ep_events",
    "ivh_cs_abstain_tenure",
    "ivh_cs_abstain_hashed",
    "ivh_cs_abstain_late",
    "ivh_cs_abstain_retag",
    "ivh_cs_tenure0_enter",
    "ivh_cs_tenure0_hashed",
    "ivh_cs_tenure0_hashed_released",
    "ivh_cs_tenure0_late",
    "ivh_cs_shadow_gate_pass_released",
    # is_cs_preempted() Stage B (compiled in, inert unless ivh_cs_head_bail=1).
    "ivh_cs_head_bailed",
    "ivh_head_spin_iters_bail_sum",
    "ivh_head_spin_bail_attempts",
]

# G-LOCK-25: enum pv_bail_cause (arch/x86/include/asm/ivh_tsc_beat.h) order --
# index 0 (PV_BAIL_NONE) should always sum to 0 by construction, kept in the
# label list purely so index arithmetic below matches the enum exactly.
PV_BAIL_CAUSE_NAMES = [
    "NONE", "TIER1", "TIER1_AGREED", "TIER1_DISAGREED", "TIER2", "EXHAUST",
]
IVH_BEAT_AGE_HIST_BUCKETS = 32
IVH_ROT_HOP_CAP = 8   # must match IVH_ROT_HOP_CAP in <asm/ivh_tsc_beat.h>
IVH_CS_EP_NR = 3            # must match IVH_CS_EP_NR in <asm/ivh_tsc_beat.h>
IVH_CS_EP_NAMES = ["ACQUIRED", "HOLDER_CHANGED", "EXHAUST"]
IVH_CS_HALT_NR = 2          # must match IVH_CS_HALT_NR in <asm/ivh_tsc_beat.h>
IVH_CS_HALT_NAMES = ["EXHAUST", "CS"]
IVH_CS_TENURE_NAMES = ["no detection", "detected"]

# Phase 0b idle-time classes == (pv_node.rot_flags & 0x3). Index 2 is
# SKIPPABLE-without-STALE, which pv_handoff_rotate() can never produce; it is
# kept so the index arithmetic matches the flag bits exactly, and it must read
# zero in every run -- a nonzero value there means the deposit is corrupt.
IVH_ROT_CLASS_NAMES = [
    "live (baseline)", "stale, nowhere to skip", "IMPOSSIBLE", "stale + skippable",
]

# name -> tuple of dimension sizes, outermost first. Per-CPU arrays are laid
# out contiguously per CPU (element i at base + i*8 + per_cpu_offset[cpu]),
# so each flattened index is summed across CPUs independently.
ARRAY_COUNTERS = {
    # G-LOCK-41 verdict audit. Index [irqs_disabled()][ivh_vact_preempt_since()]
    # where the second index is 0=not preempted, 1=preempted, 2=ambiguous window.
    "ivh_cs_v_flagged": (2, 4),
    "ivh_cs_v_unflagged": (2, 4),
    "ivh_evict_v": (4,),
    "ivh_evict_gap_hist": (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_cs_hold_by_flag":     (2, IVH_BEAT_AGE_HIST_BUCKETS),
    "ivh_cs_react":            (6, IVH_BEAT_AGE_HIST_BUCKETS),  # [had_tail*3+state][bucket]
    "ivh_skipcheck":           (2, IVH_BEAT_AGE_HIST_BUCKETS),  # [was_SKIPPED][gap_bucket]
    "ivh_evict_age_used_hist": (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_evict_age_true_hist": (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_evict_cpubeat_hist": (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_node_halt_cycles": (len(PV_BAIL_CAUSE_NAMES),),
    "ivh_node_halt_events": (len(PV_BAIL_CAUSE_NAMES),),
    "ivh_node_halt_hist": (len(PV_BAIL_CAUSE_NAMES), IVH_BEAT_AGE_HIST_BUCKETS),
    # index 0..HOP_CAP-1 = depth of first live waiter; index HOP_CAP = none found
    "ivh_rot_depth_hist": (IVH_ROT_HOP_CAP + 1,),
    "ivh_rot_idle_cycles": (len(IVH_ROT_CLASS_NAMES),),
    "ivh_rot_idle_events": (len(IVH_ROT_CLASS_NAMES),),
    "ivh_rot_idle_hist": (len(IVH_ROT_CLASS_NAMES), IVH_BEAT_AGE_HIST_BUCKETS),
    # is_cs_preempted() Stage A
    "ivh_cs_ep_events_by_end": (IVH_CS_EP_NR,),
    "ivh_cs_ep_cycles":        (IVH_CS_EP_NR,),
    "ivh_cs_ep_hist":          (IVH_CS_EP_NR, IVH_BEAT_AGE_HIST_BUCKETS),
    "ivh_cs_tenure_cycles":    (2,),
    "ivh_cs_tenure_hist":      (2, IVH_BEAT_AGE_HIST_BUCKETS),
    "ivh_cs_prev_hold_hist":   (IVH_BEAT_AGE_HIST_BUCKETS,),
    # G-LOCK-50: Gate 2's burst-length population, from ivh_vact_tick().
    "ivh_act_hist":            (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_g2_act_hist":         (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_cs_prompt_hist":      (IVH_BEAT_AGE_HIST_BUCKETS,),
    # is_cs_preempted() Stage B
    "ivh_head_halt_cycles":    (IVH_CS_HALT_NR,),
    "ivh_head_halt_events":    (IVH_CS_HALT_NR,),
    "ivh_head_halt_hist":      (IVH_CS_HALT_NR, IVH_BEAT_AGE_HIST_BUCKETS),
}

# Row labels for the is_cs_preempted() arrays. The PV_BAIL_CAUSE_NAMES printer
# below assumes a 6-row first dimension and would index past the end of these.
# None == a bare log2 histogram with no row dimension.
CS_ARRAY_LABELS = {
    "ivh_cs_ep_events_by_end": IVH_CS_EP_NAMES,
    "ivh_cs_ep_cycles":        IVH_CS_EP_NAMES,
    "ivh_cs_ep_hist":          IVH_CS_EP_NAMES,
    "ivh_cs_tenure_cycles":    IVH_CS_TENURE_NAMES,
    "ivh_cs_tenure_hist":      IVH_CS_TENURE_NAMES,
    "ivh_cs_prev_hold_hist":   None,
    "ivh_cs_prompt_hist":      None,
    "ivh_head_halt_cycles":    IVH_CS_HALT_NAMES,
    "ivh_head_halt_events":    IVH_CS_HALT_NAMES,
    "ivh_head_halt_hist":      IVH_CS_HALT_NAMES,
}

def load_kallsyms():
    sym = {}
    with open(KALLSYMS) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr, typ, name = parts[0], parts[1], parts[2]
            sym[name] = int(addr, 16)
    return sym

def read_phdrs(f):
    f.seek(0)
    ident = f.read(64)
    e_phoff, = struct.unpack_from("<Q", ident, 32)
    e_phentsize, = struct.unpack_from("<H", ident, 54)
    e_phnum, = struct.unpack_from("<H", ident, 56)
    phdrs = []
    f.seek(e_phoff)
    for i in range(e_phnum):
        ph = f.read(e_phentsize)
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<IIQQQQQQ", ph)
        if p_type == 1:
            phdrs.append((p_vaddr, p_offset, p_filesz))
    return phdrs

def va_to_offset(phdrs, va):
    for p_vaddr, p_offset, p_filesz in phdrs:
        if p_vaddr <= va < p_vaddr + p_filesz:
            return p_offset + (va - p_vaddr)
    raise ValueError(f"va {hex(va)} not mapped")

MASK64 = (1 << 64) - 1

def read_u64(f, phdrs, va):
    va &= MASK64
    off = va_to_offset(phdrs, va)
    f.seek(off)
    return struct.unpack("<Q", f.read(8))[0]

def online_cpus():
    with open("/sys/devices/system/cpu/online") as f:
        spec = f.read().strip()
    cpus = []
    for part in spec.split(","):
        if "-" in part:
            lo, hi = part.split("-")
            cpus.extend(range(int(lo), int(hi) + 1))
        else:
            cpus.append(int(part))
    return cpus

def flat_size(shape):
    n = 1
    for d in shape:
        n *= d
    return n

def read_array_counter(f, phdrs, base, offsets, shape):
    """Sum each flattened element across all CPUs; return a flat list."""
    n = flat_size(shape)
    totals = [0] * n
    for off in offsets:
        for i in range(n):
            totals[i] += read_u64(f, phdrs, base + 8 * i + off)
    return totals

def print_rot_depth_hist(name, totals):
    """Depth of the first live waiter at handoff. Index 0 means the immediate
    successor was live (the common, uninteresting case); index IVH_ROT_HOP_CAP
    is the 'no live waiter found within the cap' bucket."""
    found = sum(totals[:IVH_ROT_HOP_CAP])
    none = totals[IVH_ROT_HOP_CAP]
    for i in range(IVH_ROT_HOP_CAP):
        label = "successor live" if i == 0 else f"depth {i}"
        print(f"{name:28s}[{label:18s}] = {totals[i]}")
    print(f"{name:28s}[{'none within cap':18s}] = {none}")
    print(f"{name:28s}[{'TOTAL':18s}] = {found + none}")


TSC_MHZ = None


def tsc_mhz():
    """Cycles->us needs the guest TSC rate; read it once from dmesg."""
    global TSC_MHZ
    if TSC_MHZ is None:
        TSC_MHZ = 0.0
        try:
            import re as _re, subprocess
            m = _re.search(r"tsc: Detected ([0-9.]+) MHz",
                           subprocess.run(["dmesg"], capture_output=True,
                                          text=True).stdout)
            if m:
                TSC_MHZ = float(m.group(1))
        except Exception:
            pass
    return TSC_MHZ


def print_rot_idle(name, totals, shape):
    """Phase 0b. Idle time = lock released -> queue head claims it, split by
    whether that head was flagged stale at ITS promotion. Only the EXCESS of a
    stale class over the 'live (baseline)' class is time rotation could
    recover; the absolute value is dominated by ordinary handoff latency."""
    nb = shape[1] if len(shape) > 1 else 0
    mhz = tsc_mhz()
    for i, label in enumerate(IVH_ROT_CLASS_NAMES):
        if len(shape) == 1:
            print(f"{name:28s}[{label:22s}] = {totals[i]}")
        else:
            row = totals[i * nb:(i + 1) * nb]
            # Show the bucket's lower edge in us -- a log2 histogram read as
            # raw exponents is unreadable, and the median must come from here
            # rather than from cycles/events, which a single capped artifact
            # can dominate.
            nz = [(f"{(2 ** b) / mhz:.1f}us" if mhz else b, v)
                  for b, v in enumerate(row) if v]
            print(f"{name:28s}[{label:22s}] sum={sum(row)}  {nz}")


def print_cs_array(name, totals, shape, labels):
    """is_cs_preempted() arrays: labelled rows, or a bare log2 histogram."""
    if labels is None:
        nz = [(b, v) for b, v in enumerate(totals) if v]
        print(f"{name:28s} sum={sum(totals)}  nonzero_buckets={nz}")
        return
    if len(shape) == 1:
        for i, label in enumerate(labels):
            print(f"{name:28s}[{label:16s}] = {totals[i]}")
        print(f"{name:28s}[{'TOTAL':16s}] = {sum(totals)}")
    else:
        nb = shape[1]
        for i, label in enumerate(labels):
            row = totals[i * nb:(i + 1) * nb]
            nz = [(b, v) for b, v in enumerate(row) if v]
            print(f"{name:28s}[{label:16s}] sum={sum(row)}  nonzero_buckets={nz}")


TSC_HZ = 2200.0e6   # dmesg: "tsc: Detected 2200.000 MHz processor"


def print_age_hist(name, totals):
    """log2 TSC-cycle histogram, labelled by each bucket's lower edge in us.

    These arrays have IVH_BEAT_AGE_HIST_BUCKETS (32) entries. Without an entry
    in CS_ARRAY_LABELS they fall through print_bail_cause_array() to the
    PV_BAIL_CAUSE_NAMES branch, which prints SIX labels and silently discards
    buckets 6..31 -- everything above 64 cycles, i.e. the entire informative
    range -- while still printing a TOTAL summed over all 32, so the total
    looks right and the breakdown is wrong.
    """
    tot = sum(totals)
    print(f"{name}  n={tot}")
    if not tot:
        return
    cum = 0
    for b, v in enumerate(totals):
        if not v:
            continue
        cum += v
        lo_us = (2.0 ** b) / TSC_HZ * 1e6
        print(f"    b{b:<2d} >= {lo_us:10.2f} us  {v:12d}  {100.0*v/tot:6.2f}%  "
              f"cum {100.0*cum/tot:6.2f}%")


def print_bail_cause_array(name, totals, shape):
    if name in ("ivh_evict_gap_hist", "ivh_evict_age_used_hist",
                "ivh_evict_age_true_hist", "ivh_evict_cpubeat_hist",
                "ivh_act_hist", "ivh_g2_act_hist"):
        print_age_hist(name, totals)
        return
    if name in CS_ARRAY_LABELS:
        print_cs_array(name, totals, shape, CS_ARRAY_LABELS[name])
        return
    if name == "ivh_rot_depth_hist":
        print_rot_depth_hist(name, totals)
        return
    if name.startswith("ivh_rot_idle_"):
        print_rot_idle(name, totals, shape)
        return
    if len(shape) == 1:
        for i, label in enumerate(PV_BAIL_CAUSE_NAMES):
            print(f"{name:28s}[{label:16s}] = {totals[i]}")
        print(f"{name:28s}[{'TOTAL':16s}] = {sum(totals)}")
    else:
        buckets = shape[1]
        for i, label in enumerate(PV_BAIL_CAUSE_NAMES):
            row = totals[i * buckets:(i + 1) * buckets]
            nz = [(b, v) for b, v in enumerate(row) if v]
            print(f"{name:28s}[{label:16s}] sum={sum(row)}  nonzero_buckets={nz}")

def print_phase0b_summary(got):
    """The validity gate. An idle-time histogram is only quotable if most
    acquisitions could actually be attributed to a release."""
    ev = got.get("ivh_rot_idle_events")
    if not ev:
        return
    unknown = got.get("ivh_rot_idle_unknown", 0)
    backward = got.get("ivh_rot_idle_backward", 0)
    capped = got.get("ivh_rot_idle_capped", 0)
    attributed = sum(ev)
    acks = attributed + unknown + backward + capped
    if not acks:
        return
    mhz = tsc_mhz() or 1.0
    print()
    print("=== Phase 0b summary ===")
    print(f"  acks={acks}  attributed={attributed} ({100.0*attributed/acks:.1f}%)"
          f"  unknown={unknown}  backward={backward}  capped={capped}")
    if attributed < 0.8 * acks:
        print("  *** attribution < 80%: histograms are NOT representative,"
              " do not quote them ***")
    cyc = got.get("ivh_rot_idle_cycles") or []
    for i, label in enumerate(IVH_ROT_CLASS_NAMES):
        if i < len(ev) and ev[i]:
            print(f"  {label:22s} n={ev[i]:<10d} total={cyc[i]/mhz/1e6:.4f}s"
                  f"  mean={cyc[i]/ev[i]/mhz:.1f}us")
    print("  NOTE: ivh_rot_tail_stop is a SUBSET of ivh_rot_no_live"
          " (tail_stop falls through to none_found) -- never add them.")
    print("  NOTE: recoverable time is the ABSOLUTE total of"
          " 'stale + skippable'; class 0 is not a baseline to subtract"
          " (every sampled head had already halted).")


def print_g50_summary(got):
    """G-LOCK-50 / G1 verdict: is Gate 2's input where a threshold can reach it?

    Prints the burst distribution's mean and median side by side. The whole
    point of the histogram is that these two disagree by ~7x on contended
    vCPUs, and an EWMA converges to the MEAN -- so the mean is what decides
    whether ivh_time_left_threshold_ns becomes tunable. Reading the median
    (or a /proc/kcore snapshot, which is a biased draw from the same series)
    predicts the opposite answer.
    """
    hist = got.get("ivh_act_hist")
    if not hist:
        return
    n = sum(hist)
    if not n:
        print()
        print("=== G-LOCK-50 summary ===")
        print("  ivh_act_hist is EMPTY -- ivh_vact_tick() detected no host"
              " preemptions. Check ivh_vact_jump_ns and that the run was"
              " actually under load; do not interpret the gate counters.")
        return

    # log2 buckets: use each bucket's geometric midpoint (1.5 * 2^b) as the
    # representative value, which is the standard unbiased choice for a
    # log2 histogram and is within 6% of the true mean for a smooth density.
    mean_cyc = sum(1.5 * (2.0 ** b) * v for b, v in enumerate(hist)) / n
    half, cum, med_cyc = n / 2.0, 0, 0.0
    for b, v in enumerate(hist):
        cum += v
        if cum >= half:
            med_cyc = 1.5 * (2.0 ** b)
            break

    print()
    print("=== G-LOCK-50 summary ===")
    print(f"  bursts observed      n = {n}")
    print(f"  MEAN burst             = {mean_cyc / TSC_HZ * 1e6:10.1f} us"
          f"   <-- an EWMA converges here")
    print(f"  MEDIAN burst           = {med_cyc / TSC_HZ * 1e6:10.1f} us"
          f"   <-- what last_active reports")
    if med_cyc:
        print(f"  mean/median            = {mean_cyc / med_cyc:10.1f}x"
              f"   (>2x means heavy-tailed: the median is NOT the EWMA)")
    print(f"  zero bucket (b0)       = {hist[0]} ({100.0 * hist[0] / n:.2f}%)"
          f"   Gate 2 short-circuits on these")

    cons = got.get("ivh_g2_act_hist")
    if cons and sum(cons):
        cn = sum(cons)
        cmean = sum(1.5 * (2.0 ** b) * v for b, v in enumerate(cons)) / cn
        chalf, ccum, cmed = cn / 2.0, 0, 0.0
        for b, v in enumerate(cons):
            ccum += v
            if ccum >= chalf:
                cmed = 1.5 * (2.0 ** b)
                break
        print()
        print("  --- CONSUMED distribution (what Gate 2 actually reads) ---")
        print(f"  consultations        n = {cn}")
        print(f"  MEAN   (consumed)      = {cmean / TSC_HZ * 1e6:10.1f} us")
        print(f"  MEDIAN (consumed)      = {cmed / TSC_HZ * 1e6:10.1f} us")
        print(f"  length-bias factor     = {cmean / mean_cyc:10.1f}x"
              f"   (consumed mean / produced mean)")
        lo_c = 500e-6 * TSC_HZ
        hi_c = 10e-3 * TSC_HZ
        band = sum(v for b, v in enumerate(cons)
                   if lo_c <= 1.5 * (2.0 ** b) <= hi_c)
        print(f"  mass in 500us..10ms    = {band} ({100.0 * band / cn:.2f}%)"
              f"   <-- the entire swept range")
        if band < 0.10 * cn:
            print("  *** BIMODAL CONFIRMED: <10% of consultations land inside"
                  " the swept band, so ivh_time_left_threshold_ns cannot move"
                  " the verdict regardless of its value. The fix is to make"
                  " the input continuous (EWMA), not to retune the knob. ***")

    ev = got.get("ivh_g2_eval")
    if ev:
        zero = got.get("ivh_g2_zero_input", 0)
        fired = got.get("ivh_steal_imminent_time_left_reject", 0)
        g1 = got.get("ivh_steal_imminent_capacity_reject", 0)
        print(f"  Gate 1 rejects         = {g1}")
        print(f"  Gate 2 consulted       = {ev}")
        print(f"  Gate 2 zero input      = {zero} ({100.0 * zero / ev:.2f}% of consults)")
        print(f"  Gate 2 FIRED           = {fired} ({100.0 * fired / ev:.2f}% of consults)")
        print("  NOTE: Gate 2 firing near 0% with a mean burst BELOW"
              " ivh_time_left_threshold_ns is the predicted signature of the"
              " knob being out of range, not of it being correctly tuned.")


def main():
    names = sys.argv[1:] if len(sys.argv) > 1 else DEFAULT_COUNTERS
    got = {}
    sym = load_kallsyms()
    cpus = online_cpus()
    with open(KCORE, "rb") as f:
        phdrs = read_phdrs(f)
        per_cpu_offset_base = sym["__per_cpu_offset"]
        offsets = [read_u64(f, phdrs, per_cpu_offset_base + 8 * c) for c in cpus]
        for name in names:
            if name not in sym:
                print(f"{name:32s} : NOT FOUND in kallsyms")
                continue
            base = sym[name]
            if name in ARRAY_COUNTERS:
                shape = ARRAY_COUNTERS[name]
                totals = read_array_counter(f, phdrs, base, offsets, shape)
                got[name] = totals
                print_bail_cause_array(name, totals, shape)
                continue
            total = 0
            for off in offsets:
                total += read_u64(f, phdrs, base + off)
            got[name] = total
            print(f"{name:32s} = {total}")
    print_phase0b_summary(got)
    print_g50_summary(got)

if __name__ == "__main__":
    main()
