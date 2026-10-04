#!/usr/bin/env python3
import subprocess,re,sys
C=["ivh_head_blocked_cycles","ivh_head_blocked_events","ivh_head_blocked_trunc_cycles",
   "ivh_head_blocked_trunc_events","ivh_head_obs_actionable","ivh_head_blocked_hist"]
out=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+C,
                   capture_output=True,text=True,timeout=160).stdout
s={}; hist={}
for ln in out.splitlines():
    m=re.match(r'\s*ivh_head_blocked_hist\s+b?(\d+)?\s*>=\s*([\d.]+)\s+us\s+(\d+)',ln)
    if m: hist[float(m.group(2))]=int(m.group(3)); continue
    m=re.match(r'\s*(\S+)\s*=\s*(\d+)\s*$',ln)
    if m: s[m.group(1)]=int(m.group(2))
print(" ".join(str(s.get(k,0)) for k in C[:5]))
