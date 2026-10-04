import sys, statistics as st
f, wl = sys.argv[1], sys.argv[2]
rows = [l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
r = [x for x in rows if x[1] == wl and x[5] != 'NA']
if not r:
    print(f"  {wl}: no data"); sys.exit()
metric = r[0][2]
by, mig = {}, {}
for x in r:
    by.setdefault(x[3], []).append(float(x[5]))
    mig.setdefault(x[3], []).append(int(x[6]))
if 'pv' not in by or 'mig' not in by:
    print(f"  {wl}: incomplete arms"); sys.exit()
A, B = by['pv'], by['mig']
# ratio-of-means, never mean-of-ratios: on a baseline whose denominator swings
# (dedup OFF spans 17-189s) mean-of-ratios understates badly -- it turned
# dedup's +88% into +77.55% and vips's +18.59% into -81.54%.
ma, mb = st.mean(A), st.mean(B)
gain = 100 * (ma - mb) / ma if metric == 'TIME' else 100 * (mb - ma) / ma
def cv(v): return 100 * st.stdev(v) / st.mean(v) if len(v) > 1 else 0.0
sa = st.stdev(A) / len(A) ** .5 if len(A) > 1 else 0
sb = st.stdev(B) / len(B) ** .5 if len(B) > 1 else 0
se = 100 * ((sa ** 2 + sb ** 2) ** .5) / ma
print(f"  >> {wl:<10s} pv {ma:10.1f} (CV {cv(A):4.1f}%)   mig {mb:10.1f} (CV {cv(B):4.1f}%)   "
      f"{gain:+6.2f}% +/-{se:4.2f}  {'SIG' if abs(gain) > 2 * se else 'ns '}  "
      f"n={len(A)}/{len(B)}  migrations={int(st.mean(mig['mig']))}/run")
# A high baseline CV is NOT disqualifying. On dedup it IS the phenomenon: the OFF
# arm stalls on lock-holder preemption (CV 44-76%, 17-189s) and migration
# collapses it to ~9s at CV 8-12%. Test whether the TREATMENT arm is calm.
if cv(A) > 25:
    if cv(B) < cv(A) / 2:
        print(f"     ++ pv CV {cv(A):.0f}% -> mig CV {cv(B):.0f}%: variance COLLAPSE -- "
              f"the baseline spread is the signal, not noise")
    else:
        print(f"     !! pv CV {cv(A):.0f}% and mig CV {cv(B):.0f}% both high -- "
              f"no variance collapse, do not quote this row")
