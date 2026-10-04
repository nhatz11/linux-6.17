#!/bin/bash
# ivh_state.sh -- capture, restore and VERIFY the full IVH sysctl state.
#
#   ivh_state.sh snapshot [name]   capture every ivh_* sysctl + a behavioural
#                                  fingerprint, into a dated directory
#   ivh_state.sh restore <dir>     replay those values, asserting each write
#   ivh_state.sh verify <dir>      re-measure the fingerprint and diff it
#   ivh_state.sh boot-defaults     record the kernel's own post-boot values
#                                  (run this FIRST after a reboot, before any
#                                   arm script touches anything)
#
# WHY A BEHAVIOURAL FINGERPRINT AND NOT JUST VALUES: several ivh_ sysctls are
# boot-DERIVED from tsc_khz and HZ at late_initcall (ivh_pv_beat_calibrate,
# kvm.c:1556, and ivh_cs_tick_period at kvm.c:2006), so the same written value
# can mean different things on a different boot. And the knobs that matter most
# are the ones whose EFFECT is invisible in their value -- ivh_cs_tick_period is
# dead at ivh_cs_criterion=1 but live at 0; ivh_pv_beat_threshold does nothing
# unless tier2/bypass are enabled. Replaying 94 numbers proves nothing on its
# own. The fingerprint runs a short canary in two known arms and records what
# the mechanisms actually DID.
#
# NOTE ON pvbase.sh: it zeroes the mechanism ENABLES but leaves
# ivh_pv_beat_threshold / ivh_pv_evict_threshold / ivh_cs_tick_period at
# whatever the previous arm wrote. "Stock PV" is therefore not a clean slate.
# This tool records them explicitly so that leak is visible rather than silent.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
BASE=/root/ivh_logs/state
EXTRA_KERNEL="watchdog_thresh nmi_watchdog sched_schedstats numa_balancing panic_on_oops"

die() { echo "FATAL: $*" >&2; exit 1; }

capture_values() {
	local out="$1"
	: > "$out"
	for f in $S/ivh_*; do
		[ -r "$f" ] || continue
		printf "%s\t%s\n" "$(basename "$f")" "$(tr -d '\n' < "$f")" >> "$out"
	done
	for k in $EXTRA_KERNEL; do
		[ -r "$S/$k" ] && printf "EXTRA:%s\t%s\n" "$k" "$(tr -d '\n' < "$S/$k")" >> "$out"
	done
}

capture_env() {
	local out="$1"
	{
		echo "kernel	$(uname -r)"
		echo "tsc_mhz	$(grep -oP 'tsc: Detected \K[0-9.]+' <(dmesg 2>/dev/null) | head -1)"
		echo "tsc_khz	$(grep -oP 'tsc: Refined TSC clocksource calibration: \K[0-9.]+' <(dmesg 2>/dev/null) | head -1)"
		echo "nproc	$(nproc)"
		echo "cmdline	$(tr -d '\n' < /proc/cmdline)"
		echo "bpf_atc	$(pgrep -xc MY_ivh_atc || echo 0)"
		echo "ivh_cfg0	$(bpftool map lookup name ivh_cfg key 0 0 0 0 2>/dev/null | grep -oP '"value": \K[0-9]+' || echo NA)"
		echo "ivh_cfg1	$(bpftool map lookup name ivh_cfg key 1 0 0 0 2>/dev/null | grep -oP '"value": \K[0-9]+' || echo NA)"
		echo "cap_mean	$(awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats)"
	} > "$out"
}

# One short canary per arm, recording what the mechanisms ACTUALLY did.
# hackbench -l20000 keeps it ~3s so this is cheap enough to run on every boot.
fingerprint() {
	local out="$1"
	local C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked ivh_cs_abstain_young ivh_cs_long_hold"
	printf "arm\tseconds\titers\tentries\tt2f\tcsb\tev\tabstain_young\tlong_hold\n" > "$out"
	local saved_mode saved_noise
	saved_mode=$(cat $S/ivh_adaptive_mode); saved_noise=$(cat $S/ivh_cs_noise_cycles)
	for a in pv as50; do
		if [ "$a" = pv ]; then
			bash "$T/pvbase.sh" >/dev/null 2>&1 || { echo "  fingerprint: pvbase failed" >&2; continue; }
		else
			IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || { echo "  fingerprint: p11_arm failed" >&2; continue; }
		fi
		sleep 1
		local b f t0 t1
		b=($(python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'))
		t0=$(date +%s%N)
		timeout 180 hackbench -T -g1 -f8 -l60000 >/dev/null 2>&1
		t1=$(date +%s%N)
		f=($(python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'))
		printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" \
			"$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")" \
			"$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( ${f[2]}-${b[2]} ))" \
			"$(( ${f[3]}-${b[3]} ))" "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" \
			"$(( ${f[6]}-${b[6]} ))" "$(( ${f[7]}-${b[7]} ))" >> "$out"
	done
	bash "$T/pvbase.sh" >/dev/null 2>&1
	echo "$saved_noise" > $S/ivh_cs_noise_cycles
}

case "${1:-}" in
snapshot)
	D="$BASE/${2:-snap}_$(date +%m%d-%H%M%S)"; mkdir -p "$D"
	echo "### snapshot -> $D"
	capture_values "$D/values.tsv"; echo "  values: $(wc -l < "$D/values.tsv") knobs"
	capture_env "$D/env.tsv";       echo "  env:    $(wc -l < "$D/env.tsv") facts"
	echo "  fingerprint (2 canary runs, ~10s)..."
	fingerprint "$D/fingerprint.tsv"
	cat "$D/fingerprint.tsv" | column -t
	echo "SNAPSHOT_OK $D"
	;;
boot-defaults)
	D="$BASE/bootdefaults_$(uname -r)"; mkdir -p "$D"
	capture_values "$D/values.tsv"; capture_env "$D/env.tsv"
	echo "### boot defaults recorded -> $D"
	echo "  NOTE: only valid if nothing has written a sysctl since boot."
	echo "  Boot-derived knobs to check against source defaults:"
	for k in ivh_pv_beat_threshold ivh_cs_tick_period ivh_cs_noise_cycles ivh_pv_spin_threshold; do
		printf "    %-28s %s\n" "$k" "$(cat $S/$k)"
	done
	;;
restore)
	D="${2:?usage: ivh_state.sh restore <dir>}"
	[ -f "$D/values.tsv" ] || die "no values.tsv in $D"
	fail=0; n=0
	# ivh_universal_eligible LAST: it is the sole migration gate and several
	# consumers latch state when it flips, so it must not be on while the rest
	# of the configuration is still half-applied.
	while IFS=$'\t' read -r k v; do
		case "$k" in EXTRA:*) continue ;; ivh_universal_eligible) continue ;; esac
		[ -w "$S/$k" ] || continue
		echo "$v" > "$S/$k" 2>/dev/null
		got=$(tr -d '\n' < "$S/$k")
		[ "$got" = "$v" ] || { echo "  MISMATCH $k: wrote '$v' read '$got'"; fail=$((fail+1)); }
		n=$((n+1))
	done < "$D/values.tsv"
	while IFS=$'\t' read -r k v; do
		case "$k" in EXTRA:*) kk=${k#EXTRA:}; [ -w "$S/$kk" ] && echo "$v" > "$S/$kk" 2>/dev/null ;; esac
	done < "$D/values.tsv"
	ue=$(awk -F'\t' '$1=="ivh_universal_eligible"{print $2}' "$D/values.tsv")
	[ -n "$ue" ] && { echo "$ue" > $S/ivh_universal_eligible; n=$((n+1)); }
	echo "### restored $n knobs, $fail mismatches"
	[ "$fail" -eq 0 ] || die "$fail knobs would not take the recorded value"
	echo "RESTORE_OK"
	;;
verify)
	D="${2:?usage: ivh_state.sh verify <dir>}"
	[ -f "$D/fingerprint.tsv" ] || die "no fingerprint.tsv in $D"
	echo "### value diff"
	TMP=$(mktemp -d); capture_values "$TMP/values.tsv"; capture_env "$TMP/env.tsv"
	if diff -q "$D/values.tsv" "$TMP/values.tsv" >/dev/null; then
		echo "  values: IDENTICAL"
	else
		echo "  values DIFFER:"
		join -t$'\t' <(sort "$D/values.tsv") <(sort "$TMP/values.tsv") 2>/dev/null |
			awk -F'\t' '$2!=$3 {printf "    %-32s was %-14s now %s\n", $1, $2, $3}'
	fi
	echo "### env diff"
	diff <(sort "$D/env.tsv") <(sort "$TMP/env.tsv") | grep -E '^[<>]' | sed 's/^/    /' || echo "  env: IDENTICAL"
	echo "### behavioural fingerprint (the part values cannot prove)"
	fingerprint "$TMP/fingerprint.tsv"
	python3 - "$D/fingerprint.tsv" "$TMP/fingerprint.tsv" <<'PY'
import sys
def load(p):
    L=[l.split('\t') for l in open(p).read().splitlines()]
    return {r[0]: dict(zip(L[0][1:], r[1:])) for r in L[1:]}
a,b=load(sys.argv[1]),load(sys.argv[2])
# TOLERANCES, calibrated 2026-10-03 against two back-to-back runs with NOTHING
# changed: fire counts moved 82-137% and canary seconds 55% purely from host
# contention drift. So magnitudes cannot be the test. What IS stable and is the
# thing we actually care about is WHETHER each mechanism fires at all:
#   pv arm   must fire ZERO     (a non-zero means leftover state contaminated it)
#   as arm   must fire NON-ZERO (a zero means the mechanism is dead)
# Magnitudes are reported for context with a deliberately wide 5x band, so only
# an order-of-magnitude shift flags.
MUST_BE_ZERO  = {'pv':   ['t2f','csb','ev','abstain_young','long_hold']}
MUST_BE_NONZERO = {'as50':['t2f','abstain_young']}   # csb/ev fire too rarely to require
bad=[]
print(f"    {'arm':>6s} {'field':<16s} {'recorded':>14s} {'now':>14s} {'delta':>9s}  note")
for arm in a:
    if arm not in b: bad.append(f"{arm} missing from the new fingerprint"); continue
    for k in a[arm]:
        try: x,y=float(a[arm][k]), float(b[arm][k])
        except Exception: continue
        d = 0.0 if x==0 and y==0 else (100*(y-x)/x if x else float('inf'))
        note=""
        if k in MUST_BE_ZERO.get(arm,[]) and y!=0:
            note="  <== MUST BE ZERO: pv arm is contaminated"; bad.append(f"{arm}.{k} nonzero")
        elif k in MUST_BE_NONZERO.get(arm,[]) and y==0:
            note="  <== MUST BE NONZERO: mechanism is DEAD"; bad.append(f"{arm}.{k} zero")
        elif k=='seconds' and x>0 and (y/x>1.6 or y/x<0.625):
            note="  (canary time shifted >1.6x -- host contention, not config)"
        elif x>0 and (y/x>5 or y/x<0.2):
            note="  <== order-of-magnitude shift"; bad.append(f"{arm}.{k} {x:.0f}->{y:.0f}")
        print(f"    {arm:>6s} {k:<16s} {x:14,.0f} {y:14,.0f} {d:+8.1f}%{note}")
print()
if bad:
    print("  VERIFY: FAILED -- " + "; ".join(bad))
    print("  (magnitude drift alone is expected; the checks above are zero/non-zero and 5x)")
else:
    print("  VERIFY: PASS -- every mechanism fires as recorded, no order-of-magnitude shift")
PY
	rm -rf "$TMP"
	;;
*)
	grep '^#' "$0" | sed 's/^# \{0,1\}//' | head -28
	;;
esac
