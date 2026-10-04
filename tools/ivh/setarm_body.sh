setarm(){
  local A="$1" heh=0 t2=0 sk=0 hb=0 beat=11000000
  case "$A" in
    mig_t1)                 ;;
    mig_t1_heh)             heh=1 ;;
    mig_t1_heh_t2)          heh=1; t2=1 ;;
    mig_t1_heh_t2_sk)       heh=1; t2=1; sk=1 ;;
    mig_t1_heh_t2_sk_hb)    heh=1; t2=1; sk=1; hb=1 ;;
    *) echo "FATAL: unknown arm $A"; exit 1 ;;
  esac
  # tier2 AND head bypass both read ivh_pv_beat_threshold; at the shipped 5 ms
  # NEITHER fires. 1 ms is where bypass was validated (99.7% taken) and tier2
  # fires ~64k/run.
  if [ "$t2" = 1 ] || [ "$hb" = 1 ]; then beat=2200000; fi

  # spin_mode FIRST: it sets tier1_enable=1, tier2_enable=1 and forces
  # beat_threshold=11000000, so every feature write must follow it.
  /root/spin_mode 2 >/dev/null 2>&1

  # migration ON in every arm -- it is the baseline mechanism, not a variable
  echo 2 > $S/ivh_pv_preempt_src
  echo 2 > $S/ivh_preempt_event_source
  echo 4000000 > $S/ivh_time_left_threshold_ns
  echo 8 > $S/ivh_max_concurrent
  echo 1 > $S/ivh_universal_eligible
  # tier1 ON in every arm (it is the baseline's own mechanism)
  echo 1 > $S/ivh_pv_tier1_enable
  echo 32768 > $S/ivh_pv_spin_threshold

  echo "$beat" > $S/ivh_pv_beat_threshold
  echo "$t2"   > $S/ivh_pv_tier2_enable

  if [ "$heh" = 1 ]; then
    echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_cs_owner_enable
    echo 1 > $S/ivh_cs_owner_clear;   echo 0 > $S/ivh_cs_owner_fast
    echo 1 > $S/ivh_cs_scan;          echo 1 > $S/ivh_cs_criterion
    echo 1 > $S/ivh_cs_head_probe;    echo 1 > $S/ivh_cs_head_bail
  else
    echo 0 > $S/ivh_cs_head_bail;  echo 0 > $S/ivh_cs_head_probe
    echo 0 > $S/ivh_cs_criterion;  echo 0 > $S/ivh_cs_owner_enable
    echo 0 > $S/ivh_cs_scan
  fi

  if [ "$sk" = 1 ]; then
    echo 1 > $S/ivh_pv_evict_enable;    echo 1 > $S/ivh_pv_evict_node_stamp
    echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
    echo 2 > $S/ivh_pv_evict_hop_cap;   echo 4 > $S/ivh_pv_requeue_max
    echo 0 > $S/ivh_pv_skip_point;      echo 1100000 > $S/ivh_pv_evict_threshold
  else
    echo 0 > $S/ivh_pv_evict_enable
  fi

  if [ "$hb" = 1 ]; then
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
    echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
    echo 4 > $S/ivh_head_bypass_max
  else
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  fi

  echo 1 > $S/ivh_slowpath_wait_measure

  # assert EVERY factor, on AND off -- an off-assert is what catches a leak
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
  chk ivh_adaptive_mode 2
  chk ivh_universal_eligible 1
  chk ivh_preempt_event_source 2
  chk ivh_pv_tier1_enable 1
  chk ivh_pv_spin_threshold 32768
  chk ivh_pv_tier2_enable "$t2"
  chk ivh_cs_head_bail "$heh"
  chk ivh_cs_head_probe "$heh"
  chk ivh_pv_evict_enable "$sk"
  chk ivh_head_bypass_probe "$hb"
  chk ivh_pv_beat_threshold "$beat"
  chk ivh_slowpath_wait_measure 1
  [ "$sk" = 0 ] || chk ivh_pv_evict_node_stamp 1
}
