#!/usr/bin/env python3
"""
Replay ivh_tick_steal_accumulate() over a vcpu_trace recording.

The guest tick (nohz=off, CONFIG_HZ=1000) is a periodic hrtimer on an ABSOLUTE
grid: hrtimer_forward advances by whole periods from the previous expiry and
skips deadlines that have already passed, so a gap spanning k deadlines yields
ONE delivery on resume, not k queued ones. Given the gap list that makes tick
delivery times fully determined, so the kernel's arithmetic can be reproduced
exactly rather than measured.

Kernel arithmetic reproduced verbatim from kernel/sched/core.c:257 with
ivh_tks_idle_sub=0 (so d_idle_c is forced to 0).
"""
import struct, sys

def load(path):
    with open(path,'rb') as f:
        hdr = struct.unpack('<7Q', f.read(56))
        magic, cpu, khz, t0, t1, n, minc = hdr
        assert magic == 0x56435047414F4E31, "bad magic"
        gs = struct.unpack(f'<{n}Q', f.read(8*n)) if n else ()
        gl = struct.unpack(f'<{n}Q', f.read(8*n)) if n else ()
    return dict(cpu=cpu, khz=khz, t0=t0, t1=t1, gaps=list(zip(gs,gl)), minc=minc)

def ns2c(ns, khz):  return (ns * khz) // 1_000_000
def c2ns(c,  khz):  return (c * 1_000_000) // khz

def tick_times(tr, phase=0, period_ns=1_000_000):
    """Delivery TSC of each tick, given the absolute-grid + skip semantics."""
    khz, t0, t1 = tr['khz'], tr['t0'], tr['t1']
    T = ns2c(period_ns, khz)                 # sampler period (TICK_NSEC at HZ=1000)
    gaps = tr['gaps']
    out, gi, k = [], 0, 1
    origin = t0 + phase
    while True:
        D = origin + k*T
        if D > t1: break
        while gi < len(gaps) and gaps[gi][0] + gaps[gi][1] <= D: gi += 1
        if gi < len(gaps) and gaps[gi][0] <= D < gaps[gi][0] + gaps[gi][1]:
            fire = gaps[gi][0] + gaps[gi][1]          # delivered on resume
        else:
            fire = D
        if not out or fire > out[-1]: out.append(fire)  # collapse skipped ticks
        k += 1
    return out, T

def replay(tr, phase_pct, deadband_ns, carry_ticks=8, phase=0, period_ns=1_000_000):
    khz = tr['khz']
    ticks, T = tick_times(tr, phase, period_ns)
    db_c = ns2c(deadband_ns, khz)
    floor_c = -(T * max(1, min(carry_ticks, 10000)))
    carry, steal_ns = 0, 0
    for i in range(1, len(ticks)):
        avail_c = ticks[i] - ticks[i-1]
        excess  = avail_c - T
        if excess > db_c and phase_pct:
            excess += (T * phase_pct) // 100
        carry += excess
        if carry > 0:
            steal_ns += c2ns(carry, khz); carry = 0
        elif carry < floor_c:
            carry = floor_c
    return steal_ns, len(ticks)

def truth_ns(tr, min_ns=0):
    khz = tr['khz']; lo = ns2c(min_ns, khz)
    return sum(c2ns(l, khz) for _, l in tr['gaps'] if l >= lo)

if __name__ == '__main__':
    tr = load(sys.argv[1])
    span_ns = c2ns(tr['t1']-tr['t0'], tr['khz'])
    print(f"cpu={tr['cpu']} span={span_ns/1e9:.3f}s gaps={len(tr['gaps'])} "
          f"tsc_khz={tr['khz']}")
    for th in (0, 1000, 5000, 50000):
        print(f"  truth(gaps>={th}ns) = {truth_ns(tr,th)/1e6:10.3f} ms "
              f"({truth_ns(tr,th)/span_ns*100:5.2f}% of span)")
