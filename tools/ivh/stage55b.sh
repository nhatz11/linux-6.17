#!/bin/bash
# Wait for the G-LOCK-55b build, verify it, install it. Does NOT touch grub --
# that is staged separately with grub-reboot so the menu entry can be checked,
# and the REBOOT ITSELF IS THE USER'S to issue.
set -u
REPO=/root/kernels/linux-6.17-vanilla
LOG=/root/ivh_tools/build55b.log
VER=55b-sysctl

while pgrep -f "make -j16 bzImage" > /dev/null; do sleep 10; done
echo "=== build finished ==="
ERR=$(grep -cE "error:" "$LOG")
if [ "$ERR" != "0" ]; then
	echo "*** $ERR errors -- NOT installing ***" >&2; grep -E "error:" "$LOG" | head; exit 1
fi
grep -q "Kernel: arch/x86/boot/bzImage is ready" "$LOG" || {
	echo "*** bzImage never reported ready -- NOT installing ***" >&2; exit 1; }
echo "errors=0, bzImage ready"

FREE=$(df --output=avail -k /boot | tail -1)
echo "/boot free: $((FREE/1024)) MB"
[ "$FREE" -lt 75000 ] && { echo "*** under 75MB free -- aborting ***" >&2; exit 1; }

cd "$REPO" || exit 1
echo "=== make modules_install ==="
make -j16 modules_install > /root/ivh_tools/install55b.log 2>&1 || {
	echo "*** modules_install FAILED ***" >&2; tail -20 /root/ivh_tools/install55b.log; exit 1; }
echo "=== make install ==="
make install >> /root/ivh_tools/install55b.log 2>&1 || {
	echo "*** install FAILED ***" >&2; tail -20 /root/ivh_tools/install55b.log; exit 1; }

echo "=== installed ==="
ls -la /boot/vmlinuz-*$VER* /boot/initrd.img-*$VER* 2>/dev/null
df -h /boot | tail -1
echo "STAGED-OK"
