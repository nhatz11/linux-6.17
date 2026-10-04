#!/bin/bash
# IVH benchmark campaign: PV (stock) vs IVH (migration + adaptive spinning).
#
# Design rules (from tools/bpf/docs/ivh_sustained_load_drift_and_capacity_test_2026-09-15.md):
#   - never compare across boots; every comparison lives inside one ABBA block
#   - capacity-settled wait before each block, so load history does not bias an arm
#   - ABBA / BAAB alternating so drift and position cancel
#   - verify the arm actually took before every run; abort if the infra died
# Phases: screen (SCREEN_BLOCKS per workload) -> decide -> confirm (CONFIRM_BLOCKS more
# for candidates only). Resumable: finished (workload, block) pairs are skipped.
set -u
CAMP=${CAMP:-/root/ivh_tools/campaign}
OUT=${OUT:-$CAMP/run_$(date +%Y%m%d_%H%M%S)}
LIST=${LIST:-$CAMP/benchmarks.tsv}
SCREEN_BLOCKS=${SCREEN_BLOCKS:-3}
CONFIRM_BLOCKS=${CONFIRM_BLOCKS:-5}
MIN_FREE_GB=${MIN_FREE_GB:-10}
ONLY=${ONLY:-}          # regex filter
SETTLE=${SETTLE:-1}     # 0 = skip the capacity-settled wait (debug only)
S=/proc/sys/kernel

mkdir -p "$OUT"
CSV=$OUT/results.csv
LOG=$OUT/log
[ -f "$CSV" ] || echo "workload,phase,block,pos,mode,value,ts" > "$CSV"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
die() { log "FATAL: $*"; echo "$*" > "$OUT/ABORTED"; exit 1; }

# ---------------------------------------------------------------- arms
# Both arms keep the daemons running: stopping vcap_probe costs a ~130 s
# capacity-EMA reconvergence (goto_mode.sh), which would bias whichever arm
# followed it. Migration is switched off at the gate instead
# (ivh_universal_eligible), exactly as every earlier IVH A/B in this project.
set_mode() {
    case "$1" in
      pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1 ;;
      ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1 ;;
      *)   die "unknown mode $1" ;;
    esac
    local el am
    el=$(cat $S/ivh_universal_eligible); am=$(cat $S/ivh_adaptive_mode)
    case "$1" in
      pv)  [ "$el" = 0 ] && [ "$am" = 0 ] || die "pv arm did not take (eligible=$el mode=$am)" ;;
      ivh) [ "$el" = 1 ] && [ "$am" = 2 ] || die "ivh arm did not take (eligible=$el mode=$am)" ;;
    esac
}

health_check() {
    [ "$(pgrep -xc MY_ivh_atc)" = 1 ] || die "MY_ivh_atc is not running"
    [ "$(pgrep -xc vcap_probe)" = 1 ] || die "vcap_probe is not running"
    local free_gb; free_gb=$(df -BG --output=avail /root | tail -1 | tr -dc '0-9')
    [ "${free_gb:-0}" -ge "$MIN_FREE_GB" ] || die "low disk: ${free_gb}G free"
    # Hard failures abort. Plain WARNING lines are recorded but do not abort: the
    # "Voluntary context switch within RCU read-side critical section" warning at
    # tree_plugin.h:332 predates this campaign (it appears in the boot that produced
    # the 2026-09-15 half-contention results) and fires under load in whatever
    # process happens to be running.
    local hard; hard=$(dmesg 2>/dev/null | grep -ciE 'soft lockup|rcu[_ ]*sched.*stall|hung task|BUG:|kernel panic|Oops')
    [ "$hard" = "$DMESG_HARD0" ] || die "new HARD kernel error in dmesg (was $DMESG_HARD0, now $hard)"
    local w; w=$(dmesg 2>/dev/null | grep -ciE 'WARNING:')
    if [ "$w" != "$DMESG_WARN" ]; then
        log "  note: kernel WARNING count $DMESG_WARN -> $w (recorded, not fatal)"
        dmesg 2>/dev/null | grep -iE 'WARNING:' | tail -1 >> "$OUT/kernel_warnings.txt"
        DMESG_WARN=$w
    fi
}

capacity_line() {
    python3 - <<'PY' 2>/dev/null || echo "NA"
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r, statistics as st
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    v=[r.read_u64(f,ph,sym["runqueues"]+3824+o) for o in offs]
print(f"{round(st.mean(v[:8]))}/{round(st.mean(v[8:]))}")
PY
}

settle() {
    [ "$SETTLE" = 1 ] || { sleep 3; return 0; }
    QUIET=1 MIN_S=${MIN_S:-45} MAX_S=180 /root/ivh_tools/wait_capacity_settled.sh >/dev/null 2>&1
}

# ---------------------------------------------------------------- one run
run_one() {   # $1 workload $2 phase $3 block $4 pos $5 mode
    local name=$1 phase=$2 blk=$3 pos=$4 mode=$5 v
    set_mode "$mode"
    v=$(cd "$W_DIR" && timeout "$W_TO" bash -c "$W_CMD" 2>/dev/null | eval "$W_EXT" 2>/dev/null | head -1)
    [ -z "$v" ] && v=FAIL
    echo "$name,$phase,$blk,$pos,$mode,$v,$(date +%s)" >> "$CSV"
    log "  $name blk$blk pos$pos $mode = $v"
    [ "$v" = FAIL ] && return 1
    return 0
}

run_block() {  # $1 workload $2 phase $3 block
    local name=$1 phase=$2 blk=$3 order fails=0
    if [ $((blk % 2)) -eq 1 ]; then order="pv ivh ivh pv"; else order="ivh pv pv ivh"; fi
    health_check
    settle
    log "$name block $blk ($phase) order=[$order] capacity=$(capacity_line)"
    local pos=0
    for m in $order; do
        pos=$((pos+1))
        run_one "$name" "$phase" "$blk" "$pos" "$m" || fails=$((fails+1))
    done
    [ "$fails" -ge 3 ] && return 1
    return 0
}

done_blocks() { grep -c "^$1,[a-z]*,$2," "$CSV" 2>/dev/null; }

run_workload() {  # $1 name $2 phase $3 first_block $4 last_block
    local name=$1 phase=$2 b
    for b in $(seq "$3" "$4"); do
        [ "$(done_blocks "$name" "$b")" -ge 4 ] && { log "$name block $b already done, skip"; continue; }
        run_block "$name" "$phase" "$b" || { log "$name: too many failures in block $b, skipping workload"; echo "$name" >> "$OUT/skipped"; return 1; }
    done
    return 0
}

load_workload() {  # sets W_DIR W_TO W_CMD W_EXT W_DIRHL; returns 1 if not found
    local want=$1 line
    line=$(awk -F'\t' -v w="$want" 'NF && $1 !~ /^#/ && $1==w {print; exit}' "$LIST")
    [ -z "$line" ] && return 1
    W_DIR=$(cut -f2 <<<"$line"); W_TO=$(cut -f3 <<<"$line")
    W_CMD=$(cut -f4 <<<"$line"); W_EXT=$(cut -f5 <<<"$line"); W_DIRHL=$(cut -f6 <<<"$line")
    [ -d "$W_DIR" ] || return 1
    return 0
}

# ---------------------------------------------------------------- setup
DMESG_HARD0=$(dmesg 2>/dev/null | grep -ciE 'soft lockup|rcu[_ ]*sched.*stall|hung task|BUG:|kernel panic|Oops')
DMESG_WARN=$(dmesg 2>/dev/null | grep -ciE 'WARNING:')
dmesg -n 1 2>/dev/null
mkdir -p /dev/shm/fiobench /dev/shm/fsmark /root/dbench_test
pgrep -x netserver >/dev/null || (netserver -p 12865 >/dev/null 2>&1 &)
pgrep -x iperf3 >/dev/null || (setsid nohup iperf3 -s -1 --daemon >/dev/null 2>&1; setsid nohup iperf3 -s >/dev/null 2>&1 &)
sleep 2
trap 'log "campaign exiting; restoring IVH+AS"; set_mode ivh 2>/dev/null; echo "$(date)" > "$OUT/FINISHED_OR_KILLED"' EXIT

log "=== campaign start: kernel $(uname -r), $(awk -F'\t' 'NF && $1 !~ /^#/{n++} END{print n}' "$LIST") workloads, screen=$SCREEN_BLOCKS confirm=$CONFIRM_BLOCKS ==="
log "capacity 0-7/8-15 = $(capacity_line)  (half contention expected: low/1024)"

WORKLOADS=$(awk -F'\t' -v only="$ONLY" 'NF && $1 !~ /^#/ && (only=="" || $1 ~ only) {print $1}' "$LIST")

# ---------------------------------------------------------------- phase 1: screen
for name in $WORKLOADS; do
    load_workload "$name" || { log "SKIP $name (missing dir or entry)"; echo "$name" >> "$OUT/skipped"; continue; }
    run_workload "$name" screen 1 "$SCREEN_BLOCKS"
done
log "=== screen phase complete ==="

# ---------------------------------------------------------------- decide
python3 "$CAMP/analyze.py" "$CSV" "$LIST" > "$OUT/decisions.txt" 2>&1
cp "$OUT/decisions.txt" "$OUT/decisions_screen.txt"
CANDS=$(awk '/^CANDIDATE/{print $2}' "$OUT/decisions.txt")
log "=== candidates: $(echo "$CANDS" | tr '\n' ' ') ==="

# ---------------------------------------------------------------- phase 2: confirm
for name in $CANDS; do
    load_workload "$name" || continue
    run_workload "$name" confirm $((SCREEN_BLOCKS+1)) $((SCREEN_BLOCKS+CONFIRM_BLOCKS))
done
python3 "$CAMP/analyze.py" "$CSV" "$LIST" > "$OUT/decisions_final.txt" 2>&1
log "=== campaign complete -> $OUT ==="
echo done > "$OUT/DONE"
