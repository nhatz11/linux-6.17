#!/bin/bash
set -u; D=/root/ivh_tools/evict; L=/root/ivh_logs/cwl_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== CWL start $(date -Is) kernel=$(uname -r) ==="
for hop in 2 4; do
  echo "### hackbench hop_cap=$hop"
  BLOCKS=10 HOP=$hop WLS="hackbench" timeout -k 30 5400 "$D/stepcombowl.sh"
done
echo "### dbench hop_cap=2"
BLOCKS=8 HOP=2 WLS="dbench" timeout -k 30 5400 "$D/stepcombowl.sh"
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_evict_lookahead; do echo 0 > /proc/sys/kernel/$k; done
echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== CWL COMPLETE $(date -Is) ==="
