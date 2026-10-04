#!/bin/bash
# point4.sh <workload-comm> <dir> <cmd> <seconds>  -- migratable-thread census
set -u
SC=/tmp/claude-0/-root-linux-6-17/38f54852-065f-4ed1-a226-12f02f7c3273/scratchpad
COMM="$1"; DIR="$2"; CMD="$3"; DUR="${4:-12}"
OUT="$SC/p4_$COMM.out"
# workload runs in the background; the probe samples a bounded WINDOW so long
# workloads do not sit under a very hot hook for their whole runtime.
# ATTACH FIRST, then start the workload: kprobe setup takes seconds and short
# workloads (fsmark 5.6s) otherwise finish before the probe is live.
timeout $((DUR+40)) bpftrace /root/ivh_tools/point4.bt "$COMM" > "$OUT" 2>&1 & BT=$!
for i in $(seq 1 60); do grep -q Attaching "$OUT" 2>/dev/null && break; sleep 0.5; done
( cd "$DIR" && eval "$CMD" ) >/dev/null 2>&1 & WL=$!
sleep "${WARMUP:-0}"
sleep "$DUR"
kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
kill $WL 2>/dev/null; pkill -x "$COMM" 2>/dev/null; wait $WL 2>/dev/null
python3 - "$OUT" "$COMM" <<'PY'
import sys,re
o=open(sys.argv[1]).read(); w=sys.argv[2]
def keys(m):
    return len(re.findall(rf'@{m}\[\d+\]:', o))
def val(m):
    x=re.search(rf'@{m}: (\d+)', o); return int(x.group(1)) if x else 0
seen,ok,gated,ev = keys('tid_seen'),keys('tid_ok'),keys('tid_gated'),keys('tid_eval')
print(f"\n=== {w} ===")
print(f"  rcu_guard={open('/proc/sys/kernel/ivh_rcu_guard').read().strip()}")
print(f"  threads seen taking locks        {seen:>6}")
print(f"  threads that passed ALL gates    {gated:>6}   (reached ivh_eval_cooldown_ok)")
print(f"  threads that evaluated migration {ev:>6}   (reached bpf_sched_pre_lock_migrate)")
print(f"  threads NEVER migratable         {seen-gated:>6}")
print(f"\n  acquisitions: {val('calls'):,}   passed gates: {val('gated_ok'):,}   evaluated: {val('evaluated'):,}")
print("\n  why NOT migratable (per acquisition):")
tot=0; rows=[]
for m in re.finditer(r'@why\[(.*?)\]: (\d+)', o):
    rows.append((m.group(1).strip('"'), int(m.group(2)))); tot+=int(m.group(2))
for k,v in sorted(rows, key=lambda x:-x[1]):
    print(f"    {k:34} {v:>12,}  {100*v/max(tot,1):5.1f}%")
print(f"  threads pinned to one cpu        {keys('tid_pinned'):>6}")
print(f"  acquisitions in an RCU reader    {val('in_rcu_any'):>12,}  ({100*val('in_rcu_any')/max(val('calls'),1):.1f}% of all)")
vis=dict(rows).get('0_PASSES_visible_gates',0)
go=val('gated_ok')
if vis:
    print(f"\n  passes everything bpftrace can see : {vis:>12,}")
    print(f"  actually reached the gated call    : {go:>12,}")
    print(f"  => failed on preemptible()/in_task(): {vis-go:>12,}  ({100*(vis-go)/vis:5.1f}% of visible-pass)")
PY
