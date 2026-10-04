#!/bin/bash
# checkmask128.sh -- G-LOCK-54 mini-experiment: stamp AND check at 128 iters.
#
# ESCALATES. A new lock path gets a 2 s canary, then 10 s, then 30 s, each with
# a hard timeout -- G-LOCK-33 passed a 5.5 s run and then hung the VM twice, so
# "it survived once" is not evidence. Restores on every exit path.
#
# 128 iterations = mask 0x7f. Both floors were lowered 0xff -> 0x1f in
# G-LOCK-54 specifically to make this expressible; 0/1/3/7/15 are still
# rejected because mask==0 would run pv_wait_early()'s full body, two remote
# cacheline loads, on EVERY iteration across all 16 vCPUs.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools

[ "$(cat $S/ivh_pv_prev_check_mask 2>/dev/null)" ] || {
	echo "FATAL: ivh_pv_prev_check_mask missing -- not running G-LOCK-54?"; exit 1; }
echo "  kernel: $(uname -r)"
echo "  watchdog_thresh: $(cat /proc/sys/kernel/watchdog_thresh)  (keep at 10)"

OLD_C=$(cat $S/ivh_pv_prev_check_mask); OLD_P=$(cat $S/ivh_pv_beat_publish_mask)
restore() { echo "$OLD_C" > $S/ivh_pv_prev_check_mask 2>/dev/null
            echo "$OLD_P" > $S/ivh_pv_beat_publish_mask 2>/dev/null
            echo "  restored: check=$(cat $S/ivh_pv_prev_check_mask) publish=$(cat $S/ivh_pv_beat_publish_mask)"; }
trap restore EXIT

snap() { python3 "$T/read_ivh_counters.py" ivh_prev_check_fired ivh_slowpath_wait_events \
         ivh_beat_tier2_checked 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }

run_canary() {   # $1 = seconds, $2 = label
	local b f
	b=($(snap))
	timeout $(( $1 + 15 )) hackbench -T -g1 -f8 -l$(( $1 * 5000 )) >/dev/null 2>&1
	local rc=$?
	f=($(snap))
	local ent=$(( ${f[1]} - ${b[1]} ))
	printf "    %-10s rc=%-3s entries=%-9s prev_check_fired=%-12s per-entry=%s\n" \
		"$2" "$rc" "$ent" "$(( ${f[0]} - ${b[0]} ))" \
		"$(python3 -c "e=$ent; print(f'{(${f[0]}-${b[0]})/e:.2f}' if e else 'n/a')")"
	return $rc
}

for M in 255 127; do
	echo
	echo "### check=$M publish=$M  ($(( M + 1 )) iterations)"
	echo "$M" > $S/ivh_pv_beat_publish_mask 2>/dev/null
	echo "$M" > $S/ivh_pv_prev_check_mask  2>/dev/null
	GC=$(cat $S/ivh_pv_prev_check_mask); GP=$(cat $S/ivh_pv_beat_publish_mask)
	if [ "$GC" != "$M" ] || [ "$GP" != "$M" ]; then
		echo "  REFUSED: check=$GC publish=$GP (wanted $M) -- see dmesg for the pr_err"
		dmesg | tail -2 | sed 's/^/    /'
		continue
	fi
	echo "  applied: check=$GC publish=$GP"
	# tier 2 must be ON or prev_check_fired is the only thing that moves
	for sec in 2 10 30; do
		run_canary "$sec" "${sec}s" || { echo "  *** canary FAILED at ${sec}s ***"; break; }
	done
done
echo
echo "EXPECTED: prev_check_fired per-entry should roughly DOUBLE from 255 -> 127."
echo "If it does not move, the write was refused or the nesting is wrong."
