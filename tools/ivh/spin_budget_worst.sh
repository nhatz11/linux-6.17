#!/bin/bash
# spin_budget_worst.sh -- how long the spin budget takes to deplete, no preemption.
# v2 2026-10-02 after review; v1's "worst case" was a replica artifact (it read
# the predecessor's line every iteration; the node loop reads it 1-in-256).
set -u
T=/root/ivh_tools; BIN=$T/spin_budget_worst
MHZ=$(grep -oP 'tsc: Detected \K[0-9.]+' <(dmesg 2>/dev/null) | head -1); MHZ=${MHZ:-2200.0}
TH=$(cat /proc/sys/kernel/ivh_pv_spin_threshold)
[ -x "$BIN" ] || gcc -O2 -pthread -o "$BIN" "$BIN.c" || exit 1
echo "threshold=$TH  TSC=$MHZ MHz  CPU=family$(awk -F: '/cpu family/{print $2;exit}' /proc/cpuinfo)/model$(awk -F: '/^model\t/{print $2;exit}' /proc/cpuinfo)"
echo "live: preempt_src=$(cat /proc/sys/kernel/ivh_pv_preempt_src) beat_publish_mask=$(cat /proc/sys/kernel/ivh_pv_beat_publish_mask) evict_node_stamp=$(cat /proc/sys/kernel/ivh_pv_evict_node_stamp)"
echo
echo "== NODE loop: prev->state consulted 1-in-256 (PV_PREV_CHECK_MASK=0xff)."
echo "   Nothing stores into our node line during the spin, so this is flat:"
for w in 0 1 8; do $BIN -M node -m 0xff -w $w -i "$TH" -t "$MHZ" -r 400; done
echo "   (-w has almost no effect: the spinner only looks every 256th iteration)"
echo
echo "== HEAD loop: polls the lock word EVERY iteration (trylock_clear_pending)."
echo "   The lock word is written by every acquire/release/xchg_tail, so THIS is"
echo "   the cache-sensitive loop and the one a dirty-line sweep models:"
for w in 0 1 2 8; do $BIN -M head -w $w -i "$TH" -t "$MHZ" -r 400; done
echo
echo "NOTE absolute cross-core figures are NOT reproducible constants: vCPU-to-pCPU"
echo "placement is unobservable inside a TD, and two runs of the same binary differed"
echo "~15%. Report a range across runs, never a single remote-miss latency."
echo "NOTE PAUSE latency varies ~10x across x86 generations (~10cyc pre-Skylake,"
echo "~140 Skylake/Cascade Lake, lower again Ice Lake+). This is Emerald Rapids."
echo "NOTE a TD cannot observe PAUSE-loop exiting (PLE is a VMCS control the TDX"
echo "module owns), so 'no preemption' is unverifiable from inside the guest."
