#!/usr/bin/env python3
"""Summarise acq_suite.sh output: median rates, floor subtraction, stability."""
import sys, csv, statistics as st

def load(paths):
    rows={}
    for p in paths:
        for r in csv.DictReader(open(p)):
            rows.setdefault(r['workload'],[]).append(r)
    return rows

def main(paths):
    rows=load(paths)
    floor_rows=rows.get('idle_floor',[])
    fl=[float(r['acq_per_s']) for r in floor_rows]
    fls=[float(r['slow_per_s']) for r in floor_rows]
    floor=st.median(fl) if fl else 0.0
    floor_s=st.median(fls) if fls else 0.0
    print(f"# idle floor: {floor:,.0f} acq/s   {floor_s:,.0f} slowpath/s   "
          f"(n={len(fl)}, min={min(fl):,.0f} max={max(fl):,.0f})\n")
    hdr=(f"{'workload':22} {'n':>2} {'wall_s':>7} {'acq/s':>12} {'net acq/s':>12} "
         f"{'xfloor':>7} {'slow/s':>10} {'slow%':>6} {'spread':>7} {'CV%':>6}")
    print(hdr); print('-'*len(hdr))
    out=[]
    for w,rs in rows.items():
        if w=='idle_floor': continue
        a=[float(r['acq_per_s']) for r in rs]
        s=[float(r['slow_per_s']) for r in rs]
        d=[float(r['seconds']) for r in rs]
        med=st.median(a); net=med-floor
        out.append((med,w,len(a),st.median(d),med,net,med/floor if floor else 0,
                    st.median(s),100*st.median(s)/max(med,1),
                    max(a)/max(min(a),1),
                    100*st.pstdev(a)/st.mean(a) if len(a)>1 else 0.0))
    for _,w,n,d,med,net,xf,sl,slp,sp,cv in sorted(out, reverse=True):
        flag=''
        if d < 1.0: flag=' <-WINDOW<1s'
        elif xf < 1.5: flag=' <-AT FLOOR'
        elif xf < 3: flag=' <-near floor'
        print(f"{w:22} {n:2d} {d:7.1f} {med:12,.0f} {net:12,.0f} {xf:6.1f}x "
              f"{sl:10,.0f} {slp:5.2f}% {sp:6.2f}x {cv:5.1f}{flag}")

if __name__=='__main__':
    main(sys.argv[1:])
