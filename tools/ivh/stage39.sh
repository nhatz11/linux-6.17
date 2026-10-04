#!/bin/bash
# Wait for the G-LOCK-39 build, verify it, install it. Does NOT touch grub --
# that step is done separately so the menu entry can be checked first.
set -u
REPO=/root/kernels/linux-6.17-vanilla
LOG=/root/ivh_tools/build39b.log

while pgrep -f "make -j16 bzImage" > /dev/null; do sleep 15; done
echo "=== build finished ==="

ERR=$(grep -cE "error:" "$LOG")
if [ "$ERR" != "0" ]; then
    echo "*** $ERR errors in the build -- NOT installing ***" >&2
    grep -E "error:" "$LOG" | head; exit 1
fi
if ! grep -q "Kernel: arch/x86/boot/bzImage is ready" "$LOG"; then
    echo "*** bzImage never reported ready -- NOT installing ***" >&2; exit 1
fi
echo "errors=0, bzImage ready"

FREE=$(df --output=avail -k /boot | tail -1)
echo "/boot free: $((FREE/1024)) MB"
if [ "$FREE" -lt 75000 ]; then
    echo "*** under 75MB free -- aborting rather than risk a partial install ***" >&2
    exit 1
fi

cd "$REPO" || exit 1
echo "=== make modules_install ==="
make -j16 modules_install > /root/ivh_tools/install39.log 2>&1 || {
    echo "*** modules_install FAILED ***" >&2; tail -20 /root/ivh_tools/install39.log; exit 1; }
echo "=== make install ==="
make install >> /root/ivh_tools/install39.log 2>&1 || {
    echo "*** install FAILED ***" >&2; tail -20 /root/ivh_tools/install39.log; exit 1; }

echo "=== installed ==="
ls -la /boot/vmlinuz-*G-LOCK-39* /boot/initrd.img-*G-LOCK-39* 2>/dev/null
df -h /boot | tail -1
echo "STAGED-OK"
