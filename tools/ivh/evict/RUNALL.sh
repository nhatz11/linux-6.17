#!/bin/bash
# Detached runner: survives SSH/terminal loss. Results land on disk regardless.
set -u
D=/root/ivh_tools/evict; L=/root/ivh_logs/runall_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs
exec > "$L" 2>&1
echo "=== RUNALL start $(date -Is) ==="
echo "kernel=$(uname -r) ncpu=$(nproc) cmdline=$(cat /proc/cmdline)"
echo "tas=$(cat /proc/sys/kernel/ivh_pv_tas) allow=$(cat /proc/sys/kernel/ivh_pv_allow)"
echo
echo "### STEP 1/2: stepnogain.sh (sizes professor pointer 1) $(date -Is)"
BLOCKS=6 DUR=10 timeout -k 30 3600 "$D/stepnogain.sh"; echo "  exit=$?"
echo
echo "### STEP 2/2: steptier.sh (professor pointer 2) $(date -Is)"
BLOCKS=10 DUR=10 timeout -k 30 7200 "$D/steptier.sh"; echo "  exit=$?"
echo
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > /proc/sys/kernel/ivh_pv_evict_enable
echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== RUNALL COMPLETE $(date -Is) ==="
echo "results:"; ls -t $D/nogain_*.csv $D/tier_*.csv 2>/dev/null | head -4
