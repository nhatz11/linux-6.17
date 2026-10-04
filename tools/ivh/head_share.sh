#!/bin/bash
# head_share.sh -- is ebizzy's spinning done by HEADS (unmeasured) rather than
# NODE waiters (measured)? If so, every ebizzy spin number today is blind to the
# majority of its spinning.
#
# ivh_head_spin_success_* does not exist: the head records only exhaustion
# (:3680) and Stage-B bails (:3677); the `goto gotlock` success paths record
# nothing. So head spin VOLUME is unmeasurable, but head TENURE COUNT
# (ivh_head_spin_enter, :3563) is, and so are node attempts. The ratio tells us
# how much of the spinning we cannot see.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_slowpath_wait_events ivh_head_spin_enter ivh_head_spin_attempts ivh_head_spin_bail_attempts ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_cs_head_bailed ivh_head_bypass_fired ivh_beat_tier2_fired"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1; echo 0 > $S/ivh_universal_eligible; fi; sleep 1; }
exec 9>/var/lock/ivh_clean_check.lock; flock -w 900 9 || exit 1
echo "arm wl node_att head_ten node_iters head_acct_att head_acct_iters csb hbf t2f entries" | tr ' ' '\t'
for wl in ebizzy hackbench; do
 for a in pv as; do
  arm "$a"
  ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 )
  sync; sleep 1; b=($(snap))
  case $wl in
   ebizzy)    ( cd /root && timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
   hackbench) timeout 180 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1 ;;
  esac
  f=($(snap))
  NA=$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))
  NI=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))
  HT=$(( ${f[5]}-${b[5]} )); HA=$(( (${f[6]}-${b[6]}) + (${f[7]}-${b[7]}) ))
  HI=$(( (${f[8]}-${b[8]}) + (${f[9]}-${b[9]}) )); EN=$(( ${f[4]}-${b[4]} ))
  printf "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n" "$a" "$wl" "$NA" "$HT" "$NI" "$HA" "$HI" \
    "$(( ${f[10]}-${b[10]} ))" "$(( ${f[11]}-${b[11]} ))" "$(( ${f[12]}-${b[12]} ))" "$EN"
 done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
echo HEADSHARE_DONE
