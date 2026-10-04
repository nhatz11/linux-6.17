import re, sys
MHZ=2200.0
def rows(s):
    return [{int(x):int(y) for x,y in re.findall(r'\((\d+),\s*(\d+)\)',p)} for p in s.split('|') if p.strip()]
a,b=rows(sys.argv[1]),rows(sys.argv[2])
while len(a)<6: a.append({})
while len(b)<6: b.append({})
d=[{k:b[i].get(k,0)-a[i].get(k,0) for k in set(a[i])|set(b[i])} for i in range(6)]
print(f"{'bucket':>7} {'>= us':>10} {'long+queued':>12} {'head halted':>12} {'halted%':>9}")
for k in range(14,32):
    spin=max(d[3].get(k,0),0); hal=max(d[4].get(k,0),0); n=spin+hal
    if n<=0: continue
    print(f"  b{k:<4} {(2.0**k)/MHZ:10.1f} {n:12d} {hal:12d} {100.0*hal/n:8.1f}%")
