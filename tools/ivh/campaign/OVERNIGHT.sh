#!/bin/bash
set -u; C=/root/ivh_tools/campaign; L=/root/ivh_logs/overnight_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== OVERNIGHT start $(date -Is) kernel=$(uname -r) ncpu=$(nproc) ==="
echo "### PHASE A: 6 arms x 5 workloads x 10 blocks (~70 min)  $(date -Is)"
OUT=$C/PHASEA LIST=$C/bench5.tsv BLOCKS=10 ARMS="pv byp t12 t12byp combo comboByp" \
  timeout -k 60 14400 "$C/run_arms.sh"; echo "  phaseA exit=$?"
echo "--- PHASE A health: rows=$(tail -n +2 $C/PHASEA/results.csv 2>/dev/null|wc -l) fails=$(grep -c ',FAIL,' $C/PHASEA/results.csv 2>/dev/null)"
echo "### PHASE B: threshold sweep, 4 thr x 2 arms x 5 wl x 6 blocks (~56 min)  $(date -Is)"
OUT=$C/PHASEB LIST=$C/bench5.tsv BLOCKS=6 \
  timeout -k 60 14400 "$C/run_thresh.sh"; echo "  phaseB exit=$?"
echo "--- PHASE B health: rows=$(tail -n +2 $C/PHASEB/results.csv 2>/dev/null|wc -l) fails=$(grep -c ',FAIL,' $C/PHASEB/results.csv 2>/dev/null)"
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_evict_lookahead ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed; do echo 0 > /proc/sys/kernel/$k 2>/dev/null; done
echo 220000 > /proc/sys/kernel/ivh_pv_beat_threshold; echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== OVERNIGHT COMPLETE $(date -Is) ==="
