#!/usr/bin/env python3
"""Primary metric: ITERS PER SPIN PASS vs mig+t1. Immune to entry-count shifts."""
import sys, statistics as st
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
rows=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
ARMS=['mig','200m4095','400m4095','800m4095','200m255','400m255','800m255']
for w in ([want] if want else list(dict.fromkeys(r[0] for r in rows))):
    rr=[x for x in rows if x[0]==w and x[4] not in('NA','')]
    if not rr: continue
    met=rr[0][1]; g={}
    for a in ARMS:
        v=[x for x in rr if x[2]==a]
        if v: g[a]=dict(n=len(v),val=st.mean([float(x[4]) for x in v]),spin=st.mean([int(x[5]) for x in v]),
                        ent=st.mean([int(x[7]) for x in v]),pas=st.mean([int(x[8]) for x in v]),
                        it=st.mean([int(x[9]) for x in v]),t2f=st.mean([int(x[11]) for x in v]),
                        t2c=st.mean([int(x[12]) for x in v]),csb=st.mean([int(x[13]) for x in v]),ev=st.mean([int(x[14]) for x in v]))
    if 'mig' not in g: continue
    M=g['mig']; Mipp=M['it']/M['pas'] if M['pas'] else 0
    print(f"\n== {w} [{met}]  ref mig+t1: val {M['val']:.2f} spin {M['spin']/1e9:.3f}s iters/pass {Mipp:.0f}")
    print(f"  {'arm':>9s} {'n':>2s} {'value':>10s} {'perf':>8s} | {'it/pass':>8s} {'IT/PASS':>9s} | {'spin_tot':>9s} | {'t2%':>5s} {'csb':>6s} {'ev':>5s}")
    for a in ARMS:
        if a not in g: continue
        d=g[a]; ipp=d['it']/d['pas'] if d['pas'] else 0
        if a=='mig': perf=dp=ds=''
        else:
            perf=f"{100*(M['val']-d['val'])/M['val']:+7.2f}%" if met=='TIME' else f"{100*(d['val']-M['val'])/M['val']:+7.2f}%"
            dp=f"{100*(Mipp-ipp)/Mipp:+8.2f}%" if Mipp else ''
            ds=f"{100*(M['spin']-d['spin'])/M['spin']:+8.2f}%"
        t2=f"{100*d['t2f']/d['t2c']:.1f}" if d['t2c'] else "-"
        print(f"  {a:>9s} {d['n']:2d} {d['val']:10.2f} {perf:>8s} | {ipp:8.0f} {dp:>9s} | {ds:>9s} | {t2:>5s} {d['csb']:6.0f} {d['ev']:5.0f}")
    ok=[(a,100*(M['spin']-g[a]['spin'])/M['spin'],
         100*(Mipp-(g[a]['it']/g[a]['pas'] if g[a]['pas'] else 0))/Mipp if Mipp else 0)
        for a in ARMS if a!='mig' and a in g]
    good=[z for z in ok if z[1]>=-1.0]
    print(f"  --> spin-neutral-or-better arms (>= -1%): {[(z[0], round(z[1],2)) for z in good] or 'NONE'}")
