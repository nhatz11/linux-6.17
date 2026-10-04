#!/bin/bash
# oddthresh.sh -- remove the REDUNDANT first-iteration publish using a sysctl.
#
# THE FINDING IT ACTS ON
# ivh_node_stamp_set() writes pn->head_ctl (offset 24) while the successor polls
# prev->state (offset 20). struct pv_node is 32 bytes, so those are 4 bytes apart
# in the SAME 64-byte cacheline: every publish invalidates the line the next
# waiter is spinning on. Measured on vips: publishing with ALL AS mechanisms
# DISABLED still costs +74.6% more spin than stock PV (3/3 reps), so the cost is
# the publish, not the halting.
#
# WHY AN ODD BUDGET FIXES IT
# The publish fires when (loop & mask) == 0, with loop counting DOWN from
# ivh_pv_spin_threshold. 32768 is a power of two, so iteration 1 always
# publishes -- and that publish is redundant, because pv_init_node()
# (qspinlock_paravirt.h:1522) stamped the node at enqueue microseconds earlier.
# A budget of 32767 moves the first publish 255 iterations in:
#     ebizzy (43 iters/entry):  1 -> 0 publishes per entry
#     vips   (138):             1 -> 0
#     hackbench (542): 3 -> 2   memtier (685): 3 -> 2   dbench (1668): 7 -> 6
# Short spinners stop publishing entirely; long spinners keep theirs. The budget
# itself changes by 0.003%, and short waiters do not need the republish anyway --
# vips waits ~3.6us, well inside a 50us staleness threshold.
#
# Same budget in BOTH arms and for every workload, so the comparison stays fair
# and the "one shared value per mechanism" constraint holds.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-6}"
BUDGETS="${BUDGETS:-32768 32767}"
OUT=/root/ivh_logs/oddthresh_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }

arm() {   # $1 = pv|as   $2 = spin budget
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
	else
		IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || return 1
		echo 0 > $S/ivh_universal_eligible
	fi
	echo "$2" > $S/ivh_pv_spin_threshold
	[ "$(cat $S/ivh_pv_spin_threshold)" = "$2" ] || { echo "  BUDGETFAIL"; return 1; }
	sleep 1
}

runvips() {
	local t0 t1
	t0=$(date +%s%N)
	( cd "$P/pkgs/apps/vips/run" && IM_CONCURRENCY=16 \
	  "$P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips" im_benchmark \
	  orion_18000x18000.v output.v >/dev/null 2>&1 )
	t1=$(date +%s%N)
	python3 -c "print(f'{($t1-$t0)/1e9:.3f}')"
}

runebizzy() {
	( cd /root && timeout 180 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 ) \
	  | grep -oP '^\K[0-9]+(?= records/s)' | head -1
}

exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "wl\tbudget\tarm\trep\tval\tnode\thead\tentries\tt2f\n" > "$OUT"
echo "### oddthresh budgets=[$BUDGETS] reps=$REPS -> $OUT"

for wl in vips ebizzy; do
	echo "########## $wl ##########"
	case "$wl" in
	vips)   runvips >/dev/null 2>&1 ;;
	ebizzy) ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
	esac
	for B in $BUDGETS; do
		echo "--- budget $B ---"
		for rep in $(seq 1 "$REPS"); do
			case $((rep % 2)) in 1) ORD="pv as" ;; 0) ORD="as pv" ;; esac
			for a in $ORD; do
				arm "$a" "$B" || { echo "  ARMFAIL $a"; continue; }
				sync
				[ "$wl" = ebizzy ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
				sleep 1
				b=($(snap))
				case "$wl" in
				vips)   v=$(runvips) ;;
				ebizzy) v=$(runebizzy) ;;
				esac
				f=($(snap))
				printf "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\n" \
					"$wl" "$B" "$a" "$rep" "${v:-NA}" \
					"$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))" \
					"$(( (${f[2]} - ${b[2]}) + (${f[3]} - ${b[3]}) ))" \
					"$(( ${f[4]} - ${b[4]} ))" "$(( ${f[5]} - ${b[5]} ))" >> "$OUT"
				echo "  B$B rep$rep $a ${v:-NA}"
			done
		done
		python3 "$T/oddthresh_report.py" "$OUT" "$wl" "$B" || true
	done
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
python3 "$T/oddthresh_report.py" "$OUT"
echo "ODDTHRESH_DONE $OUT"
