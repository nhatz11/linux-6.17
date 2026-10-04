#!/usr/bin/env python3
"""Pick the best threshold from Phase B: lowest MEAN spin ratio across the
workloads that actually engaged the mechanism (tier2 fires >= 50). Workloads
where AS is inert carry no information about the threshold and would dilute the
choice. Prints just the number, for the shell to consume."""
import sys, statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
arms=[a for a in dict.fromkeys(x[2] for x in rows) if a!='pv']
score={}
for a in arms:
    rs=[]
    for w in dict.fromkeys(x[0] for x in rows):
        pv=[x for x in rows if x[0]==w and x[2]=='pv']
        g =[x for x in rows if x[0]==w and x[2]==a]
        if not(pv and g): continue
        if st.mean(int(x[7]) for x in g) < 50: continue      # inert: no signal
        i0=st.mean(int(x[5]) for x in pv); i1=st.mean(int(x[5]) for x in g)
        if i0>0: rs.append(i1/i0)
    if rs: score[a]=st.mean(rs)
if not score:
    print("100"); sys.exit(0)
best=min(score, key=score.get)
sys.stderr.write("  threshold scores (mean spin ratio, engaged workloads only):\n")
for a in sorted(score, key=score.get):
    sys.stderr.write(f"    {a}us -> {score[a]:.4f}{'   <-- BEST' if a==best else ''}\n")
print(best)
