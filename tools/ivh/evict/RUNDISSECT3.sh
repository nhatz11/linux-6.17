#!/bin/bash
set -u; D=/root/ivh_tools/evict; L=/root/ivh_logs/dissect3_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== DISSECT3 (head-halt discriminator) start $(date -Is) ==="
for hop in 1 4; do
  echo "### hop_cap=$hop  $(date -Is)"
  BLOCKS=8 DUR=10 HOP=$hop REQ=4 timeout -k 30 3600 "$D/dissect1.sh"; echo "  exit=$?"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > /proc/sys/kernel/ivh_pv_evict_enable; echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== DISSECT3 COMPLETE $(date -Is) ==="
