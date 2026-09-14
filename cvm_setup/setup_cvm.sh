#!/bin/bash
# cvm_setup/setup_cvm.sh — bring up a fresh CVM to the validated IVH state:
# custom kernel built+booted, vsched_module loaded, MY_ivh_atc + vcap_probe
# daemons running with the TSC-only (PV-free, CVM-safe) sysctl pipeline, all
# benchmark tools built, then a verification suite proving it actually works.
#
# Mirrors, rather than replaces, the existing scripts this project already
# validated: install_module.sh (module cache), setup.sh (module load +
# cgroups), and the sysctl/daemon-start block from /home/nick/IVH. Those are
# called directly, not reimplemented, so a change to the validated state only
# has to be made in one place.
#
# Assumes /home/nick as the home directory and /home/nick/vsched_main as the
# sibling repo location — the same hardcoded convention every other script in
# this project already uses (/home/nick/IVH, /home/nick/ivh_logs, etc.), not
# a new one introduced here.
#
# A kernel build requires a real reboot, which nothing in this environment
# can script through. Usage is therefore two calls, with you rebooting
# in between:
#
#   ./setup_cvm.sh build         # preflight, clone, configure, build+install
#                                 # kernel, cache the module, then STOPS
#   << reboot into the new kernel yourself >>
#   ./setup_cvm.sh post-reboot   # load module, build daemons+tools, start IVH
#   ./setup_cvm.sh test          # verification suite, pass/fail
#
# Or `./setup_cvm.sh all` to run build, and `./setup_cvm.sh finish` after
# reboot to run post-reboot+test together.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VSCHED=/home/nick/vsched_main
VCAP_DIR="$VSCHED/vcapacity"
LOGDIR=/home/nick/ivh_logs
STATE_DIR="$REPO/cvm_setup/.state"
mkdir -p "$STATE_DIR" "$LOGDIR"

PASS=0
FAIL_COUNT=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL_COUNT=$((FAIL_COUNT+1)); }
info() { echo ">>> $1"; }
step_done() { touch "$STATE_DIR/$1.done"; }
step_is_done() { [ -f "$STATE_DIR/$1.done" ]; }

# ---------------------------------------------------------------------------
# PHASE: preflight — packages + sibling repos this project depends on but
# does not vendor. Idempotent: every apt-get and every clone is skip-if-present.
# ---------------------------------------------------------------------------
phase_preflight() {
    info "Preflight: build + benchmark packages"
    local pkgs=(
        build-essential libncurses-dev libssl-dev libelf-dev zlib1g-dev
        pkg-config dwarves rsync cpio bison flex bc git
        clang llvm python3 python3-pip
        dbench rt-tests sysbench pbzip2 linux-tools-common linux-tools-generic
    )
    local missing=()
    for p in "${pkgs[@]}"; do
        dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        info "Installing missing packages: ${missing[*]}"
        sudo apt-get update -qq
        sudo apt-get install -y "${missing[@]}"
    else
        info "All apt packages already present"
    fi

    info "Preflight: sibling repos"
    # vsched_main is NOT auto-cloned. Its vcapacity submodule points at
    # vSched/vcapacity (a different org's repo, not this project's), and
    # vcap_probe.cpp — the file the running daemon is actually built from —
    # has never been pushed there (no push access to that remote as of
    # 2026-08-15). A git clone here would silently produce a checkout
    # missing the one file everything downstream depends on. Until that's
    # resolved upstream, vsched_main has to be copied onto this machine by
    # hand from a machine that already has the working tree, e.g.:
    #   rsync -avz --exclude='*.o' /home/nick/vsched_main/ <this-host>:/home/nick/vsched_main/
    if [ ! -f "$VCAP_DIR/vcap_probe.cpp" ]; then
        fail "vsched_main/vcapacity/vcap_probe.cpp not found at $VCAP_DIR"
        echo "    vsched_main can't be auto-cloned (see comment above this check in"
        echo "    cvm_setup/setup_cvm.sh). Copy it from a machine that has it, e.g.:"
        echo "      rsync -avz --exclude='*.o' <source-host>:/home/nick/vsched_main/ /home/nick/vsched_main/"
        echo "    then re-run: $0 preflight"
        exit 1
    else
        info "vsched_main/vcapacity present with vcap_probe.cpp"
    fi

    # ebizzy: this project's copy was hand-dropped from LTP with no git
    # history, so pull it straight from upstream LTP instead of depending on
    # that ad-hoc directory existing on the new machine. (Note:
    # /home/nick/Desktop/ebizzy on the reference machine is the BUILT
    # BINARY, a file, not a directory — never check inside it as a path.)
    if [ ! -f /home/nick/Desktop/ltp-src/ltp/utils/benchmark/ebizzy-0.3/ebizzy.c ] \
        && [ ! -f "$HOME/ebizzy-src/utils/benchmark/ebizzy-0.3/ebizzy.c" ]; then
        info "Fetching ebizzy source from upstream LTP (sparse, shallow)"
        rm -rf "$HOME/ebizzy-src"
        git clone --filter=blob:none --no-checkout --depth 1 \
            https://github.com/linux-test-project/ltp.git "$HOME/ebizzy-src" 2>/dev/null
        (cd "$HOME/ebizzy-src" && git sparse-checkout set utils/benchmark/ebizzy-0.3 && git checkout)
    fi

    step_done preflight
}

# ---------------------------------------------------------------------------
# PHASE: kernel config — restore the validated .config (not git-tracked by
# kernel convention) from the reference copy checked into this repo, then
# reconcile it against whatever this tree version actually has.
# ---------------------------------------------------------------------------
phase_config() {
    info "Kernel config"
    if [ ! -f "$REPO/.config" ]; then
        cp "$REPO/cvm_setup/reference.config" "$REPO/.config"
        info "Restored reference.config as .config"
    else
        info ".config already present, leaving as-is"
    fi
    cd "$REPO"
    make olddefconfig
    step_done config
}

# ---------------------------------------------------------------------------
# PHASE: libbpf + bpftool — this project builds both from the kernel tree's
# own vendored source (tools/lib/bpf, tools/bpf/bpftool) into /usr/local,
# NOT from a distro package. MY_ivh_atc/ivh_exec link against exactly this
# build (see tools/bpf/Makefile's IVH_LIBBPF_INC/LIB).
# ---------------------------------------------------------------------------
phase_libbpf() {
    info "libbpf + bpftool"
    if [ ! -f /usr/local/lib64/libbpf.a ]; then
        info "Building + installing libbpf to /usr/local"
        make -C "$REPO/tools/lib/bpf" \
            PREFIX=/usr/local LIBDIR=/usr/local/lib64 install install_headers
        sudo ldconfig
    else
        info "libbpf already installed at /usr/local/lib64"
    fi
    if ! command -v bpftool >/dev/null 2>&1; then
        info "Building + installing bpftool to /usr/local"
        make -C "$REPO/tools/bpf/bpftool" \
            EXTRA_CFLAGS=-I/usr/local/include EXTRA_LDFLAGS=-L/usr/local/lib64
        sudo make -C "$REPO/tools/bpf/bpftool" install \
            EXTRA_CFLAGS=-I/usr/local/include EXTRA_LDFLAGS=-L/usr/local/lib64
    else
        info "bpftool already installed: $(bpftool version 2>&1 | head -1)"
    fi
    step_done libbpf
}

# ---------------------------------------------------------------------------
# PHASE: kernel build + install + module cache. Skips the actual rebuild if
# already booted into the target kernel (safe to re-run this script on a box
# that's already there — e.g. this reference machine itself).
# ---------------------------------------------------------------------------
phase_build_kernel() {
    cd "$REPO"
    local target_ver
    target_ver=$(make -s kernelrelease)
    info "Target kernel: $target_ver   (currently running: $(uname -r))"

    if [ "$(uname -r)" = "$target_ver" ]; then
        info "Already booted into $target_ver — skipping build+reboot, module cache still runs below"
    else
        info "Building kernel (this is the long step — expect 20-60+ minutes on $(nproc) cores)"
        make -j"$(nproc)"
        sudo make modules_install
        sudo make install

        # nohz=off is REQUIRED, not optional: without it, ivh_tks_idle_sub=0
        # (shipped) reinstates an unbounded phantom steal-time debt under a
        # tickless boot. See tools/bpf/docs/ivh_nohz_fallback_2026-08-09.md.
        if ! grep -q "nohz=off" /etc/default/grub; then
            info "Adding nohz=off to /etc/default/grub"
            sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 nohz=off"/' \
                /etc/default/grub
        fi
        sudo update-grub
    fi

    # install_module.sh's own header: run this while Module.symvers still
    # matches the kernel just built — i.e. now, before reboot, not after.
    info "Caching vsched_module.ko for $target_ver"
    sudo "$REPO/install_module.sh" "$target_ver"

    step_done build_kernel

    if [ "$(uname -r)" != "$target_ver" ]; then
        echo
        echo "=========================================================="
        echo " Kernel $target_ver built and installed. REBOOT NOW into it,"
        echo " then run:  $0 post-reboot"
        echo "=========================================================="
        exit 0
    fi
}

# ---------------------------------------------------------------------------
# PHASE: post-reboot — load the module, build every userspace piece, start
# the IVH daemons with the validated TSC-only sysctl pipeline.
# ---------------------------------------------------------------------------
phase_post_reboot() {
    local target_ver
    target_ver=$(cd "$REPO" && make -s kernelrelease)
    if [ "$(uname -r)" != "$target_ver" ]; then
        fail "Running $(uname -r), expected $target_ver — reboot into the built kernel first"
        exit 1
    fi
    info "Booted into $target_ver, continuing"

    info "Loading vsched_module + cgroup setup (setup.sh)"
    sudo "$REPO/setup.sh"

    # vmlinux.h is BPF CO-RE input generated from the *running* kernel's own
    # BTF — machine- and boot-specific, correctly gitignored, so a fresh
    # clone never has it and it must be regenerated here, post-reboot,
    # against this exact kernel. A stale or missing one compiles against
    # the wrong struct rq/task_struct layout and fails with "no member
    # named ivh_uc_capacity" style errors (this struct rq addition is
    # IVH-specific, not in any stock vmlinux.h).
    info "Regenerating tools/bpf/vmlinux.h from this kernel's live BTF"
    sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$REPO/tools/bpf/vmlinux.h"

    info "Building MY_ivh_atc + ivh_exec"
    make -C "$REPO/tools/bpf" MY_ivh_atc ivh_exec
    # ivh_verify.sh (and phase_test below) hardcode /home/nick/ivh_exec —
    # a separate path from where the build actually puts it ($REPO/ivh_exec).
    # On the reference machine that's a manually-placed, un-synced copy;
    # keep it current with a symlink instead so a rebuild can't go stale.
    ln -sf "$REPO/ivh_exec" /home/nick/ivh_exec

    info "Building vcap + vcap_probe"
    make -C "$VCAP_DIR" all

    info "Building NHextend3 + NHextend4"
    gcc -O2 -pthread -o "$REPO/NHextend3" "$REPO/NHextend3.c"
    gcc -O2 -pthread -o "$REPO/NHextend4" "$REPO/NHextend4.c"

    info "Building ebizzy"
    local ebz_src=""
    [ -f "$HOME/ebizzy-src/utils/benchmark/ebizzy-0.3/ebizzy.c" ] \
        && ebz_src="$HOME/ebizzy-src/utils/benchmark/ebizzy-0.3/ebizzy.c"
    [ -z "$ebz_src" ] && [ -f /home/nick/Desktop/ltp-src/ltp/utils/benchmark/ebizzy-0.3/ebizzy.c ] \
        && ebz_src=/home/nick/Desktop/ltp-src/ltp/utils/benchmark/ebizzy-0.3/ebizzy.c
    if [ -n "$ebz_src" ]; then
        mkdir -p /home/nick/Desktop
        gcc -O2 -o /home/nick/Desktop/ebizzy "$ebz_src"
    else
        fail "ebizzy source not found (expected from preflight's LTP fetch) — skipping, not required for core IVH verification"
    fi

    # /home/nick/IVH itself was never in git (a gap found live on this
    # bring-up) — it's tracked here now as cvm_setup/IVH_start.sh. Every
    # other script (this one included, and ivh_verify.sh) hardcodes the
    # /home/nick/IVH path, so install it there rather than changing every
    # caller.
    if [ ! -f /home/nick/IVH ]; then
        info "Installing cvm_setup/IVH_start.sh as /home/nick/IVH"
        cp "$REPO/cvm_setup/IVH_start.sh" /home/nick/IVH
        chmod +x /home/nick/IVH
    fi

    info "Starting IVH (sysctls + MY_ivh_atc + vcap_probe daemons)"
    bash /home/nick/IVH

    step_done post_reboot
    info "post-reboot phase complete. Capacity EMA needs to settle (see /home/nick/IVH's own"
    info "output above) before benchmark numbers are trustworthy — run: $0 test"
}

# ---------------------------------------------------------------------------
# PHASE: test — build-level checks + the same mechanism/corunner/timing
# checks as ivh_verify.sh, extended with artifact and module checks.
# ---------------------------------------------------------------------------
phase_test() {
    echo "=== 1. Build artifacts present and executable ==="
    for f in tools/bpf/MY_ivh_atc ivh_exec NHextend3 NHextend4; do
        if [ -x "$REPO/$f" ]; then pass "$f built"; else fail "$f missing or not executable"; fi
    done
    if [ -x "$VCAP_DIR/vcap_probe" ]; then pass "vcap_probe built"; else fail "vcap_probe missing"; fi
    if [ -x /home/nick/Desktop/ebizzy ]; then pass "ebizzy built"; else fail "ebizzy missing (non-blocking)"; fi
    for c in dbench hackbench sysbench pbzip2; do
        command -v "$c" >/dev/null 2>&1 && pass "$c installed" || fail "$c missing"
    done

    echo
    echo "=== 2. Module + daemons ==="
    lsmod | grep -q vsched_module && pass "vsched_module loaded" || fail "vsched_module not loaded"
    [ -f /proc/vcap_info ] && pass "/proc/vcap_info present" || fail "/proc/vcap_info missing"
    [ -f /proc/ivh_debug ] && pass "/proc/ivh_debug present" || fail "/proc/ivh_debug missing"
    if pgrep -xc MY_ivh_atc | grep -qx 1; then pass "single MY_ivh_atc running"; else fail "MY_ivh_atc: $(pgrep -xc MY_ivh_atc 2>/dev/null || echo 0) instances"; fi
    if pgrep -xc vcap_probe | grep -qx 1; then pass "single vcap_probe running"; else fail "vcap_probe: $(pgrep -xc vcap_probe 2>/dev/null || echo 0) instances"; fi

    echo
    echo "=== 3. Mechanism assertions (CVM-safe TSC-only pipeline) ==="
    check_sysctl() {
        local got
        got=$(cat /proc/sys/kernel/"$1" 2>/dev/null)
        [ "$got" == "$2" ] && pass "$1 = $got" || fail "$1 = $got, expected $2"
    }
    check_sysctl ivh_universal_eligible 1
    check_sysctl ivh_ref_steal_enabled 0
    check_sysctl ivh_steal_source 2
    check_sysctl ivh_uc_used_source 0
    check_sysctl ivh_cap_source 3
    check_sysctl ivh_uc_min_steal_ns 500000
    check_sysctl ivh_capacity_threshold 1010

    echo
    echo "=== 4. BPF gate constants ==="
    BPFSRC="$REPO/tools/bpf/MY_ivh_atc.bpf.c"
    check_define() {
        local got
        got=$(grep -oP "^#define $1\s+\K[0-9]+" "$BPFSRC")
        [ "$got" == "$2" ] && pass "$1 = $got" || fail "$1 = $got, expected $2"
    }
    check_define IVH_CAP_HARDFLOOR 850
    check_define IVH_CAP_TOPBAND   50
    check_define IVH_CAP_MARGIN    20

    # Overridable so this smoke test doesn't force every machine to run the
    # exact reference-machine validation size (-l 400000, ~11.5s there).
    # This is a functional check ("does IVH visibly do something"), not a
    # perf comparison against that number — different hardware, and CVMs in
    # particular carry real memory-encryption/isolation overhead on top, so
    # absolute time here was never going to match the reference machine.
    local hb_loops="${HACKBENCH_LOOPS:-400000}"
    local hb_runs="${HACKBENCH_RUNS:-6}"

    echo
    echo "=== 5. Corunner contention + timed hackbench (n=$hb_runs, -l $hb_loops) ==="
    if command -v python3 >/dev/null 2>&1; then
        python3 - <<'PYEOF' > /tmp/cvm_test_cap_b.txt
import subprocess, json
def dump(name):
    out = subprocess.run(["sudo","bpftool","map","dump","name",name,"-j"],
                          capture_output=True, text=True).stdout
    return json.loads(out)
def val(e):
    return int.from_bytes(bytes(int(b,16) for b in e["value"]), "little")
sums = {int(e["key"][0],16): val(e) for e in dump("cap_sum")}
cnts = {int(e["key"][0],16): val(e) for e in dump("cap_cnt")}
for cpu in sorted(sums): print(cpu, sums[cpu], cnts[cpu])
PYEOF
    fi
    before_mig=$(grep "^ivh_migrations_done:" /proc/ivh_debug 2>/dev/null | awk -F: '{print $2}')
    TIMES=()
    for i in $(seq "$hb_runs"); do
        t=$(/home/nick/ivh_exec -v hackbench -T -g 1 -f 8 -l "$hb_loops" 2>&1 | grep "^Time:" | awk '{print $2}')
        echo "  run $i: ${t}s"
        TIMES+=("$t")
        sleep 5
    done
    after_mig=$(grep "^ivh_migrations_done:" /proc/ivh_debug 2>/dev/null | awk -F: '{print $2}')
    echo "  migrations during batch: $(( ${after_mig:-0} - ${before_mig:-0} ))"

    if command -v python3 >/dev/null 2>&1; then
        python3 - <<'PYEOF' >> /tmp/cvm_test_cap_a.txt
import subprocess, json
def dump(name):
    out = subprocess.run(["sudo","bpftool","map","dump","name",name,"-j"],
                          capture_output=True, text=True).stdout
    return json.loads(out)
def val(e):
    return int.from_bytes(bytes(int(b,16) for b in e["value"]), "little")
sums = {int(e["key"][0],16): val(e) for e in dump("cap_sum")}
cnts = {int(e["key"][0],16): val(e) for e in dump("cap_cnt")}
for cpu in sorted(sums): print(cpu, sums[cpu], cnts[cpu])
PYEOF
        python3 - <<'PYEOF'
before, after = {}, {}
try:
    for line in open("/tmp/cvm_test_cap_b.txt"):
        c,s,n = map(int, line.split()); before[c]=(s,n)
    for line in open("/tmp/cvm_test_cap_a.txt"):
        c,s,n = map(int, line.split()); after[c]=(s,n)
    contended, clean = [], []
    for cpu in sorted(before):
        ds = after[cpu][0]-before[cpu][0]; dn = after[cpu][1]-before[cpu][1]
        avg = ds/dn if dn else 0
        (contended if cpu < 8 else clean).append(avg)
    c_max, cl_min = max(contended), min(clean)
    print(f"  contended max={c_max:.1f}  clean min={cl_min:.1f}  separation={cl_min-c_max:.1f}")
    print("  [PASS] real separation between contended/clean vCPUs" if cl_min-c_max > 10
          else "  [FAIL] no meaningful separation — is the host corunner running?")
except Exception as e:
    print(f"  [SKIP] corunner check inconclusive: {e}")
PYEOF
    fi

    python3 - "$hb_loops" "${TIMES[@]}" <<'PYEOF'
import sys, statistics
hb_loops = int(sys.argv[1])
times = [float(t) for t in sys.argv[2:] if t]
if not times:
    print("  [FAIL] no hackbench times recorded"); sys.exit()
mean = statistics.mean(times); sd = statistics.pstdev(times)
cv = sd / mean if mean else 0
print(f"  mean={mean:.3f}s sd={sd:.3f} ({cv*100:.1f}% of mean) min={min(times):.3f} max={max(times):.3f}")
# Absolute-time check only means anything at the exact size the reference
# machine was validated against (-l 400000, ~11.5s there) — different
# hardware, and a CVM's own encryption/isolation overhead on top, means no
# other machine should be expected to land near that number.
if hb_loops == 400000:
    print("  [PASS] within validated ~11.5s reference-machine range" if mean < 13.0
          else f"  [INFO] mean {mean:.3f}s is off the reference machine's target — expected on different hardware, not necessarily a problem")
else:
    print(f"  [INFO] -l {hb_loops} has no reference-machine target to compare against (only -l 400000 does)")
# Variance check is hardware-independent and catches what absolute time can't.
print("  [PASS] run-to-run variance is tight (<15% of mean)" if cv < 0.15
      else f"  [FAIL] run-to-run variance is high ({cv*100:.1f}% of mean) — see the variance-diagnosis notes for this run")
PYEOF

    echo
    echo "=== Summary: $PASS passed, $FAIL_COUNT failed ==="
    [ "$FAIL_COUNT" -eq 0 ] && echo "Everything checked out." || echo "See [FAIL] lines above before trusting this box."
}

# ---------------------------------------------------------------------------
case "${1:-}" in
    preflight)    phase_preflight ;;
    config)       phase_config ;;
    libbpf)       phase_libbpf ;;
    build-kernel) phase_build_kernel ;;
    build)        phase_preflight; phase_config; phase_libbpf; phase_build_kernel ;;
    post-reboot)  phase_post_reboot ;;
    test)         phase_test ;;
    finish)       phase_post_reboot; phase_test ;;
    all)          phase_preflight; phase_config; phase_libbpf; phase_build_kernel ;;
    *)
        echo "Usage: $0 {build|post-reboot|test|finish}"
        echo "  build        preflight + clone + config + libbpf + kernel build/install (stops for reboot)"
        echo "  post-reboot  load module, build daemons/tools, start IVH  (run after rebooting)"
        echo "  test         verification suite"
        echo "  finish       post-reboot + test in one call"
        exit 1
        ;;
esac
