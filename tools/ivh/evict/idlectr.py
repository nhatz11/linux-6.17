#!/usr/bin/env python3
# Sum ivh_rot_idle_cycles/events across ALL classes (they are class-indexed
# arrays with no TOTAL row), plus the scalar counters. One line, 5 numbers.
import subprocess,re
C=["ivh_rot_idle_cycles","ivh_rot_idle_events","ivh_rot_idle_unknown",
   "ivh_rot_idle_capped","ivh_evict_marked"]
out=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+C,
                   capture_output=True,text=True,timeout=150).stdout
cyc=ev=unk=cap=mk=0
for ln in out.splitlines():
    m=re.match(r'\s*(\S+)\s*\[[^\]]*\]\s*=\s*(\d+)',ln)
    if m:
        if m.group(1)=="ivh_rot_idle_cycles": cyc+=int(m.group(2))
        elif m.group(1)=="ivh_rot_idle_events": ev+=int(m.group(2))
        continue
    m=re.match(r'\s*(\S+)\s*=\s*(\d+)',ln)
    if m:
        k,v=m.group(1),int(m.group(2))
        if k=="ivh_rot_idle_unknown": unk=v
        elif k=="ivh_rot_idle_capped": cap=v
        elif k=="ivh_evict_marked": mk=v
print(cyc,ev,unk,cap,mk)
