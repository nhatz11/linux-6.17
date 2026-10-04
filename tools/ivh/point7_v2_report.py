#!/usr/bin/env python3
"""Point 7 v2 report: performance + spin reduction (both definitions) + anomaly flags."""
import sys, os, statistics, collections

out = sys.argv[1]
KIND = {"memtier_memcached":"hi","hackbench_pipe_thr":"lo","ebizzy_mmap":"hi",
        "dbench_16":"hi","nhextend_full":"hi","parsec_vips":"lo"}
SH = {"memtier_memcached":"memtier","hackbench_pipe_thr":"hackbench","ebizzy_mmap":"ebizzy",
      "dbench_16":"dbench","nhextend_full":"ext-sched","parsec_vips":"vips"}
ORDER = list(KIND)
NS_PER_ITER = 23.5

rows = []
with open(os.path.join(out, "raw.tsv")) as f:
    hdr = next(f).rstrip("\n").split("\t")
    for ln in f:
        p = ln.rstrip("\n").split("\t")
        if len(p) < len(hdr) or p[3] == "NA":
            continue
        d = dict(zip(hdr, p))
        try: d["value"] = float(d["value"])
        except ValueError: continue
        for k in hdr[4:]:
            d[k] = float(d[k])
        d["A"] = (d["node_i"]+d["node_si"]+d["head_i"]+d["head_bi"])*NS_PER_ITER/1e9
        d["B"] = (d["wait_ns"] - d["halt_ns"])/1e9          # G-LOCK-53 fixed form
        d["Bold"] = (d["wait_ns"] - (d["node_halt_c"]+d["head_halt_c"])/2.2)/1e9
        rows.append(d)

by = collections.defaultdict(list)
for d in rows: by[(d["arm"], d["bench"])].append(d)
arms = ["pv"] + sorted({d["arm"] for d in rows if d["arm"] != "pv"}, key=int)
benches = [b for b in ORDER if any((a,b) in by for a in arms)]

def med(a,b,k): 
    v=by.get((a,b)); return statistics.median(x[k] for x in v) if v else None

print("PERFORMANCE  (% vs PV; TIME -> time saved, THROUGHPUT -> gain)\n")
print(f"{'arm':>8} {'fire%':>7} {'migs':>10} " + " ".join(f"{SH[b]:>10}" for b in benches))
for a in arms:
    ev=sum(d["g2_eval"] for d in rows if d["arm"]==a); fi=sum(d["g2_fired"] for d in rows if d["arm"]==a)
    mg=sum(d["migs"] for d in rows if d["arm"]==a)
    lbl = "pv" if a=="pv" else f"{int(a)/1e6:g}ms"
    row=f"{lbl:>8} {(f'{100*fi/ev:.1f}%' if ev else '-'):>7} {mg:>10,.0f} "
    for b in benches:
        m=med(a,b,"value"); pv=med("pv",b,"value")
        if m is None or pv is None: row+=f"{'-':>10} "; continue
        if a=="pv": row+=f"{m:>10.0f} " if m>=1000 else f"{m:>10.2f} "
        else:
            pct=(pv-m)/pv*100 if KIND[b]=="lo" else (m-pv)/pv*100
            row+=f"{pct:>+9.1f}% "
    print(row)

for defn,lab in (("A","A: iterations x 23.5ns  (exact, never negative)"),
                 ("B","B: wait_ns - halt_ns   (G-LOCK-53: same gate, same clock, subset by construction)")):
    print(f"\n\nSPIN TIME, definition {lab}\n")
    print(f"{'arm':>8} " + " ".join(f"{SH[b]:>10}" for b in benches))
    for a in arms:
        row=f"{(('pv' if a=='pv' else f'{int(a)/1e6:g}ms')):>8} "
        for b in benches:
            m=med(a,b,defn); pv=med("pv",b,defn)
            if m is None: row+=f"{'-':>10} "; continue
            if a=="pv": row+=f"{m:>9.2f}s "
            else:
                row += f"{(pv-m)/pv*100:>+9.1f}% " if pv and pv>0 else f"{'n/a':>10} "
        print(row)
    if defn=="B":
        bad=[(a,b) for a in arms for b in benches if med(a,b,"B") is not None and med(a,b,"B")<0]
        if bad:
            print("\n  NEGATIVE (B invalid here): " + ", ".join(f"{SH[b]}@{'pv' if a=='pv' else str(int(a)/1e6)+'ms'}" for a,b in bad))

print("\n\nANOMALY SCAN  (spread > 10% CV, or a rep >20% off its arm median)\n")
flagged=False
for a in arms:
    for b in benches:
        v=[x["value"] for x in by.get((a,b),[])]
        if len(v)<2: continue
        cv=100*statistics.stdev(v)/statistics.mean(v); m=statistics.median(v)
        outl=[x for x in v if m and abs(x-m)/m>0.20]
        if cv>10 or outl:
            flagged=True
            lbl="pv" if a=="pv" else f"{int(a)/1e6:g}ms"
            print(f"  {lbl:>7} {SH[b]:<11} CV={cv:5.1f}%  vals={[round(x,1) for x in v]}"
                  + ("  <-- OUTLIER" if outl else ""))
if not flagged: print("  none")
