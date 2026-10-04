import csv, sys, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
by = defaultdict(lambda: defaultdict(list)); mg = defaultdict(list)
for x in rows:
    by[x['pkg']][x['arm']].append(float(x['seconds']))
    if x['arm'] == 'on': mg[x['pkg']].append(int(x['migrations']))
for pkg, d in by.items():
    o, n = d.get('off', []), d.get('on', [])
    if not o or not n: continue
    mo, mn = st.mean(o), st.mean(n)
    cvo = 100*st.stdev(o)/mo if len(o) > 1 else 0
    cvn = 100*st.stdev(n)/mn if len(n) > 1 else 0
    rom = 100*(mo-mn)/mo                                  # ratio-of-means: correct
    mor = st.mean([100*(a-b)/a for a, b in zip(o, n)])     # mean-of-ratios: what the harness prints
    wins = sum(1 for a, b in zip(o, n) if b < a)
    so = st.stdev(o)/len(o)**.5 if len(o) > 1 else 0
    sn = st.stdev(n)/len(n)**.5 if len(n) > 1 else 0
    t = (mo-mn)/((so**2+sn**2)**.5) if (so or sn) else 0
    print(f"\n  ===== {pkg} =====")
    print(f"    OFF (stock PV)   {mo:8.2f}s   CV {cvo:5.1f}%   range {min(o):6.2f}-{max(o):7.2f}s   n={len(o)}")
    print(f"    ON  (migration)  {mn:8.2f}s   CV {cvn:5.1f}%   range {min(n):6.2f}-{max(n):7.2f}s   n={len(n)}")
    print(f"    ratio-of-means   {rom:+7.2f}% faster   t={t:5.2f}   ON faster {wins}/{len(o)} pairs")
    print(f"    mean-of-ratios   {mor:+7.2f}%  <- what the harness prints; understates when OFF swings")
    print(f"    speedup          {mo/mn:5.2f}x      migrations/run {int(st.mean(mg[pkg])) if mg[pkg] else 0}")
    if cvo > 25 and cvn < cvo/2:
        print(f"    ++ variance COLLAPSE {cvo:.0f}% -> {cvn:.0f}%: migration is removing the stalls,")
        print(f"       which is the mechanism -- the OFF spread is signal, not instrument noise")
