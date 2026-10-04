#!/bin/bash
# Does look-ahead + nosteal together beat base? Both knobs already in G-LOCK-38.
#   lookahead : evict ONLY when a live replacement is confirmed
#   nosteal   : the victim does not camp on the lock on re-entry
set -u; D=/root/ivh_tools/evict; L=/root/ivh_logs/combo_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== COMBO start $(date -Is) kernel=$(uname -r) ==="
for hop in 1 2 4; do
  echo "### hop_cap=$hop"
  TAG=cmb$hop BLOCKS=10 DUR=10 HOP=$hop ARMS="base skip lookahead combo" \
    timeout -k 30 5400 "$D/dissect1.sh"
done
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none ivh_pv_evict_lookahead; do echo 0 > /proc/sys/kernel/$k 2>/dev/null; done
echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== COMBO COMPLETE $(date -Is) ==="
