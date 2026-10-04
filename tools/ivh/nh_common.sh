# sourced by the nh_* runners -- the arm switches.
# 2026-10-01: setivh() previously overrode ivh_cap_writer=0, ivh_act_writer=0 and
# ivh_time_left_source=1 AFTER p7v2_arm.sh had asserted 1/1/2. cap_writer=0 hands
# rq->ivh_uc_capacity back to the in-kernel ivh_uc_tick() estimator
# (kernel/sched/core.c:672), i.e. it silently reverts G-LOCK-51/52 and makes vcap
# a no-op for the gate. Use setivh for the shipped config; setivh_kcap only when
# the in-kernel estimator is deliberately the thing under test.
S=/proc/sys/kernel
setpv(){  bash /root/ivh_tools/p7v2_arm.sh pv >/dev/null; sleep ${SETTLE:-1}; }
setivh(){ bash /root/ivh_tools/p7v2_arm.sh ${1:-4000000} >/dev/null; sleep ${SETTLE:-1}; }
setivh_kcap(){ bash /root/ivh_tools/p7v2_arm.sh ${1:-4000000} >/dev/null
          echo 0 > $S/ivh_cap_writer; echo 0 > $S/ivh_act_writer
          echo 1 > $S/ivh_time_left_source; sleep ${SETTLE:-1}; }
g(){ echo "$1" | grep -oP "$2" | head -1; }
