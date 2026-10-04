#!/usr/bin/env python3
# Per-CLASS ivh_rot_idle: the class is the ACQUIRING node's rot_flags at release
# (qspinlock_paravirt.h ivh_rot_ack_slow), so:
#   "live (baseline)"        = a LIVE successor acquired via the queue
#   "stale, nowhere to skip" = stale successor, no live replacement existed
#   "stale + skippable"      = stale successor eviction COULD have acted on,
#                              which acquired through the queue anyway
import subprocess,re
C=["ivh_rot_idle_cycles","ivh_rot_idle_events","ivh_rot_idle_unknown",
   "ivh_rot_idle_backward","ivh_rot_idle_capped","ivh_evict_marked"]
out=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+C,
                   capture_output=True,text=True,timeout=160).stdout
cyc={}; ev={}; scal={}
for ln in out.splitlines():
    m=re.match(r'\s*(\S+)\s*\[\s*(.*?)\s*\]\s*=\s*(\d+)',ln)
    if m:
        n,cls,v=m.group(1),m.group(2),int(m.group(3))
        if n=="ivh_rot_idle_cycles": cyc[cls]=v
        elif n=="ivh_rot_idle_events": ev[cls]=v
        continue
    m=re.match(r'\s*(\S+)\s*=\s*(\d+)',ln)
    if m: scal[m.group(1)]=int(m.group(2))
order=["live (baseline)","stale, nowhere to skip","IMPOSSIBLE","stale + skippable"]
vals=[]
for c in order: vals += [cyc.get(c,0), ev.get(c,0)]
vals += [scal.get("ivh_rot_idle_unknown",0), scal.get("ivh_rot_idle_backward",0),
         scal.get("ivh_evict_marked",0)]
print(" ".join(str(v) for v in vals))
