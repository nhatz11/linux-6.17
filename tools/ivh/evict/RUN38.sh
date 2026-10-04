#!/bin/bash
# G-LOCK-38 campaign. STEP 0 is the MANDATORY acceptance check: with every new
# knob at 0 the new kernel must reproduce G-LOCK-37's numbers, or no arm below
# is interpretable.
set -u; D=/root/ivh_tools/evict; L=/root/ivh_logs/run38_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== RUN38 start $(date -Is) kernel=$(uname -r) ncpu=$(nproc) ==="
case "$(uname -r)" in *G-LOCK-38*) ;; *) echo "WRONG KERNEL"; exit 1;; esac
echo "### STEP 0: ACCEPTANCE -- must reproduce -6.16% (hop1) / -14.88% (hop4)"
for hop in 1 4; do
  echo "--- accept hop_cap=$hop"
  TAG=accept BLOCKS=8 DUR=10 HOP=$hop ARMS="base skip" timeout -k 30 3600 "$D/dissect1.sh"
done
echo "### STEP 1: the four-arm bracket, hop_cap=1"
TAG=brk1 BLOCKS=8 DUR=10 HOP=1 ARMS="base skip nosteal noreq" timeout -k 30 5400 "$D/dissect1.sh"
echo "### STEP 2: the four-arm bracket, hop_cap=4"
TAG=brk4 BLOCKS=8 DUR=10 HOP=4 ARMS="base skip nosteal noreq" timeout -k 30 5400 "$D/dissect1.sh"
echo "### STEP 3: look-ahead (the only candidate FIX), both hop caps"
for hop in 1 2 4; do
  TAG=la$hop BLOCKS=8 DUR=10 HOP=$hop ARMS="base skip lookahead" timeout -k 30 5400 "$D/dissect1.sh"
done
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none ivh_pv_evict_lookahead; do echo 0 > /proc/sys/kernel/$k 2>/dev/null; done
echo 1 > /proc/sys/kernel/ivh_pv_evict_hop_cap
echo "=== RUN38 COMPLETE $(date -Is) ==="; ls -t $D/accept_*.csv $D/brk*.csv $D/la*.csv 2>/dev/null | head
