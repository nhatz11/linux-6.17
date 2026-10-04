#!/usr/bin/env python3
"""
Read per-CPU struct rq fields (runqueues[cpu].<field>) live from /proc/kcore.
Distinct from read_ivh_counters.py: those are standalone DEFINE_PER_CPU
scalars; these are fields INSIDE the per-CPU struct rq, so the address is
runqueues_base + __per_cpu_offset[cpu] + field_byte_offset (offset obtained
via `pahole -C rq vmlinux` once, hardcoded below -- re-run pahole and update
if struct rq layout ever changes).
"""
import struct, sys

KCORE = "/proc/kcore"
KALLSYMS = "/proc/kallsyms"

# field_name -> (byte_offset_in_struct_rq, size_in_bytes, signed)
FIELDS = {
    "prmpt_flags":                (3664, 4, False),   # atomic_t; BIT(2)=PRMPT_HELD_MASK
    "clock_preempt":             (3672, 8, False),
    "last_idle_tp":               (3680, 8, False),
    "last_preemption":            (3688, 8, False),
    "last_active_time":           (3704, 8, False),
    "ivh_vact_stamp":             (3944, 8, False),
    "ivh_vact_idle_exit_tsc":     (3952, 8, False),
    "ivh_vact_burst_start_tsc":   (3960, 8, False),
    "ivh_vact_last_preempt_tsc":  (0xf80, 8, False),
    "ivh_vact_last_preempt_start_tsc": (0xf88, 8, False),   # G-LOCK-41
    "ivh_vact_last_active_c":     (0xf90, 8, False),   # G-LOCK-41: +8
    "ivh_vact_jumps":             (0xf98, 8, False),   # G-LOCK-41: +8
    "ivh_vact_idle_explained":    (0xfa0, 8, False),   # G-LOCK-41: +8
    "ivh_uc_capacity":            (3824, 8, False),
    "ivh_tks_steal_ns":           (3912, 8, False),
    "ivh_uc_win_avail_c_": (3776, 8, False),
    "ivh_uc_win_stolen_c_": (3784, 8, False),
    "ivh_uc_win_used_c": (3792, 8, False),
    "ivh_uc_win_acct_c": (3800, 8, False),
    "ivh_uc_capacity_wall": (3832, 8, False),
    "ivh_uc_capacity_acct": (3840, 8, False),
    "ivh_uc_windows": (3848, 8, False),
    "ivh_uc_extended": (3864, 8, False),
    "ivh_uc_raw_wall": (3872, 8, False),
    "ivh_tks_carry_c":    (3904, 8, True),
    "ivh_tks_samples":    (3920, 8, False),
    "ivh_tks_skipped":    (3936, 8, False),
    "ivh_uc_win_avail_c":         (3776, 8, False),
    "ivh_uc_win_stolen_c":        (3784, 8, False),
    "ivh_uc_capacity_wall":       (3832, 8, False),
}

MASK64 = (1 << 64) - 1

def load_kallsyms():
    sym = {}
    with open(KALLSYMS) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            sym[parts[2]] = int(parts[0], 16)
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

def main():
    names = sys.argv[1:] if len(sys.argv) > 1 else list(FIELDS.keys())
    sym = load_kallsyms()
    cpus = online_cpus()
    rq_base = sym["runqueues"]
    per_cpu_offset_base = sym["__per_cpu_offset"]

    with open(KCORE, "rb") as f:
        phdrs = read_phdrs(f)
        offsets = [read_u64(f, phdrs, per_cpu_offset_base + 8 * c) for c in cpus]

        for name in names:
            if name not in FIELDS:
                print(f"{name:28s} : NOT IN FIELD TABLE")
                continue
            byte_off, size, signed = FIELDS[name]
            vals = []
            for off in offsets:
                v = read_u64(f, phdrs, rq_base + byte_off + off)
                vals.append(v)
            total = sum(vals)
            print(f"{name:28s} sum={total:20d}  per-cpu={vals}")

if __name__ == "__main__":
    main()
