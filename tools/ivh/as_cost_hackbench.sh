#!/bin/bash
# as_cost_hackbench.sh -- the substitution hypothesis, tested where it matters.
#
# THE HYPOTHESIS (measured on vips, n=10, same 4 arms): migration and adaptive
# spinning are SUBSTITUTES, not complements. tier 1 fires 14.534% of slowpath
# entries with migration OFF and 0.017% with it ON -- an 855x collapse -- because
# migration gets the preempted predecessor running again before a waiter ever
# inspects prev->state. That is why migas-mig came out +6.8 ms (a RESOLVED COST)
# on vips: AS paid full price for a population migration had already removed.
#
# THE PREDICTION: AS wins where migration is weak or off. So `as` (migration OFF)
# should beat `pv`, while `migas` should NOT beat `mig`. hackbench is the right
# instrument -- unlike vips its waits are long enough for a 50us threshold to
# engage, and AS already has +38.80% spin / +6.02% perf recorded here with CIs
# nowhere near zero. vips could never test this: 1.3-2.6 us waits, detector
# correctly abstaining, and corr(SEC,spin)=+0.02 so its runtime is not lock-bound.
#
# Q1 "can AS's cost be lowered?" -- needs the cost SPLIT. mig+AS differs from
#    stock PV in migration AND adaptive spinning AND the preempt_src pedestal at
#    once, so no previous arm could attribute the overhead. Four arms give the
#    two clean contrasts:  (migas - as) = migration alone,  (migas - mig) = AS alone.
#
# Q2 "is the detector broken?" -- needs HOST ground truth. floor3 rep8 was a
#    50.5 ms disaster where tier 2 was CHECKED 6840 times (5x its normal rate)
#    and fired ONCE; rep11 was an equal disaster where it fired 1271 times
#    (22.15%); rep6 fired 0 times and was the BEST run at 3.32 ms. So in-guest
#    firing is uncorrelated with the outcome and cannot settle it. The host can:
#    $HOME/vcpuwait.sh sums run_ns/wait_ns/nr_switches over the TD's 16 vcpu
#    threads, and wait_ns is runqueue-wait = real preemption of our vCPUs.
#      high host wait + no fires  -> detector is BROKEN (missed real preemption)
#      flat host wait on disasters -> nothing to detect; vips's tail is not
#                                     preemption and AS cannot fix it
#
# NOTE ON THE METRIC: score on ivh_slowpath_wait_ns / ivh_slowpath_wait_events,
# the kernel's own measured slowpath residence per acquisition. Boot-cumulative it
# reads 10.66 us/entry, whereas node-only iterations read 1.2-3 us -- the iteration
# counters see only the node loop. Worse, node+head iterations x 26ns convert to
# 104.60% of total residence, which is impossible (26.2% of residence is HALT, so
# spin is at most 73.8%); the implied real cost is 18.35 ns/iter, so every absolute
# "spin ms" figure from the iteration metric is inflated ~42%. Ratios survive,
# absolutes do not. ivh_slowpath_wait_measure=1 in pvbase.sh:36 AND p11_arm.sh:98,
# so this counter is symmetric across every arm.
#
# Legacy note: score on NODE iterations only. ivh_head_spin_iters_sum
# counts only budget EXHAUSTIONS, so it is quantised to multiples of
# ivh_pv_spin_threshold (0.85 ms lumps) and it manufactured a fake "AS has a
# hard floor at 15.2 ms" result. Head is still recorded here, never scored.
#
# pvbase keeps ivh_cs_track_enabled=1 and ivh_pv_tier1_enable=1 deliberately, so
# CS stamping and tier 1 are in the BASELINE and are not costs AS adds.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-12}"
OUT=/root/ivh_logs/hbcost_$(date +%m%d-%H%M%S).tsv
HOSTCMD="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$IVH_HOST""
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_halt_events ivh_beat_tier1_fired ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_check_calls ivh_cs_fired ivh_cs_abstain_young ivh_cs_head_bailed ivh_evict_marked ivh_evict_requeued ivh_g2_eval"

snap()  { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
hsnap() { timeout 25 $HOSTCMD "echo "$IVH_HOST_PASS" | sudo -S \$HOME/vcpuwait.sh" 2>/dev/null | tail -1; }
capm()  { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

# --- the AS mechanism set, applied or zeroed as a unit ---
as_on() {
	echo 1 > $S/ivh_pv_tier2_enable
	echo 1 > $S/ivh_head_bypass_enable;  echo 1 > $S/ivh_head_bypass_probe
	echo 1 > $S/ivh_head_bypass_runs;    echo 0 > $S/ivh_head_bypass_hold
	echo 4 > $S/ivh_head_bypass_max
	echo 1 > $S/ivh_cs_owner_enable;     echo 1 > $S/ivh_cs_owner_clear
	echo 0 > $S/ivh_cs_owner_fast;       echo 1 > $S/ivh_cs_scan
	echo 1 > $S/ivh_cs_criterion;        echo 1 > $S/ivh_cs_head_probe
	echo 1 > $S/ivh_cs_head_bail;        echo 1 > $S/ivh_cs_owed_ticks
	echo 1 > $S/ivh_pv_evict_enable;     echo 1 > $S/ivh_pv_evict_node_stamp
	echo 1 > $S/ivh_pv_evict_lookahead;  echo 1 > $S/ivh_pv_requeue_nosteal
	echo 2 > $S/ivh_pv_evict_hop_cap;    echo 4 > $S/ivh_pv_requeue_max
	echo 0 > $S/ivh_pv_evict_cheap_now
	echo 110000 > $S/ivh_pv_beat_threshold     # 50us @ 2200MHz
	echo 110000 > $S/ivh_pv_evict_threshold
	echo 110000 > $S/ivh_cs_tick_period
	echo 110000 > $S/ivh_cs_noise_cycles       # the REAL head-early-halt gate
	echo 255    > $S/ivh_pv_beat_publish_mask  # kernel REFUSES <255 or non-2^n-1
}
as_off() {
	for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe \
	         ivh_head_bypass_runs ivh_cs_owner_enable ivh_cs_owner_clear \
	         ivh_cs_scan ivh_cs_criterion ivh_cs_head_probe ivh_cs_head_bail \
	         ivh_pv_evict_enable ivh_pv_evict_node_stamp ivh_pv_evict_lookahead \
	         ivh_pv_requeue_nosteal; do echo 0 > $S/$k; done
}
mig_on() {
	echo 2 > $S/ivh_preempt_event_source      # =2 or migration NEVER fires
	echo 1 > $S/ivh_universal_eligible;  echo 0 > $S/ivh_migrate_mechanism
	echo 0 > $S/ivh_rcu_guard                 # guard=1 blocks most migration
	echo 2500000 > $S/ivh_time_left_threshold_ns; echo 8 > $S/ivh_max_concurrent
	echo 1 > $S/ivh_selection_trylock;   echo 1010 > $S/ivh_capacity_threshold
	echo 1 > $S/ivh_cap_writer;          echo 1 > $S/ivh_act_writer
	echo 2 > $S/ivh_time_left_source;    echo 16000000000 > $S/ivh_ucw_max_age_ns
}

arm() {
	bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
	case "$1" in
	pv)  echo 32768 > $S/ivh_pv_spin_threshold
	     [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
	     [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1 ;;
	mig) /root/spin_mode 2 >/dev/null || return 1
	     as_off; mig_on; echo 2 > $S/ivh_pv_preempt_src
	     echo 32768 > $S/ivh_pv_spin_threshold
	     [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  MIGFAIL"; return 1; }
	     [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "  AS LEAKED"; return 1; } ;;
	# 'as' == the EXACT config that won on vips earlier in the session:
	# p11_arm.sh 50 + mask 255 + budget 32768 + migration OFF (vips_resolve.sh:40).
	# It is the reference arm, not a variant.
	as)  /root/spin_mode 2 >/dev/null || return 1
	     as_on; echo 0 > $S/ivh_universal_eligible; echo 2 > $S/ivh_pv_preempt_src
	     echo 32768 > $S/ivh_pv_spin_threshold
	     [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "  MIG LEAKED"; return 1; }
	     [ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
	     [ "$(cat $S/ivh_pv_beat_publish_mask)" = 255 ] || { echo "  MASKFAIL"; return 1; } ;;
	migas) /root/spin_mode 2 >/dev/null || return 1
	     as_on; mig_on; echo 2 > $S/ivh_pv_preempt_src
	     echo 32768 > $S/ivh_pv_spin_threshold
	     [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  MIGFAIL"; return 1; }
	     [ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
	     [ "$(cat $S/ivh_pv_beat_publish_mask)" = 255 ] || { echo "  MASKFAIL"; return 1; } ;;
	esac
	[ "$(cat $S/ivh_cs_track_enabled)" = 1 ] || { echo "  CSTRACK ASYMMETRY"; return 1; }
	# The spin BUDGET must be identical in every arm or the arms are not
	# comparable. It drifted to 32767 in the AS arms while PV ran 32768 -- the
	# value measured WORSE earlier (3.902 vs 2.805 us/entry) -- which crippled
	# every AS reading. 32768 is what the winning vips config used (p11_arm.sh:69).
	[ "$(cat $S/ivh_pv_spin_threshold)" = 32768 ] || { echo "  BUDGET ASYMMETRY: $(cat $S/ivh_pv_spin_threshold)"; return 1; }
	sleep 1
}

runbench() {
	# hackbench -T -g1 -f8 -l150000 per campaign/benchmarks.tsv (the config
	# authority -- 5 of 19 invocations were wrong when taken from other scripts).
	# Reports its own "Time: N" in seconds; lower is better, same convention as
	# vips, so the report's sign handling is unchanged.
	local out t
	out=$( cd /root && timeout 120 hackbench -T -g1 -f8 -l150000 2>&1 )
	t=$(printf '%s\n' "$out" | grep -oP '^Time:\s*\K[0-9.]+' | head -1)
	[ -n "$t" ] || { echo NA; return; }
	echo "$t"
}

exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
HB=($(hsnap)); [ "${HB[0]:-0}" -gt 0 ] || { echo "FATAL: host sampler returned nothing"; exit 1; }
printf "arm\trep\tsec\tnode\thead\tent\twaitns\thaltns\thalte\tt1f\tt2chk\tt2f\tcschk\tcsf\tcsyoung\tcsbail\tevmark\tevreq\tg2\thrun\thwait\thsw\tcap\n" > "$OUT"
ARMS="pv mig as migas"
echo "### as_cost_and_truth: arms = $ARMS, vips, reps=$REPS"
echo "### Q1 cost split: (migas-as)=migration alone, (migas-mig)=AS alone"
echo "### Q2 detector: host vcpu wait_ns vs in-guest fires, per rep"
echo "### scored on NODE iters only (head counter is exhaustion-quantised)"
echo "### data $OUT   cap=$(capm)   host sampler OK"
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
runbench >/dev/null 2>&1

for rep in $(seq 1 "$REPS"); do
	ORD=$(python3 -c "
a='''$ARMS'''.split(); k=($rep-1)%len(a); print(' '.join(a[k:]+a[:k]))")
	for a in $ORD; do
		arm "$a" || { echo "  ARMFAIL $a"; continue; }
		sync; sleep 1
		CM=$(capm); b=($(snap)); hb=($(hsnap))
		v=$(runbench)
		f=($(snap)); hf=($(hsnap))
		printf "%s\t%s\t%s" "$a" "$rep" "$v" >> "$OUT"
		printf "\t%d" "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" >> "$OUT"
		printf "\t%d" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" >> "$OUT"
		for i in 4 5 6 7 8 9 10 11 12 13 14 15 16 17; do
			printf "\t%d" "$(( ${f[$i]} - ${b[$i]} ))" >> "$OUT"
		done
		printf "\t%d\t%d\t%d\t%s\n" "$(( ${hf[0]:-0} - ${hb[0]:-0} ))" \
			"$(( ${hf[1]:-0} - ${hb[1]:-0} ))" "$(( ${hf[2]:-0} - ${hb[2]:-0} ))" "$CM" >> "$OUT"
		DW=$(( ${f[5]} - ${b[5]} )); DE=$(( ${f[4]} - ${b[4]} ))
		echo "  rep$rep $a ${v}s  wait/acq=$(( DW / (DE>0?DE:1) ))ns  ent=$DE  t2f=$(( ${f[10]}-${b[10]} ))  hwait_ms=$(( (${hf[1]:-0}-${hb[1]:-0})/1000000 ))"
	done
	[ $((rep % 4)) -eq 0 ] && python3 "$T/waitns_report.py" "$OUT"
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL ##########"
python3 "$T/waitns_report.py" "$OUT"
echo "HBCOST_DONE $OUT"
