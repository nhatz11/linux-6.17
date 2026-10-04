#!/usr/bin/env python3
"""Measure delta: Gate 2's runway prediction error, on a real workload.

Gate 2 predicts remaining runway as last_active - elapsed_since_active, where
last_active is the length of the PREVIOUS active burst. So burst[i] predicts
burst[i+1]. delta is the margin needed to absorb that error.

Reads /proc/ivh_cpu_stats field 9 (last_active_c) rather than struct rq via
/proc/kcore. Two bugs killed the kcore path (2026-10-02): a held-open fd
returns FROZEN values (1 distinct in 30 samples), and the hardcoded struct
offset 3976 went stale when G-LOCK-50/51/52 added rq fields -- it was reading
a TSC timestamp, not a duration, which showed up as correlation 1.000 and 0%
error. The proc file regenerates per open, so neither failure can recur.

usage: delta_measure.py <seconds> [threshold_us ...]
"""
import sys, time, statistics as st

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
THRESH = [float(x) for x in sys.argv[2:]] or [100, 250, 500, 750, 1000, 2000, 4000]
P = "/proc/ivh_cpu_stats"

def khz():
    with open(P) as f:
        return float(f.readline().split("tsc_khz=")[1].split()[0])
KHZ = khz(); CYC_US = KHZ / 1000.0

def snap():
    out = {}
    with open(P) as f:
        for i, ln in enumerate(f):
            if i < 2 or ln.startswith("#"): continue
            p = ln.split()
            if len(p) > 10:
                out[p[0]] = (int(p[8]), int(p[10]))   # last_active_c, uc_cap
    return out

seq = {}; last = {}; cap = {}
t0 = time.time()
while time.time() - t0 < SECS:
    for c, (la, uc) in snap().items():
        if la > 0 and last.get(c) != la:
            seq.setdefault(c, []).append(la / CYC_US)
            last[c] = la
        cap[c] = uc
    time.sleep(0.002)

# Gate 2 only ever runs on vCPUs Gate 1 admitted: capacity <= threshold.
with open("/proc/sys/kernel/ivh_capacity_threshold") as f: CT = int(f.read())
elig = [c for c in seq if cap.get(c, 1024) <= CT and len(seq[c]) > 20]
if not elig:
    sys.exit(f"no gate-eligible vCPU with enough transitions (cap<={CT}); need host contention")
pairs = []
for c in elig: pairs += list(zip(seq[c][:-1], seq[c][1:]))
if len(pairs) < 50: sys.exit(f"only {len(pairs)} pairs; run longer")

print(f"{len(pairs)} burst pairs on {len(elig)} GATE-ELIGIBLE vCPUs (cap<={CT}): {sorted(elig)}")
pred = [p for p, _ in pairs]; act = [a for _, a in pairs]
print(f"\nburst length: median pred {st.median(pred):.1f} us   median actual {st.median(act):.1f} us")
err = sorted(abs(p - a) for p, a in pairs)
rel = sorted(abs(p - a) / a for p, a in pairs if a > 0)
def pct(v, q): return v[min(int(q * len(v)), len(v) - 1)]
print("\n=== delta: ABSOLUTE prediction error (the margin Gate 2 must absorb) ===")
for q in (0.50, 0.75, 0.90, 0.95, 0.99):
    print(f"   p{int(q*100):<3} |pred-actual| = {pct(err,q):9.1f} us")
print(f"   mean           = {st.mean(err):9.1f} us")
print("\n=== relative error ===")
for q in (0.50, 0.90, 0.95):
    print(f"   p{int(q*100):<3} = {100*pct(rel,q):6.1f}%")
print("\n=== DECISION quality: does 'pred > T' predict 'actual > T'? ===")
print(f"   {'T us':>7}{'acc':>8}{'prec':>8}{'recall':>8}{'baseline':>10}  verdict")
for T in THRESH:
    tp = sum(1 for p,a in pairs if p>T and a>T); fp = sum(1 for p,a in pairs if p>T and a<=T)
    fn = sum(1 for p,a in pairs if p<=T and a>T); tn = sum(1 for p,a in pairs if p<=T and a<=T)
    n = len(pairs); acc = 100*(tp+tn)/n
    base = 100*max(tp+fn, tn+fp)/n
    prec = 100*tp/(tp+fp) if tp+fp else float('nan')
    rec  = 100*tp/(tp+fn) if tp+fn else float('nan')
    v = "no better than guessing" if acc <= base+1 else f"beats {base:.0f}% baseline"
    print(f"   {T:7.0f}{acc:7.1f}%{prec:7.1f}%{rec:7.1f}%{base:9.1f}%  {v}")
