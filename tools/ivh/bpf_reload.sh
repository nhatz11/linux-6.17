#!/bin/bash
# Reload MY_ivh_atc. Ordering per IVH_start.sh + the recorded gotcha that
# ivh_cfg must be written before ivh_universal_eligible=1, else every target
# resolves to CPU 0. We lower eligible first so the ordering holds either way.
# NOTE: reject_reasons is re-created by the reload, so counters restart at 0.
set -u; S=/proc/sys/kernel; B=/root/kernels/linux-6.17-vanilla/tools/bpf
PCT="${1:-0}"
echo 0 > $S/ivh_universal_eligible
pkill -9 -x MY_ivh_atc 2>/dev/null
for i in $(seq 1 50); do pgrep -x MY_ivh_atc >/dev/null || break; sleep 0.2; done
pgrep -x MY_ivh_atc >/dev/null && { echo "FATAL: old loader survived"; exit 1; }
setsid nohup $B/MY_ivh_atc > /root/ivh_logs/atc.log 2>&1 < /dev/null &
for i in $(seq 1 100); do bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break; sleep 0.2; done
bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0 || { echo "FATAL: cfg[0]"; exit 1; }
bpftool map update name ivh_cfg key 1 0 0 0 value $PCT 0 0 0 || { echo "FATAL: cfg[1]"; exit 1; }
echo 1 > $S/ivh_universal_eligible
G0=$(bpftool map lookup name ivh_cfg key 0 0 0 0 | grep -oP '"value": \K[0-9]+')
G1=$(bpftool map lookup name ivh_cfg key 1 0 0 0 | grep -oP '"value": \K[0-9]+')
echo "loader pid=$(pgrep -x MY_ivh_atc) cap_source=$G0 dest_margin_pct=$G1 eligible=$(cat $S/ivh_universal_eligible)"
[ "$G0" = 3 ] && [ "$G1" = "$PCT" ] || { echo "FATAL: cfg mismatch"; exit 1; }
