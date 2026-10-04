# PLE-on-CVM experiment — self-contained runbook

Goal: show that Pause Loop Exiting (PLE) suppresses guest spinning on a legacy
VM, but **cannot** on a confidential VM, because the hypervisor loses the
information it needs to pick a yield target.

This file is written to be picked up cold, on a new machine, by a person or an
agent with no memory of the conversation that produced it.

---

## 1. The claim, and why it is true (verify before building anything)

PLE causes a VMEXIT when a guest spins in a PAUSE loop. The host then runs
`kvm_vcpu_on_spin()` and tries to yield to *another vCPU that is in kernel
mode*, on the theory that it is the one holding the contended lock.

On AMD, the PLE handler is `pause_interception()` in
`arch/x86/kvm/svm/svm.c`, and it contains this:

```c
static int pause_interception(struct kvm_vcpu *vcpu)
{
	bool in_kernel;
	/*
	 * CPL is not made available for an SEV-ES guest, therefore
	 * vcpu->arch.preempted_in_kernel can never be true.  Just
	 * set in_kernel to false as well.
	 */
	in_kernel = !sev_es_guest(vcpu->kvm) && svm_get_cpl(vcpu) == 0;
```

`in_kernel` becomes `kvm_vcpu_on_spin()`'s `yield_to_kernel_mode`. The same
degeneration appears generically in `kvm_arch_vcpu_in_kernel()`
(`arch/x86/kvm/x86.c`), which returns `true` unconditionally when
`vcpu->arch.guest_state_protected` is set.

**So the exit still happens and still costs; only target SELECTION is lost.**
Predict accordingly: on the CVM expect PLE exits to be comparable or HIGHER,
while spin time does NOT fall. "PLE cannot yield" is the wrong phrasing.

### The configuration trap that would silently kill this experiment

`guest_state_protected` is set in exactly two places, both of which encrypt the
VMSA:
- `sev.c` after `SEV_CMD_LAUNCH_UPDATE_VMSA`  -> **SEV-ES**
- `sev.c` after `SEV_CMD_SNP_LAUNCH_UPDATE`   -> **SEV-SNP**

**Plain SEV encrypts memory but NOT the VMSA, so it does NOT set the flag.**
A plain-SEV guest reads its real CPL, gets a working directed yield, and will
behave exactly like the legacy VM. If you build plain SEV you will measure a
null result and wrongly conclude the hypothesis is false.

> The bench CVM MUST be **SEV-ES or SEV-SNP**. Verify in the guest with
> `dmesg | grep -i sev` (expect "Memory Encryption Features active: AMD SEV-ES"
> or SEV-SNP) and on the host with `sev_es_guest()` behaviour, not just
> `sev_enabled`.

---

## 2. Topology

Three guests on one AMD SEV-ES/SNP-capable host:

| VM | purpose | notes |
|---|---|---|
| `noise` | sysbench pressure generator | the ONLY source of host contention; oversubscribes the shared cores |
| `bench-vm` | legacy (non-confidential) VM | all workloads; stock distro kernel is fine |
| `bench-cvm` | SEV-ES/SNP CVM | same workloads, same vCPU count, same pinning |

`bench-vm` and `bench-cvm` must be as identical as possible: same vCPU count,
same memory, same distro/kernel, same workload builds. They differ only in
confidentiality.

**Pin all three to a known core set, and pin each guest within ONE NUMA node.**
A guest straddling two nodes inflated spin CV from 6.3% to 17.7% in the prior
project and flipped verdicts. Use `virsh vcpupin`, not `taskset`.
**Pinning does not survive a guest reboot — re-check it every boot.**

Oversubscribe deliberately: `noise` must contend for the SAME physical cores as
the bench guest, or there is nothing for PLE to mitigate and every arm reads
zero.

---

## 3. Run matrix

Each workload, on each bench guest, in both noise conditions:

```
    {bench-vm, bench-cvm} x {noise off, noise on} x {workloads} x N reps
```

N >= 3, more where CV is high. Interleave or alternate arm order so neither
condition is systematically second. Expected shape:

| | noise OFF | noise ON |
|---|---|---|
| bench-vm | low spin, few PLE exits | spin rises, PLE exits rise, **PLE suppresses spin** |
| bench-cvm | low spin, few PLE exits | spin rises, PLE exits rise, **spin NOT suppressed** |

The headline is the bottom-right cell versus the top-right cell.

---

## 4. Measurement

### PLE exits — measured on the HOST

The exit code is `SVM_EXIT_PAUSE = 0x077` (`arch/x86/include/uapi/asm/svm.h`).
There is no dedicated KVM stat for it, so count the tracepoint:

```sh
# per-VM PLE exit count over a run
perf stat -e kvm:kvm_exit -a -- sleep <dur>          # all exits
bpftrace -e 'tracepoint:kvm:kvm_exit /args->exit_reason == 0x77/ { @ple[args->vcpu_id] = count(); }'
```

Attribute to the right guest by filtering on the qemu PID/cgroup of that VM.
Also record TOTAL exits so PLE share is interpretable.

Host-side PLE tunables (module params, read-only at runtime, set at modprobe):
`pause_filter_count`, `pause_filter_thresh` in `kvm_amd`. Record both. A clean
extra arm is **PLE disabled** (`pause_filter_count=0`) to show what PLE is
worth on each guest type.

### Spin time — measured INSIDE each guest

These are stock kernels with no IVH counters, so do NOT look for
`ivh_slowpath_wait_ns`. Use one of:

1. `perf lock contention -ab -- sleep <dur>` (BPF-based; needs BTF in the guest)
2. bpftrace bracket on the qspinlock slowpath:
   ```
   kprobe:native_queued_spin_lock_slowpath    { @t[tid] = nsecs; }
   kretprobe:native_queued_spin_lock_slowpath / @t[tid] / {
       @spin_ns = sum(nsecs - @t[tid]); @spin_n = count(); delete(@t[tid]); }
   ```
3. `CONFIG_LOCK_EVENT_COUNTS` debugfs counters, if the guest kernel has them.

Use the SAME instrument in both guests or the comparison is meaningless.
Verify bpftrace/BTF actually works inside the CVM early — that is the most
likely blocker, and if it fails everything downstream has to change.

### Traps carried over from the prior project

- **An instrument can change what it measures.** A kprobe on a path taken ~48k/s
  moved a headline from +17.5% to +7.0%. Take headline numbers from
  UNINSTRUMENTED runs; use probes only for mechanism terms.
- **Ratio-of-means, never mean-of-ratios**, whenever the baseline spreads.
- **Record the contention level with every number.** Migration/PLE verdicts in
  this project have reversed sign purely with host load.
- **Report counters as zero/non-zero, not magnitudes**, on short canaries —
  fire counts drift 82-137% run to run.
- **Assert the arm, never trust that you set it.** Read every knob back.

---

## 5. What to record per run

```
guest(vm|cvm), noise(on|off), workload, rep,
performance metric, guest spin_ns, guest spin events,
host PLE exits, host total exits, host load / steal,
pause_filter_count, pause_filter_thresh, vCPU pinning, SEV mode
```

---

## 6. Workloads -- the 10-workload suite

Run ALL TEN on both bench guests. Do not narrow the suite: the VM-vs-CVM gap is
expected to vary with how much time a workload's vCPUs spend in USER mode (the
kernel-mode filter is the only thing the CVM loses), so the SPREAD across
workloads is itself evidence. A single workload cannot show that.

| workload | invocation | recorded |
|---|---|---|
| hackbench | `hackbench -T -g1 -f8 -l150000` | +76.3% |
| memtier | `memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram` | +26.21% |
| ebizzy | `ebizzy -S 15 -t 16 -m -s 4194304` | +104.3% ‡ |
| fsmark | `fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1` | +208.8% |
| dbench | `dbench -t 15 16 -D /root/dbench_test` | +14.66% ‡ |
| NHextend-csmin | `NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=1 NHextend-csmin -l -n 16` | +18-20% |
| parsec dedup | `./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16` | +86.86% |
| parsec vips | `./bin/parsecmgmt -a run -p vips -c gcc -i native -n 16` | +57.39% |
| parsec bodytrack | `./bin/parsecmgmt -a run -p bodytrack -c gcc -i native -n 16` | +14.39% |
| parsec canneal | `./bin/parsecmgmt -a run -p canneal -c gcc -i native -n 16` | +12.55% |

PARSEC rows run from `cwd /root/parsec-benchmark`.

### Per-workload traps -- every one of these cost real time to find

- **memtier** needs its server tuned FIRST:
  `memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15`. hashpower must
  be >=15 at `-t 16`. Stop it with `systemctl stop memcached` BEFORE `pkill`;
  `Restart=always` revives it otherwise.
- **ebizzy** needs a DISCARDED WARMUP after every arm switch. Without one,
  migrations drop ~3x and a +52% win reads as +0.57%. Its path is
  machine-specific (`/home/nick/Desktop/ebizzy` on the old box) -- rebuild it.
  Its recorded +104.3% was ALSO probe-inflated; the honest probe-off figure is
  nearer +25-50%.
- **fsmark** must use `-n 30000`. At the campaign's `-n 2000` it finishes in
  0.48s and measures startup. Also: its total spin RISES while per-acquisition
  spin falls, because the acquisition count explodes ~55x. Report both.
- **dbench** must drop `-F`: +14.66% (t=10.87) without it versus -1% with it.
  tmpfs is the LOSING direction (-16 to -20% across 6 variants).
- **NHextend-csmin** is the csmin + pre-acquire-stamp binary with AFL off. Its
  wait MUST come from the program's own `Total wait time` (the userspace AFL
  lock). The kernel qspinlock counter reads ~118 events of pure noise for it and
  once flipped its verdict from 0.17x to 113x.
- **dedup** needs the page cache dropped before EVERY run plus a discarded
  warmup (`tools/ivh/parsec_ab.sh` does both). Without it a 672MB ISO's cache
  warming produced a bogus 3.6x spread. Under UNIFORM contention its PV arm
  spans 15-231s and the row becomes unquotable -- check PV-arm CV before
  reporting, drop above ~40%.
- **vips** is BIMODAL in the PV arm with a ~3-in-10 disaster rate; verdicts
  reversed at n=4/6/8/10 within one sitting. It needs ~50 pairs, and the
  disaster rate and magnitude must be reported SEPARATELY.
- **canneal, swaptions, ferret** sit at or near the instrument's idle floor on
  lock rate. Expect small effects; treat them as do-no-harm controls and report
  absolute ms, not percentages.

`tools/ivh/campaign/benchmarks.tsv` is the invocation authority for anything not
listed here, but note it has NO PARSEC rows and its fsmark/dbench rows are the
ones corrected above.
