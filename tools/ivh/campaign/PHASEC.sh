#!/bin/bash
# PHASE C: the arm Phase A was missing. Does EVICTION help ON TOP of the winner?
#   t12       tier1+tier2                      (the Phase A winner)
#   t12combo  tier1+tier2 + best-skip          <- eviction's clean contribution
#   all       tier1+tier2 + best-skip + bypass
# t12combo vs t12 moves ONE variable: ivh_pv_evict_enable (+lookahead/nosteal).
set -u; C=/root/ivh_tools/campaign; L=/root/ivh_logs/phasec_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== PHASE C start $(date -Is) ==="
OUT=$C/PHASEC LIST=$C/bench5.tsv BLOCKS=10 ARMS="pv t12 t12combo all" \
  timeout -k 60 14400 "$C/run_arms.sh"; echo "  exit=$?"
echo "--- health: rows=$(tail -n +2 $C/PHASEC/results.csv 2>/dev/null|wc -l) fails=$(grep -c ',FAIL,' $C/PHASEC/results.csv 2>/dev/null)"
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_evict_lookahead ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed; do echo 0 > /proc/sys/kernel/$k 2>/dev/null; done
echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== PHASE C COMPLETE $(date -Is) ==="
