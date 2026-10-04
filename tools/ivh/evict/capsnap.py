import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    v=[r.read_u64(f,ph,sym["runqueues"]+0xef0+o) for o in offs]
print(f"mean={sum(v)//len(v)} min={min(v)} max={max(v)}")
