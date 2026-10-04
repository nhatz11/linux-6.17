#!/bin/bash
# hostload.sh -- add/remove a HOST-SIDE CPU burner pinned to the 16 cores both
# VMs share (node0: 0-8,36-42), to raise oversubscription beyond what the
# corunner can produce.
#
# WHY THIS EXISTS: the corunner's thread count is a saturated dial. It has 16
# vCPUs, so `sysbench cpu --threads=32` consumes no more host CPU than
# --threads=16 (measured: cap_mean 776 -> 781). The only way to push past 2:1
# oversubscription without re-pinning the VMs is to add load on the host itself.
#
# This is specifying the experiment, not tuning the result: IVH targets
# oversubscribed hosts, and AS's perf delta is contention-dependent (memtier
# +2.31% at cap~650 vs -5.92% at cap~744). Report cap_mean with every number.
set -u
H="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""
CORES="0-8,36-42"
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
case "${1:-status}" in
on)
  N="${2:-8}"
  timeout 60 $H "echo "$IVH_HOST_PASS" | sudo -S systemd-run --unit=ivh-hostload --service-type=simple \
     taskset -c $CORES /usr/bin/stress-ng --cpu $N --timeout 0 2>&1 | tail -1 || \
     echo "$IVH_HOST_PASS" | sudo -S systemd-run --unit=ivh-hostload --service-type=simple \
     bash -c 'for i in \$(seq 1 $N); do taskset -c $CORES sh -c \"while :; do :; done\" & done; wait' 2>&1 | tail -1" 2>&1 | tail -1
  for i in 1 2 3 4 5 6; do sleep 20; echo "    t+$((i*20))s cap_mean=$(capm)"; done ;;
off)
  timeout 40 $H "echo "$IVH_HOST_PASS" | sudo -S systemctl stop ivh-hostload 2>/dev/null; echo stopped" 2>&1 | tail -1
  for i in 1 2 3; do sleep 15; echo "    cap_mean=$(capm)"; done ;;
status)
  timeout 40 $H "echo "$IVH_HOST_PASS" | sudo -S systemctl is-active ivh-hostload 2>/dev/null; echo \"  host load: \$(cut -d' ' -f1-3 /proc/loadavg)\"" 2>&1 | tail -2
  echo "  guest cap_mean=$(capm)" ;;
esac
