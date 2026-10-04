#!/bin/bash
# postboot.sh -- run this FIRST after rebooting into G-LOCK-55. One command.
#
# Does, in this order (the order matters):
#   1. assert the intended kernel booted, and that its NEW sysctls exist
#   2. record boot-defaults BEFORE anything writes a sysctl
#   3. goto_mode.sh -- calibration is boot-derived and does NOT persist
#   4. restore the pre-reboot sysctl state from the snapshot
#   5. re-run the behavioural fingerprint and diff it
#
# EXPECTED DIFFERENCES in step 5, which are NOT regressions:
#   - p11_arm.sh now defaults ivh_pv_tier1_halt_min=22000 (was 0). The as50
#     fingerprint row will show HEH/tier-1 behaving differently.
#   - the :3808 head-stamp clobber is fixed, so the G-LOCK-44 head stamp is
#     readable for the first time -- and halt_min!=0 is exactly what activates
#     it, so the two changes compound in the head path.
# Anything ELSE that moves is worth investigating.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
SNAP="${1:-/root/ivh_logs/state/pre-glock54_1004-072030}"
WANT=6.17.0-G-LOCK-55b-sysctl+
pass=0; fail=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }

echo "######## 1. kernel identity ########"
[ "$(uname -r)" = "$WANT" ] && ok "running $WANT" \
  || bad "running $(uname -r), wanted $WANT -- did grub fall back? check /boot/grub/grubenv"

for k in ivh_cs_gate2_reference ivh_cs_heh_reference ivh_pv_prev_check_mask; do
	if [ -r "$S/$k" ]; then ok "$k exists (= $(cat $S/$k))"
	else bad "$k MISSING -- this is not the G-LOCK-55 kernel"; fi
done
# the new knobs must boot at today's-behaviour defaults
[ "$(cat $S/ivh_cs_gate2_reference 2>/dev/null)" = 0 ] && ok "gate2_reference defaults 0 (last_cs)" || bad "gate2_reference not 0"
[ "$(cat $S/ivh_cs_heh_reference   2>/dev/null)" = 0 ] && ok "heh_reference   defaults 0 (last_cs)" || bad "heh_reference not 0"
[ "$(cat $S/ivh_pv_prev_check_mask 2>/dev/null)" = 255 ] && ok "prev_check_mask defaults 255" || bad "prev_check_mask not 255"

echo
echo "######## 2. boot defaults, BEFORE anything writes ########"
bash "$T/ivh_state.sh" boot-defaults 2>&1 | sed 's/^/  /'

echo
echo "######## 3. calibration (does not persist across reboot) ########"
if bash /root/linux-6.17/cvm_setup/goto_mode.sh ivh user >/dev/null 2>&1; then ok "goto_mode.sh ivh user"
else bad "goto_mode.sh failed -- steal estimator will read flat"; fi
pgrep -ax vcap    >/dev/null && ok "vcap capacity writer live" || bad "vcap NOT running"
pgrep -ax MY_ivh_atc >/dev/null && ok "BPF selector attached"  || bad "BPF selector NOT attached"

echo
echo "######## 4. restore the pre-reboot sysctl state ########"
if [ -d "$SNAP" ]; then
	bash "$T/ivh_state.sh" restore "$SNAP" 2>&1 | tail -6 | sed 's/^/  /'
else
	bad "snapshot $SNAP not found"
fi

echo
echo "######## 5. fingerprint diff (see EXPECTED DIFFERENCES in the header) ########"
# Capacity needs ~45-60s to settle after boot. Measured 2026-10-04: 356 at
# t+0 vs 462-490 settled. A canary inside that window makes the fingerprint
# differ for that reason alone.
echo "  waiting for capacity to settle..."
for i in $(seq 1 8); do
	sleep 10
	c=$(awk 'NR>2 && $1<8 {s+=$11; n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats)
	if [ "$c" -ge 430 ] && [ "$c" -le 530 ]; then ok "capacity settled at $c"; break; fi
	[ "$i" = 8 ] && bad "capacity $c after 80s, outside 430-530 -- fingerprint unreliable"
done
if [ -d "$SNAP" ]; then
	bash "$T/ivh_state.sh" verify "$SNAP" > /tmp/vfy.$$ 2>&1; rc=$?
	tail -14 /tmp/vfy.$$ | sed 's/^/  /'
	[ "$rc" = 0 ] && ok "fingerprint verify" \
	  || bad "fingerprint verify -- check EXPECTED DIFFERENCES before calling it a regression"
	rm -f /tmp/vfy.$$
fi

echo
echo "######## 5b. are the new knobs WRITABLE, not just readable? ########"
# G-LOCK-55 shipped ivh_cs_gate2_reference and ivh_cs_heh_reference with
# proc_doulongvec_minmax + SYSCTL_ZERO/SYSCTL_ONE. Those macros point into
# sysctl_vals[], an array of int; the handler dereferences them as
# unsigned long *, so the minimum read back was 0x100000000 and EVERY write
# returned EINVAL. A READ succeeds on such a knob, so checking the value alone
# passed it for three days. Always write-then-readback.
for k in ivh_cs_gate2_reference ivh_cs_heh_reference ivh_pv_prev_check_mask; do
	f=/proc/sys/kernel/$k
	[ -e "$f" ] || { bad "$k missing"; continue; }
	orig=$(cat "$f")
	case $k in ivh_pv_prev_check_mask) probe=63 ;; *) probe=1 ;; esac
	if echo "$probe" > "$f" 2>/dev/null && [ "$(cat "$f")" = "$probe" ]; then
		echo "$orig" > "$f" 2>/dev/null
		[ "$(cat "$f")" = "$orig" ] && ok "$k writable (set $probe, restored $orig)" \
		                           || bad "$k writable but could NOT be restored to $orig"
	else
		bad "$k REJECTS WRITES (tried $probe) -- knob is dead, reads are meaningless"
		echo "$orig" > "$f" 2>/dev/null
	fi
done

echo
echo "######## 6. vCPU PINNING -- does NOT survive a guest reboot ########"
# Measured 2026-10-04: after rebooting into G-LOCK-55 our TD came back
# IDENTITY-mapped (vcpu 9-15 -> host cores 9-15). Host cores 9-17 are NUMA
# node1, so the guest straddled node0/node1 -- the documented variance source
# (spin CV 17.7% -> 6.3% when corrected). The corunner KEPT its pinning because
# it was not rebooted, which made the capacity split look correct by accident.
# node0 = 0-8,36-44. Correct map: vcpu 0-8 -> 0-8, vcpu 9-15 -> 36-42.
HOSTSSH="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$IVH_HOST""
PINOUT=$(timeout 90 $HOSTSSH "echo "$IVH_HOST_PASS" | sudo -S bash -c '
TD=\$(virsh list --name 2>/dev/null | grep trust_domain)
bad=0
for i in 9 10 11 12 13 14 15; do
  want=\$(( i + 27 ))
  got=\$(virsh vcpupin \"\$TD\" 2>/dev/null | awk -v v=\$i \"\\\$1==v {print \\\$2}\")
  if [ \"\$got\" != \"\$want\" ]; then
    virsh vcpupin \"\$TD\" \$i \$want --live >/dev/null 2>&1 && echo \"REPINNED \$i: \$got -> \$want\" || { echo \"PINFAIL \$i\"; bad=1; }
  fi
done
[ \$bad = 0 ] && echo PINOK
'" 2>&1 | grep -E 'REPINNED|PINFAIL|PINOK')
echo "$PINOUT" | sed 's/^/    /'
if echo "$PINOUT" | grep -q PINFAIL; then bad "vCPU re-pin failed -- guest is NUMA-straddled, variance will be high"
elif echo "$PINOUT" | grep -q REPINNED; then ok "vCPU pinning was LOST and has been restored (re-settle capacity before measuring)"
else ok "vCPU pinning already correct (all 16 on node0)"; fi

echo
echo "######## 7. contention environment ########"
awk 'NR>2{c[$1]=$11} END{lo=0;nl=0;hi=0;nh=0;
  for(k in c){if(k+0<8){lo+=c[k];nl++}else{hi+=c[k];nh++}}
  printf "  corunner split: vCPU0-7=%.0f  vCPU8-15=%.0f  (want ~430-530 / ~1024)\n", lo/nl, hi/nh}' /proc/ivh_cpu_stats
echo "  base_slice_ns = $(cat /sys/kernel/debug/sched/base_slice_ns) (want 2800000 unless you are sweeping)"

echo
echo "######## RESULT: $pass pass, $fail fail ########"
[ "$fail" = 0 ] && echo "  clean -- safe to start measuring" \
  || echo "  investigate the FAILs above before trusting any number"
