import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
sym=r.load_kallsyms()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    live=r.read_u64(f,ph,sym["ivh_pv_hash_live"]) & 0xffffffff
    hwm =r.read_u64(f,ph,sym["ivh_pv_hash_hwm"])  & 0xffffffff
print(f"pv_hash live={live} hwm={hwm}   (invariant: <= 4*16 = 64; table = 256)")
