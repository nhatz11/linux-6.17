#!/bin/bash
# Run FIRST after booting into G-LOCK-52-vcapact.
set -u
echo "kernel: $(uname -r)"
[ -e /proc/sys/kernel/ivh_rcu_guard ] || { echo "FATAL: ivh_rcu_guard missing -- wrong kernel?"; exit 1; }
echo "ivh_rcu_guard default: $(cat /proc/sys/kernel/ivh_rcu_guard)  (must be 1)"
# REMOVED 2026-10-01 (vcap_probe deleted -- obsolete since vcap measures its own demand, and it inflated every PV-relative number): cd /root/vcapacity && (pgrep -x vcap_probe >/dev/null || (nohup ./vcap_probe -p 200 -s 200 >/dev/null 2>&1 & sleep 1))
# --- G-LOCK-52: two daemons, two jobs, do not conflate them ---
# vcap_probe  -p 200 -s 200  SCHED_IDLE, 50% duty. CORE RETENTION only. It
#   holds physical cores against the host corunner; stopping it costs 41-60%
#   throughput even with IVH fully off. It computes nothing.
# vcap        -p 200 -s 5000 SCHED_FIFO bursts, 3.8% duty. MEASUREMENT only:
#   TSC-gap steal -> capacity and mean active burst. The original cadence.
#
# ivh_ucw_max_age_ns MUST exceed vcap's loop period or the watchdog expires
# capacity to 1024 between every publish and the arm silently becomes "IVH
# off". At -p 200 -s 5000 the loop is ~5.2s, so 16s is ~3x it. This knob is
# a function of vcap's launch flags and a mismatch is SILENT.
echo 16000000000 > /proc/sys/kernel/ivh_ucw_max_age_ns
cd /root/vcapacity && (pgrep -x vcap >/dev/null || (nohup ./vcap -p 200 -s 5000 >/root/ivh_logs/vcap_g52.log 2>&1 & sleep 2))
cd /root/linux-6.17/cvm_setup && (pgrep -x MY_ivh_atc >/dev/null || (nohup /root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc >/root/atc_g52.log 2>&1 & sleep 3))
bash /root/linux-6.17/cvm_setup/goto_mode.sh ivh-as kernel >/dev/null 2>&1
# CRITICAL (IVH_start.sh): ivh_cfg tells the BPF selector WHICH capacity field to
# read. If it disagrees with ivh_cap_source the program silently reads
# rq->cpu_capacity -- a flat 1024 on every CPU -- so no destination is ever
# better than the source and the selector returns -1 forever. Symptom is zero
# migrations with every gate passing. Cost us two wasted reboots on 2026-09-29.
CFG=$(cat /proc/sys/kernel/ivh_cap_source)
for i in $(seq 40); do bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break; sleep 0.25; done
bpftool map update name ivh_cfg key 0 0 0 0 value $CFG 0 0 0 \
  || echo "*** ERROR: ivh_cfg update FAILED -- IVH will make no migrations ***"
echo "ivh_cfg   : $(bpftool map lookup name ivh_cfg key 0 0 0 0 2>/dev/null | grep -oE '"value": [0-9]+' | grep -oE '[0-9]+') (must equal ivh_cap_source=$CFG)"
echo "vcap       : $(pgrep -x vcap >/dev/null && echo RUNNING || echo MISSING)  (capacity+active)"
echo "vcap       : $(pgrep -x vcap >/dev/null && echo RUNNING || echo MISSING)  (capacity+active)"
echo "ucw_max_age: $(cat /proc/sys/kernel/ivh_ucw_max_age_ns) ns  (must exceed vcap loop ~5.2e9)"
echo "MY_ivh_atc : $(pgrep -x MY_ivh_atc >/dev/null && echo RUNNING || echo MISSING)"
echo "selector   : $(bpftool link list 2>/dev/null | grep -c 'target_btf_id') tracing links"
echo "--- selector sanity: migrations must be NON-ZERO ---"
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > /proc/sys/kernel/ivh_universal_eligible
echo 2 > /proc/sys/kernel/ivh_pv_preempt_src; echo 2 > /proc/sys/kernel/ivh_preempt_event_source
m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null||echo 0)
/home/nick/Desktop/ebizzy -S 6 -t 16 -m -s 4194304 >/dev/null 2>&1
m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null||echo 0)
echo "  migrations in 6s = $((m1-m0))   $([ $((m1-m0)) -gt 100 ] && echo OK || echo '*** BROKEN -- do not trust results ***')"
echo "--- capacity settling ---"
QUIET=1 MIN_S=60 MAX_S=240 timeout 300 bash /root/ivh_tools/wait_capacity_settled.sh 2>&1 | tail -2
python3 /root/ivh_tools/read_vact_rq.py ivh_uc_capacity 2>/dev/null | sed 's/.*per-cpu=//'
