#!/bin/bash
# PV vs adaptive-spinning on hackbench, 4 arms, interleaved.
#   A  PV      : migration OFF, spin_mode 1 (STOCK_PV)      <- baseline
#   B  PV+AS   : migration OFF, spin_mode 2 (IVH_PV)        <- adaptive spinning ALONE
#   C  IVH     : migration ON,  spin_mode 1                 <- migration alone
#   D  IVH+AS  : migration ON,  spin_mode 2                 <- the combined thing
set -u
ROUNDS=3
W="hackbench -T -g1 -f8 -l400000"
for a in A B C D; do > /tmp/hb_$a.txt; done

set_arm() {
    case "$1" in
      A) echo 0 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 1 >/dev/null ;;
      B) echo 0 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 2 >/dev/null ;;
      C) echo 1 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 1 >/dev/null ;;
      D) echo 1 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 2 >/dev/null ;;
    esac
    # assert the mode actually took -- tier 2 silently never fires if preempt_src==0
    local am ps ue
    am=$(cat /proc/sys/kernel/ivh_adaptive_mode)
    ps=$(cat /proc/sys/kernel/ivh_pv_preempt_src)
    ue=$(cat /proc/sys/kernel/ivh_universal_eligible)
    case "$1" in
      A|C) [ "$am" = 0 ] || { echo "FATAL arm $1: adaptive_mode=$am expected 0"; exit 1; } ;;
      B|D) { [ "$am" = 2 ] && [ "$ps" = 2 ]; } || { echo "FATAL arm $1: adaptive_mode=$am preempt_src=$ps expected 2/2"; exit 1; } ;;
    esac
    echo "    [arm $1: eligible=$ue adaptive_mode=$am preempt_src=$ps]"
}

for i in $(seq 1 $ROUNDS); do
  echo "=== round $i ==="
  for a in A B C D; do
    set_arm $a
    v=$($W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "  $a time=${v}s"
    echo "$v" >> /tmp/hb_$a.txt
  done
done

python3 - <<'PY'
def load(p): return [float(x) for x in open(p)]
A,B,C,D = (load(f'/tmp/hb_{a}.txt') for a in 'ABCD')
def m(x): return sum(x)/len(x)
mA,mB,mC,mD = m(A),m(B),m(C),m(D)
def imp(new, base):   # hackbench: lower time is better
    return (base-new)/base*100
print(f"\nA  PV      mean={mA:6.2f}s  range={min(A):.2f}-{max(A):.2f}")
print(f"B  PV+AS   mean={mB:6.2f}s  range={min(B):.2f}-{max(B):.2f}   vs PV: {imp(mB,mA):+.1f}%  <- adaptive spinning ALONE")
print(f"C  IVH     mean={mC:6.2f}s  range={min(C):.2f}-{max(C):.2f}   vs PV: {imp(mC,mA):+.1f}%")
print(f"D  IVH+AS  mean={mD:6.2f}s  range={min(D):.2f}-{max(D):.2f}   vs PV: {imp(mD,mA):+.1f}%  vs IVH: {imp(mD,mC):+.1f}%")
print("\nper-round AS-alone (B vs A):", [f"{imp(B[i],A[i]):+.1f}%" for i in range(len(A))])
print("per-round AS-on-IVH (D vs C):", [f"{imp(D[i],C[i]):+.1f}%" for i in range(len(A))])
PY
echo "=== done ==="
