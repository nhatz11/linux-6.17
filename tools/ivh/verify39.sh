#!/bin/bash
# G-LOCK-39 acceptance test. Two criteria, both from the user:
#   (1) ACCURACY  -- kernel steal/active must track wall-clock ground truth
#   (2) DIFFERENTIAL -- contended vs uncontended vCPUs must still separate
# Run AFTER goto_mode.sh, on a boot of 6.17.0-G-LOCK-39-sampler+.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools

if ! uname -r | grep -q "G-LOCK-39"; then
  echo "*** not running G-LOCK-39 (uname -r = $(uname -r)) -- aborting ***" >&2; exit 1
fi
for k in ivh_tks_sampler_ns ivh_tks_duty_pct ivh_tks_on_ns; do
  [ -f "$S/$k" ] || { echo "*** $k missing -- wrong kernel? ***" >&2; exit 1; }
done

arm(){ # $1 sampler_ns  $2 duty  $3 phase_pct
  echo "$3" > $S/ivh_tks_phase_pct
  echo "$2" > $S/ivh_tks_duty_pct
  echo "$1" > $S/ivh_tks_sampler_ns || { echo "sampler_ns=$1 REJECTED" >&2; return 1; }
  printf "  armed: sampler_ns=%s duty_pct=%s phase_pct=%s deadband=%s\n" \
    "$(cat $S/ivh_tks_sampler_ns)" "$(cat $S/ivh_tks_duty_pct)" \
    "$(cat $S/ivh_tks_phase_pct)" "$(cat $S/ivh_tks_deadband_ns)"
}

echo "### step 1: smoke test -- does enabling the sampler survive 20s of load?"
arm 50000 5 0 || exit 1
echo "  sampler on; watching for 20s (a hang here is the thing we care about)"
timeout -k 5 30 hackbench -T -g4 -f4 -l2000 >/dev/null 2>&1
echo "  survived. tks samples advancing per-cpu:"
python3 $T/read_vact_rq.py ivh_tks_samples 2>/dev/null | head -1
sleep 2
python3 $T/read_vact_rq.py ivh_tks_samples 2>/dev/null | head -1
echo "  (second row must be ~100k/cpu higher at 20us/5% duty => ~2500/s)"

echo
echo "### step 2: accuracy + differential, all 16 vCPUs"
SECS=20 $T/validate_all.sh

echo
echo "### step 3: A/B against the tick-driven path, same box, same load"
echo "--- tick-driven (sampler off, phase_pct=100: the G-LOCK-38 behaviour) ---"
arm 0 100 100
SECS=20 CPUS="3 15" $T/validate_all.sh
echo "--- sampler (50us, 5% duty, phase_pct=0) ---"
arm 50000 5 0
SECS=20 CPUS="3 15" $T/validate_all.sh
