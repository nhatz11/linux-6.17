#!/usr/bin/env python3
"""Progress / final report for vips_resolve.sh.

Ratio-of-means with a paired bootstrap CI. vips is a TIME workload on fixed work,
so no ops normalisation is applied and seconds are directly comparable.
"""
import sys
import statistics as st
import random

random.seed(2024)
NS = 26e-9
BOOT = 4000

rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
by = {}
for x in rows:
    if x[2] in ("NA", ""):
        continue
    by.setdefault(x[1], {})[x[0]] = x

ps, as_, pt, at = [], [], [], []
for rep, g in sorted(by.items(), key=lambda z: int(z[0])):
    if "pv" not in g or "as" not in g:
        continue
    p, a = g["pv"], g["as"]
    t0 = int(p[3]) + int(p[4])
    t1 = int(a[3]) + int(a[4])
    if t0 <= 0:
        continue
    ps.append(t0); as_.append(t1)
    pt.append(float(p[2])); at.append(float(a[2]))

n = len(ps)
if n < 3:
    print(f"  n={n}, too few")
    sys.exit()


def rom(x, y):
    mx = st.mean(x)
    return 100 * (mx - st.mean(y)) / mx if mx else 0.0


def boot(x, y):
    out = []
    for _ in range(BOOT):
        idx = [random.randrange(n) for _ in range(n)]
        mx = st.mean([x[i] for i in idx])
        if mx <= 0:
            continue
        out.append(100 * (mx - st.mean([y[i] for i in idx])) / mx)
    out.sort()
    return out[int(0.025 * len(out))], out[int(0.975 * len(out))]


s = rom(ps, as_); slo, shi = boot(ps, as_)
p = rom(pt, at); plo, phi = boot(pt, at)     # seconds: lower is better, same sign convention

sv = "POSITIVE" if slo > 0 else ("NEGATIVE" if shi < 0 else "spans 0")
pv_ = "POSITIVE" if plo > 0 else ("NEGATIVE" if phi < 0 else "spans 0")

print(f"  n={n:3d}  SPIN {s:+7.2f}%  CI [{slo:+7.2f},{shi:+7.2f}]  {sv:8s}   "
      f"PERF {p:+6.2f}%  CI [{plo:+6.2f},{phi:+6.2f}]  {pv_}")
print(f"        PV spin mean {st.mean(ps) * NS * 1000:7.1f} ms "
      f"(range {min(ps) * NS * 1000:.1f}-{max(ps) * NS * 1000:.1f}, {max(ps)/max(min(ps),1):.0f}x spread)"
      f"   AS {st.mean(as_) * NS * 1000:7.1f} ms")
if slo > 0 and phi >= 0:
    print("        => vips PASSES: spin CI entirely above 0, perf CI not entirely below 0")
