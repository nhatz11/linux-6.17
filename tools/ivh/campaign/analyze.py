#!/usr/bin/env python3
"""Campaign analysis. Usage: analyze.py results.csv benchmarks.tsv

Per block, the effect is IVH vs PV using the two samples of each arm inside that
block (ABBA / BAAB), so drift and position cancel. Positive always means IVH is
better, whichever direction the metric runs.

Screen rule: all blocks agree in sign AND |median| >= 5% -> CANDIDATE.
"""
import csv, sys, statistics as st

res, meta = sys.argv[1], sys.argv[2]
hl = {}
for line in open(meta):
    if not line.strip() or line.startswith('#'):
        continue
    f = line.rstrip('\n').split('\t')
    if len(f) >= 6:
        hl[f[0]] = f[5].strip()

data = {}
for r in csv.DictReader(open(res)):
    if r['value'] == 'FAIL':
        continue
    try:
        v = float(r['value'])
    except ValueError:
        continue
    data.setdefault(r['workload'], {}).setdefault(int(r['block']), {}).setdefault(r['mode'], []).append(v)

def block_effect(name, arms):
    if 'pv' not in arms or 'ivh' not in arms:
        return None
    pv, ivh = st.mean(arms['pv']), st.mean(arms['ivh'])
    if pv == 0:
        return None
    pct = (ivh - pv) / pv * 100.0
    return -pct if hl.get(name, 'hi') == 'lo' else pct

rows = []
for name, blocks in sorted(data.items()):
    eff = [(b, block_effect(name, a)) for b, a in sorted(blocks.items())]
    eff = [(b, e) for b, e in eff if e is not None]
    if not eff:
        continue
    vals = [e for _, e in eff]
    med = st.median(vals)
    same_sign = all(v > 0 for v in vals) or all(v < 0 for v in vals)
    wins = sum(1 for v in vals if v > 0)
    rows.append((med, name, vals, same_sign, wins))

rows.sort(reverse=True)
print(f"{'verdict':10s} {'workload':24s} {'median':>8s} {'n':>3s} {'wins':>5s}  per-block %")
for med, name, vals, same_sign, wins in rows:
    if same_sign and abs(med) >= 5:
        verdict = "CANDIDATE" if med > 0 else "REGRESSION"
    elif abs(med) >= 5:
        verdict = "NOISY"
    else:
        verdict = "neutral"
    print(f"{verdict:10s} {name:24s} {med:+7.1f}% {len(vals):3d} {wins:3d}/{len(vals)}  " +
          " ".join(f"{v:+.1f}" for v in vals))

print()
for med, name, vals, same_sign, wins in rows:
    if same_sign and med >= 5:
        print("CANDIDATE", name)
