#!/usr/bin/env python3
"""One row of the spin-threshold sweep.

ivh_cs_react[had_tail][state][bucket], flattened to 6 rows of 32
(row = had_tail*3 + state). Row 3 = queue present, head spinning;
row 4 = queue present, head halted on its own. Row 5 (we set _Q_SLOW_VAL
ourselves via pv_kick_node) is excluded.

CLIFF = lowest bucket whose halted% > 20% with at least 5 samples: the point
where the head has had time to react at all.
"""
import re, sys
MHZ = 2200.0

def rows(s):
    return [{int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', p)}
            for p in s.split('|') if p.strip()]

thr, arm, a_raw, b_raw = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
a, b = rows(a_raw), rows(b_raw)
while len(a) < 6: a.append({})
while len(b) < 6: b.append({})
d = [{k: b[i].get(k, 0) - a[i].get(k, 0) for k in set(a[i]) | set(b[i])} for i in range(6)]
g = lambda i, k: max(d[i].get(k, 0), 0)

cliff = "-"
for k in range(10, 32):
    n = g(3, k) + g(4, k)
    if n >= 5 and 100.0 * g(4, k) / n > 20.0:
        cliff = f"{(2.0**k)/MHZ:.0f}us"
        break
spin = sum(g(3, k) for k in range(20, 32))
hal  = sum(g(4, k) for k in range(20, 32))
n = spin + hal
pct = (100.0 * hal / n) if n else float('nan')
print(f"{thr:>8} {arm:>6} {n:10d} {hal:8d} {pct:8.1f}% {cliff:>8}")
