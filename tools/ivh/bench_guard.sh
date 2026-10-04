# bench_guard.sh -- source this at the top of any IVH throughput A/B.
#
# 2026-09-25: the G-LOCK-39 hrtimer sampler (ivh_tks_sampler_ns=200000) costs
# the arm that cannot mitigate lock-holder preemption ~48% on ebizzy and ~17%
# on perf sched pipe -- 11-15us of TDX #VE exit every 200us on every vCPU.
# It INFLATES every IVH-vs-baseline ratio by crippling the baseline. It exists
# to VALIDATE the steal estimator, not to be on during performance work.
# goto_mode.sh sets it to 200000, so anything that calls goto_mode.sh must
# override it afterwards. Capacity still gates fine at 0 (reads ~621 on the
# contended half, threshold is 1010) and migration still fires.
ivh_bench_guard() {
    local S=/proc/sys/kernel fail=0
    echo 0 > $S/ivh_tks_sampler_ns 2>/dev/null
    local s; s=$(cat $S/ivh_tks_sampler_ns 2>/dev/null)
    if [ "$s" != 0 ]; then
        echo "GUARD FAIL: ivh_tks_sampler_ns=$s, must be 0 for a throughput A/B" >&2
        fail=1
    fi
    [ "$(pgrep -xc MY_ivh_atc)" = 1 ] || { echo "GUARD FAIL: MY_ivh_atc not running" >&2; fail=1; }
    [ "$(pgrep -xc vcap)" = 1 ] || { echo "GUARD FAIL: vcap not running" >&2; fail=1; }
    local cap
    cap=$(python3 - <<'PY' 2>/dev/null
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r, statistics as st
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    v=[r.read_u64(f,ph,sym["runqueues"]+3824+o) for o in offs]
print(f"{round(st.mean(v[:8]))}/{round(st.mean(v[8:]))}")
PY
)
    echo "guard: sampler_ns=$s  capacity(0-7/8-15)=${cap:-NA}  kernel=$(uname -r)"
    [ "$fail" = 0 ] || { echo "GUARD FAILED -- refusing to run" >&2; exit 1; }
}
ivh_bench_guard
