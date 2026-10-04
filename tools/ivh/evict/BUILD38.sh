#!/bin/bash
# G-LOCK-38 diagnostic build. Detached; survives SSH loss.
set -u; K=/root/kernels/linux-6.17-vanilla; L=/root/ivh_logs/build38_$(date +%m%d-%H%M%S).log
mkdir -p /root/ivh_logs; exec > "$L" 2>&1
echo "=== BUILD38 start $(date -Is) ==="
cd "$K" || exit 1
echo "--- make -j$(nproc) ---"
timeout -k 60 7200 make -j"$(nproc)" 2>&1 | tail -40 || { echo "MAKE FAILED"; exit 1; }
echo "--- modules_install ---"
timeout -k 60 3600 make modules_install 2>&1 | tail -5 || { echo "MODULES FAILED"; exit 1; }
echo "--- install ---"
timeout -k 60 1800 make install 2>&1 | tail -20 || { echo "INSTALL FAILED"; exit 1; }
echo "--- df /boot ---"; df -h /boot | tail -1
echo "--- update-grub ---"
timeout -k 30 600 update-grub 2>&1 | tail -6
echo "=== BUILD38 COMPLETE $(date -Is) ==="
ls -la /boot/vmlinuz-*G-LOCK-38* 2>/dev/null
