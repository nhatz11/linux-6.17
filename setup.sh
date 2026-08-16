#!/bin/bash
set -e
BASE=/home/nick/kernels/linux-6.17-rseqport
RUNNING=$(uname -r)
KO="/lib/modules/$RUNNING/extra/vsched_module.ko"

# cgroup setup
echo "+cpu" | tee /sys/fs/cgroup/cgroup.subtree_control
echo "+cpuset" | tee /sys/fs/cgroup/cgroup.subtree_control
if [ ! -d /sys/fs/cgroup/hi_prgroup ]; then mkdir /sys/fs/cgroup/hi_prgroup; fi
if [ ! -d /sys/fs/cgroup/lw_prgroup ]; then mkdir /sys/fs/cgroup/lw_prgroup; fi
echo "threaded" > /sys/fs/cgroup/lw_prgroup/cgroup.type
echo "threaded" > /sys/fs/cgroup/hi_prgroup/cgroup.type
echo 1 | tee /sys/fs/cgroup/lw_prgroup/cpu.idle
echo -20 | tee /sys/fs/cgroup/hi_prgroup/cpu.weight.nice

# module setup — load the pre-cached .ko for the running kernel
lsmod | grep -q vsched_module && rmmod vsched_module 2>/dev/null || true

if [ ! -f "$KO" ]; then
    echo "ERROR: no cached module for $RUNNING"
    echo "Run: sudo $BASE/install_module.sh  (must be done while booted into $RUNNING"
    echo "after building that kernel, so Module.symvers still matches)"
    exit 1
fi

insmod "$KO"

echo "setup done"
ls /proc/vcap_info /proc/vcapacity_write /proc/vlatency_write \
   /proc/vtopology_write /proc/vav_capacity_write /proc/vact_write
