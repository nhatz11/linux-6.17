#!/bin/bash
# g52_arm.sh <pv|A|B|C|D>  -- arms for the vcap-vs-kernel A/B.
#
# Builds on p78_arm.sh's full stack (ladder arm 5) and then varies ONLY the
# three knobs under test, written LAST and asserted -- same discipline and
# for the same reason: spin_mode and the feature block overwrite unrelated
# sysctls, so an earlier write is silently reverted and every arm runs
# identically with no error.
#
#   arm  cap_writer  act_writer  time_left_source   what it isolates
#   A         0           0             1           shipped baseline
#   B         1           0             1           vcap CAPACITY only
#   C         0           0             2           in-kernel EWMA only
#   D         1           1             2           vcap capacity + vcap EWMA
#
# A->B is capacity, A->C is active time, A->D is both. Changing both at once
# without B and C would make an effect unattributable.
set -u
S=/proc/sys/kernel
ARM="$1"

if [ "$ARM" = pv ]; then bash /root/ivh_tools/pvbase.sh; exit $?; fi

bash /root/ivh_tools/p78_arm.sh tlt 4000000 >/dev/null || exit 1

case "$ARM" in
  A) cw=0; aw=0; tls=1 ;;
  B) cw=1; aw=0; tls=1 ;;
  C) cw=0; aw=0; tls=2 ;;
  D) cw=1; aw=1; tls=2 ;;
  *) echo "FATAL: unknown arm '$ARM'"; exit 1 ;;
esac

# vcap must be publishing before we hand it the field, or the watchdog
# expires capacity to 1024 and the arm silently becomes "IVH off".
if [ "$cw" = 1 ] || [ "$aw" = 1 ]; then
    pgrep -x vcap >/dev/null || { echo "FATAL: arm $ARM needs vcap running"; exit 1; }
fi

echo "$cw"  > $S/ivh_cap_writer
echo "$aw"  > $S/ivh_act_writer
echo "$tls" > $S/ivh_time_left_source

# assert
g=$(cat $S/ivh_cap_writer); h=$(cat $S/ivh_act_writer); i=$(cat $S/ivh_time_left_source)
[ "$g" = "$cw" ] && [ "$h" = "$aw" ] && [ "$i" = "$tls" ] || {
    echo "FATAL: arm $ARM did not take (cap=$g act=$h tls=$i)"; exit 1; }
echo "arm $ARM: cap_writer=$cw act_writer=$aw time_left_source=$tls"
