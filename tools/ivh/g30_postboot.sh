#!/bin/bash
# Run FIRST after booting into G-LOCK-30. Restores the two userspace pieces the
# kernel needs and that do not survive a reboot.
set -u
echo "kernel: $(uname -r)"
# REMOVED 2026-10-01 (vcap_probe deleted -- obsolete since vcap measures its own demand, and it inflated every PV-relative number): cd /root/vcapacity && (pgrep -x vcap_probe >/dev/null || (nohup ./vcap_probe -p 200 -s 200 >/dev/null 2>&1 & sleep 1))
# --- vcap: the measurement daemon (TSC-gap steal -> capacity + active time).
# Replaces vcap_probe, deleted 2026-10-01: it computed nothing and inflated
# every PV-relative number by damaging the baseline (IVH is insensitive to it,
# PV is not).  vcap needs /proc/ivh_cpu_stats (G-LOCK-51+) for tsc_khz and
# exits immediately without it, so it is guarded.
# ivh_ucw_max_age_ns MUST exceed vcap's loop period (~5.2s at -p 200 -s 5000)
# or the staleness watchdog expires capacity to 1024 between publishes and the
# arm silently becomes "IVH off".  A mismatch here is SILENT.
if [ -e /proc/ivh_cpu_stats ]; then
    [ -e /proc/sys/kernel/ivh_ucw_max_age_ns ] && echo 16000000000 > /proc/sys/kernel/ivh_ucw_max_age_ns
    cd /root/vcapacity && (pgrep -x vcap >/dev/null || (nohup ./vcap -p 200 -s 5000 >/root/ivh_logs/vcap.log 2>&1 & sleep 2))
fi
cd /root/linux-6.17/cvm_setup && (pgrep -x MY_ivh_atc >/dev/null || (nohup /root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc >/root/atc_g30.log 2>&1 & sleep 3))
echo "vcap       : $(pgrep -x vcap >/dev/null && echo RUNNING || echo MISSING)"
echo "MY_ivh_atc : $(pgrep -x MY_ivh_atc >/dev/null && echo RUNNING || echo MISSING)"
echo "selector   : $(bpftool link list 2>/dev/null | grep -c 'target_btf_id') tracing links"
echo "--- capacity settling (contended half should reach ~465-505):"
for i in $(seq 1 8); do
  python3 /root/ivh_tools/read_vact_rq.py ivh_uc_capacity 2>/dev/null \
   | sed 's/.*per-cpu=\[//;s/\].*//' \
   | python3 -c "import sys;v=[int(x) for x in sys.stdin.read().split(',')];print(f'  contended={sum(v[:8])/8:6.1f}  free={sum(v[8:])/8:6.1f}')"
  sleep 8
done
