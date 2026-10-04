#!/bin/bash
# Which bug do we actually have?
#   vcpu_trace gives ground truth for ONE vCPU at ~20ns resolution.
#   In the SAME window read that rq's ivh_vact_jumps and ivh_vact_idle_explained.
#     jumps + idle_expl ~= gaps>1.5ms  -> detector SEES them, discriminator misfiles them
#     both small                       -> gaps genuinely not appearing (rate/threshold)
# No host needed: vcpu_trace IS the ground truth here.
set -u
CPU=${1:-0}; SECS=${2:-60}
S=/proc/sys/kernel
echo 2 > $S/ivh_preempt_event_source
echo "cpu=$CPU secs=$SECS  jump_ns=$(cat $S/ivh_vact_jump_ns)  sampler_ns=$(cat $S/ivh_tks_sampler_ns)"
python3 - "$CPU" "$SECS" <<'PY' &
import sys,time
sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
cpu=int(sys.argv[1]); secs=float(sys.argv[2])
J,IE,SAMP=3984,3992,3920
sym=r.load_kallsyms(); f=open(r.KCORE,"rb"); ph=r.read_phdrs(f)
off=r.read_u64(f,ph,sym["__per_cpu_offset"]+8*cpu); b=sym["runqueues"]
g=lambda fl: r.read_u64(f,ph,b+fl+off)
a=(g(J),g(IE),g(SAMP)); t0=time.time(); time.sleep(secs); z=(g(J),g(IE),g(SAMP)); w=time.time()-t0
print(f"\nKERNEL DETECTOR on cpu{cpu} over {w:.0f}s:")
print(f"  ivh_vact_jumps           {z[0]-a[0]:8d}   {(z[0]-a[0])/w:8.1f}/s")
print(f"  ivh_vact_idle_explained  {z[1]-a[1]:8d}   {(z[1]-a[1])/w:8.1f}/s")
print(f"  jumps + idle_explained   {z[0]-a[0]+z[1]-a[1]:8d}   {(z[0]-a[0]+z[1]-a[1])/w:8.1f}/s")
print(f"  tick/accumulate calls    {z[2]-a[2]:8d}   {(z[2]-a[2])/w:8.1f}/s  (nominal 1000/s)")
PY
sleep 1
./vcpu_trace $CPU $SECS 1 2200000 /tmp/disc_trace.bin 400
wait
python3 - <<'PY'
import struct, statistics as st
d=open('/tmp/disc_trace.bin','rb').read(); n=(len(d)-56)//16
lens=struct.unpack(f'<{n}Q', d[56+8*n:56+16*n])
us=sorted(x/2200.0 for x in lens)
import subprocess
print("\nGROUND TRUTH (vcpu_trace, ~20ns resolution), same window:")
for thr in (50,500,1000,1500,2000,3000):
    print(f"  gaps > {thr:>5}us   {sum(1 for x in us if x>thr):8d}")
print(f"  median {st.median(us):.1f}us   mean {st.mean(us):.1f}us   p99 {us[int(.99*len(us))]:.0f}us")
PY
echo DISCRIMINATE-DONE
