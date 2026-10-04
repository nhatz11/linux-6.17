#!/usr/bin/env python3
"""Primary metric: spin per SLOWPATH ENTRY vs the CLEAN migration-only arm."""
import sys, statistics as st
f = sys.argv[1]; want = sys.argv[2] if len(sys.argv) > 2 else None
rows = [l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
ARMS = ['pv','mig','500','1500','3000','5000']
for w in ([want] if want else list(dict.fromkeys(r[0] for r in rows))):
    rr = [x for x in rows if x[0]==w and x[4] not in ('NA','')]
    if not rr: continue
    met = rr[0][1]; g = {}
    for a in ARMS:
        v = [x for x in rr if x[2]==a]
        if v: g[a] = dict(n=len(v), val=st.mean([float(x[4]) for x in v]),
                          spin=st.mean([int(x[5]) for x in v]), halt=st.mean([int(x[6]) for x in v]),
                          ent=st.mean([int(x[7]) for x in v]), t2f=st.mean([int(x[8]) for x in v]),
                          t2c=st.mean([int(x[9]) for x in v]), csb=st.mean([int(x[10]) for x in v]),
                          ev=st.mean([int(x[11]) for x in v]))
    if 'mig' not in g: continue
    M = g['mig']; Mspe = M['spin']/M['ent'] if M['ent'] else 0
    print(f"\n== {w} [{met}]  CLEAN mig ref: val {M['val']:.2f}  spin {M['spin']/1e9:.3f}s  "
          f"entries {M['ent']:.0f}  spin/entry {Mspe:.0f}ns")
    print(f"  {'arm':>6s} {'n':>2s} {'value':>10s} {'perf':>8s} | {'spin/ent':>9s} {'SPIN/ENT':>9s} | "
          f"{'spin_tot':>9s} {'entries':>8s} | {'t2%':>5s} {'csbail':>7s} {'evict':>6s}")
    for a in ARMS:
        if a not in g: continue
        d = g[a]; spe = d['spin']/d['ent'] if d['ent'] else 0
        if a == 'mig': perf = pe = sp = en = ''
        else:
            perf = f"{100*(M['val']-d['val'])/M['val']:+7.2f}%" if met=='TIME' else f"{100*(d['val']-M['val'])/M['val']:+7.2f}%"
            pe = f"{100*(Mspe-spe)/Mspe:+8.2f}%" if Mspe else ''
            sp = f"{100*(M['spin']-d['spin'])/M['spin']:+8.2f}%"
            en = f"{100*(d['ent']-M['ent'])/M['ent']:+7.1f}%"
        t2 = f"{100*d['t2f']/d['t2c']:.1f}" if d['t2c'] else "-"
        print(f"  {a:>6s} {d['n']:2d} {d['val']:10.2f} {perf:>8s} | {spe:9.0f} {pe:>9s} | "
              f"{sp:>9s} {en:>8s} | {t2:>5s} {d['csb']:7.0f} {d['ev']:6.0f}")
    best = [(a, (Mspe-(g[a]['spin']/g[a]['ent'] if g[a]['ent'] else 0))/Mspe) for a in ARMS if a not in ('pv','mig') and a in g]
    if best and Mspe:
        b = max(best, key=lambda z: z[1])
        print(f"  --> best shared threshold for {w}: {b[0]}us  (spin/entry {100*b[1]:+.2f}%)")
