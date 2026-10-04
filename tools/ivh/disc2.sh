#!/bin/bash
set -u
CPU=0; SECS=90
echo 2 > /proc/sys/kernel/ivh_preempt_event_source
snap(){ python3 - "$CPU" <<'PY'
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
cpu=int(sys.argv[1]); sym=r.load_kallsyms()
f=open(r.KCORE,"rb"); ph=r.read_phdrs(f)
off=r.read_u64(f,ph,sym["__per_cpu_offset"]+8*cpu); b=sym["runqueues"]
print(" ".join(str(r.read_u64(f,ph,b+fl+off)) for fl in (3984,3992,3920)))
PY
}
read j0 i0 s0 <<< "$(snap)"
./vcpu_trace $CPU $SECS 1 2200000 /tmp/disc_trace.bin 400
read j1 i1 s1 <<< "$(snap)"
echo "KERNEL DETECTOR cpu$CPU over ${SECS}s:"
echo "  ivh_vact_jumps          $((j1-j0))   $(( (j1-j0)/SECS ))/s"
echo "  ivh_vact_idle_explained $((i1-i0))   $(( (i1-i0)/SECS ))/s"
echo "  jumps + idle_explained  $((j1-j0+i1-i0))   $(( (j1-j0+i1-i0)/SECS ))/s"
echo "  tick calls              $((s1-s0))   $(( (s1-s0)/SECS ))/s  (nominal 1000/s)"
python3 - <<'PY'
import struct, statistics as st
d=open('/tmp/disc_trace.bin','rb').read(); n=(len(d)-56)//16
lens=struct.unpack(f'<{n}Q', d[56+8*n:56+16*n]); us=sorted(x/2200.0 for x in lens)
print(f"\nGROUND TRUTH vcpu_trace, same window ({n} gaps total):")
for thr in (50,500,1000,1500,2000):
    print(f"  gaps > {thr:>5}us  {sum(1 for x in us if x>thr):7d}   {sum(1 for x in us if x>thr)/60:7.1f}/s")
print(f"  median {st.median(us):.1f}us  mean {st.mean(us):.1f}us  p99 {us[int(.99*len(us))]:.0f}us")
PY
echo DISC2-DONE
