#!/usr/bin/env python3
import sys, statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
def corr(a,b):
    n=len(a)
    if n<3: return 0
    ma,mb=sum(a)/n,sum(b)/n
    num=sum((x-ma)*(y-mb) for x,y in zip(a,b))
    da=sum((x-ma)**2 for x in a)**.5; db=sum((y-mb)**2 for y in b)**.5
    return num/(da*db) if da*db else 0
by={}
for x in r: by.setdefault(x[1],{})[x[0]]=x
print("\n=== 1. IS THE VARIANCE HOST-DRIVEN? (mig arm only) ===")
m=[x for x in r if x[0]=='mig']
if len(m)>=3:
    hw=[float(x[5]) for x in m]; tv=[float(x[2]) for x in m]; sv=[int(x[3])/1e9 for x in m]
    print(f"  n={len(m)}  host wait {min(hw):.0f}-{max(hw):.0f} ms/s   time {min(tv):.1f}-{max(tv):.1f}s   spin {min(sv):.0f}-{max(sv):.0f}s")
    print(f"  r(host_wait, time) = {corr(hw,tv):+.2f}    r(host_wait, spin) = {corr(hw,sv):+.2f}")
    print("  -> a strong positive r means slow/high-spin reps ARE host preemption bursts")
print("\n=== 2. DOSE-RESPONSE on an INDEPENDENT axis ===")
X,Yp,Ys=[],[],[]
for rep,d in sorted(by.items(), key=lambda z:int(z[0])):
    if 'mig' not in d or 'as' not in d: continue
    hw=(float(d['mig'][5])+float(d['as'][5]))/2
    m_,a_=int(d['mig'][3]),int(d['as'][3])
    X.append(hw); Yp.append(100*(m_-a_)/m_); Ys.append((m_-a_)/1e9)
if len(X)>=3:
    print(f"  n={len(X)}  r(host_wait, AS spin saved %) = {corr(X,Yp):+.2f}   r(host_wait, sec saved) = {corr(X,Ys):+.2f}")
    print(f"  host wait (ms/s) {[round(x) for x in X]}")
    print(f"  AS saved pct     {[round(y,1) for y in Yp]}")
    print(f"  AS saved sec     {[round(y,1) for y in Ys]}")
    md=st.median(X)
    e=[y for x,y in zip(X,Yp) if x<=md]; h=[y for x,y in zip(X,Yp) if x>md]
    print(f"  LOW-preemption half  mean {st.mean(e):+6.2f}%  ({sum(1 for z in e if z>0)}/{len(e)} positive)")
    print(f"  HIGH-preemption half mean {st.mean(h):+6.2f}%  ({sum(1 for z in h if z>0)}/{len(h)} positive)")
print("\n=== 3. headline paired ===")
dt=[];ds=[]
for rep,d in sorted(by.items(), key=lambda z:int(z[0])):
    if 'mig' not in d or 'as' not in d: continue
    dt.append(100*(float(d['mig'][2])-float(d['as'][2]))/float(d['mig'][2]))
    ds.append(100*(int(d['mig'][3])-int(d['as'][3]))/int(d['mig'][3]))
if dt: print(f"  AS vs mig+t1: TIME median {st.median(dt):+.2f}% ({sum(1 for z in dt if z>0)}/{len(dt)})   "
             f"SPIN median {st.median(ds):+.2f}% ({sum(1 for z in ds if z>0)}/{len(ds)})")
