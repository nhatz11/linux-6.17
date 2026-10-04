import subprocess
n=["ivh_hash_ins_kick","ivh_hash_ins_head","ivh_hash_rel_unhash","ivh_hash_rel_lp"]
o=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+n,capture_output=True,text=True).stdout
v={l.split()[0]:int(l.split()[2]) for l in o.strip().splitlines() if len(l.split())>2}
ins=v.get("ivh_hash_ins_kick",0)+v.get("ivh_hash_ins_head",0)
rel=v.get("ivh_hash_rel_unhash",0)+v.get("ivh_hash_rel_lp",0)
for k in n: print(f"  {k:<24} {v.get(k,0):>12,}")
print(f"  {'INSERTS':<24} {ins:>12,}")
print(f"  {'RELEASES':<24} {rel:>12,}")
print(f"  {'LEAKED (ins-rel)':<24} {ins-rel:>12,}   <-- must equal the live gauge")
