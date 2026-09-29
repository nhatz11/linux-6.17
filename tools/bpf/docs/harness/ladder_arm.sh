#!/bin/bash
# ladder_arm.sh <arm> -- apply ONE ladder arm's sysctl set, using setarm()
# extracted VERBATIM from ladder.sh so the two cannot drift.
set -u
S=/proc/sys/kernel
source /tmp/setarm_body.sh
setarm "$1"
echo "ARM $1 applied and asserted"
