#!/usr/bin/env python3
"""p11_report.py <tsv> [workload] -- performance gain AND spin-time reduction.

SAVED, two cases, and conflating them is wrong:

  THROUGHPUT workloads (ebizzy -S 15, dbench -t 15, memtier --test-time=10) run
  a FIXED DURATION, so the AS arm completes MORE work in the same seconds.
  PV's spin must be normalised to the work the AS arm actually did:
        saved = spin_pv * (ops_as / ops_pv) - spin_as          <- same as point 9

  TIME workloads (hackbench -l150000, vips on one image) do FIXED WORK and vary
  in duration. Both arms perform IDENTICAL work, so there is nothing to
  normalise and applying the ratio would double-count the speedup:
        saved = spin_pv - spin_as
"""
import sys, statistics as st
f = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else None
rows = [l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
ARMS = ['pv', '50', '100', '200', '400', '800', '1500']
wls = [want] if want else list(dict.fromkeys(r[0] for r in rows))
for w in wls:
    r = [x for x in rows if x[0] == w and x[4] not in ('NA', '')]
    if not r:
        continue
    met = r[0][1]
    g = {}
    for a in ARMS:
        v = [x for x in r if x[2] == a]
        if v:
            g[a] = dict(val=[float(x[4]) for x in v], spin=[int(x[12])/1e9 for x in v],
                        halt=[int(x[14])/1e9 for x in v],
                        t2f=[int(x[7]) for x in v], t2c=[int(x[6]) for x in v],
                        csb=[int(x[8]) for x in v], ev=[int(x[9]) for x in v])
    if 'pv' not in g:
        print(f"  {w}: no PV reference"); continue
    bv, bs = st.mean(g['pv']['val']), st.mean(g['pv']['spin'])
    bh = st.mean(g['pv']['halt'])
    print(f"\n  == {w}  [{met}]   PV {bv:.2f}, PV spin {bs:.4f}s ==")
    print(f"  {'arm':>6s} {'value':>10s} {'gain':>8s} {'spin_s':>8s} {'saved_s':>9s} "
          f"{'halt_s':>8s} {'dhalt':>8s} {'xfer':>6s} {'t2 f/chk':>16s} {'csbail':>7s}")
    print("  xfer = -dhalt/saved; ~1.0 means spin was MOVED into halt, not removed")
    for a in ARMS:
        if a not in g: continue
        d = g[a]; mv, ms = st.mean(d['val']), st.mean(d['spin'])
        if a == 'pv':
            gain, saved = '', 0.0
        elif met == 'TIME':
            gain = f"{100*(bv-mv)/bv:+7.2f}%"
            saved = bs - ms                      # fixed work: no normalisation
        else:
            gain = f"{100*(mv-bv)/bv:+7.2f}%"
            saved = bs * (mv/bv) - ms            # fixed duration: normalise
        fr = (f"{st.mean(d['t2f']):.0f}/{st.mean(d['t2c']):.0f}"
              + (f" {100*st.mean(d['t2f'])/st.mean(d['t2c']):.0f}%" if st.mean(d['t2c']) else ""))
        mh = st.mean(d['halt']); dh = mh - bh
        xf = f"{-dh/saved:+6.2f}" if (a != 'pv' and abs(saved) > 1e-9) else ""
        print(f"  {a:>6s} {mv:10.2f} {gain:>8s} {ms:8.4f} {saved:+9.4f} {mh:8.4f} {dh:+8.4f} "
              f"{xf:>6s} {fr:>16s} {st.mean(d['csb']):7.0f}")
