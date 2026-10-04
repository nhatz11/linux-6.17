#!/usr/bin/env python3
import sys, statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
by={}
for x in r: by.setdefault(x[3],{})[x[0]]=x
HI=15e9   # halt_ns above this = HIGH-halt regime (low regime is 4-7s, high 20-50s)
arms=[a for a in dict.fromkeys(x[0] for x in r) if a!='mig']
print("Paired vs mig+t1, SPLIT by regime match (halt>15s = HIGH).")
print("A pair whose arms are in different regimes is a different operating point, not a result.\n")
for a in arms:
    same=[];diff=0;t2=[]
    for rep,d in sorted(by.items(), key=lambda z:int(z[0])):
        if 'mig' not in d or a not in d: continue
        M,A=d['mig'],d[a]
        rm = int(M[6])>HI; ra = int(A[6])>HI
        sd=100*(int(M[5])-int(A[5]))/int(M[5]); td=100*(float(M[4])-float(A[4]))/float(M[4])
        t2.append(100*int(A[8])/max(int(A[9]),1))
        if rm==ra: same.append((sd,td))
        else: diff+=1
    if not same: print(f"  {a:>9s}  no regime-matched pairs ({diff} mismatched)"); continue
    sd=[z[0] for z in same]; td=[z[1] for z in same]
    print(f"  {a:>9s}  n={len(sd)} matched ({diff} dropped)  SPIN median {st.median(sd):+6.2f}% ({sum(1 for z in sd if z>0)}/{len(sd)})"
          f"   PERF median {st.median(td):+6.2f}% ({sum(1 for z in td if z>0)}/{len(td)})   t2 fire {st.mean(t2):.1f}%")
    print(f"             spin {[round(z,1) for z in sd]}   perf {[round(z,1) for z in td]}")
