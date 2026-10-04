#!/bin/bash
# corunner.sh -- set the corunner VM's CPU load and report the resulting guest
# capacity. The corunner runs `sysbench cpu --threads=N --time=0 run` inside
# bench-18c ("$IVH_CORUNNER_IP", reachable from the host). Thread count is the
# contention dial: 16 threads against our 16 vCPUs on 16 pinned host cores is
# 2:1; more threads is heavier.
#
# WHY: AS's perf cost tracks contention -- memtier measured +2.31% at cap~650 and
# -5.92% at cap~744 on identical config. Setting contention is therefore part of
# specifying the experiment, not tuning the result, and the cap_mean must be
# reported with every number.
set -u
H="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""
CR="timeout 20 sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 "$IVH_CORUNNER""
capmean(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

case "${1:-status}" in
set)
  N="${2:?usage: corunner.sh set <threads>}"
  # systemd-run, NOT "nohup ... &": a backgrounded process launched through
  # nested ssh dies when the session closes, which silently dropped contention to
  # zero (cap_mean 1023) twice. A transient unit survives.
  timeout 60 $H "$CR 'echo "$IVH_HOST_PASS" | sudo -S systemctl stop corunner-load 2>/dev/null; \
     echo "$IVH_HOST_PASS" | sudo -S systemd-run --unit=corunner-load --service-type=simple \
       /usr/bin/sysbench cpu --threads=$N --time=0 run 2>&1 | tail -1'" 2>&1 | tail -1
  echo "  set to $N threads; settling..."
  prev=0
  for i in $(seq 1 12); do
    sleep 20; cur=$(capmean)
    echo "    t+$((i*20))s cap_mean=$cur"
    if [ "$prev" -gt 0 ] && [ "$(python3 -c "print(1 if abs($cur-$prev)<=12 else 0)")" = 1 ]; then
      echo "  settled at cap_mean=$cur"; break; fi
    prev=$cur
  done
  ;;
off)
  timeout 40 $H "$CR 'echo "$IVH_HOST_PASS" | sudo -S systemctl stop corunner-load 2>/dev/null; sleep 1; pgrep -ac sysbench || echo 0'" 2>&1 | tail -1
  for i in 1 2 3; do sleep 15; echo "    cap_mean=$(capmean)"; done
  ;;
status)
  timeout 40 $H "echo \"  host load: \$(cut -d' ' -f1-3 /proc/loadavg)\"; $CR 'echo \"  corunner: \$(pgrep -a sysbench | head -1)\"; echo \"  corunner load: \$(cut -d\" \" -f1-3 /proc/loadavg)\"'" 2>&1 | tail -3
  echo "  guest cap_mean=$(capmean)  min/max=$(awk 'NR>2{print $11}' /proc/ivh_cpu_stats | sort -n | sed -n '1p;$p' | tr '\n' '/')"
  ;;
esac
