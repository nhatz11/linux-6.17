#!/bin/bash
# G-LOCK-52 validation. Run AFTER g52_postboot.sh.
#
# Proves the new interfaces before any measurement depends on them. Every
# check here corresponds to a failure this project has already paid for.
set -u
S=/proc/sys/kernel
pass=0; fail=0
ck() { if eval "$2"; then echo "  PASS  $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); fi; }

echo "kernel: $(uname -r)"
echo
echo "=== 1. interfaces exist ==="
ck "/proc/ivh_cap_write present" '[ -e /proc/ivh_cap_write ]'
ck "/proc/ivh_cpu_stats present" '[ -e /proc/ivh_cpu_stats ]'
for k in ivh_cap_writer ivh_ucw_max_age_ns ivh_act_ema_alpha_q16 ivh_act_clamp_ns; do
    ck "sysctl $k" "[ -e $S/$k ]"
done

echo
echo "=== 2. read path is fresh, not cached (ivh_kcore_fd_must_reopen) ==="
s1=$(head -1 /proc/ivh_cpu_stats | grep -oP 'seq=\K[0-9]+')
s2=$(head -1 /proc/ivh_cpu_stats | grep -oP 'seq=\K[0-9]+')
ck "seq advances between opens ($s1 -> $s2)" "[ \"$s2\" -gt \"$s1\" ]"
# A held fd read twice must return EOF on the second read, never a repeat.
n=$(python3 - <<'PY'
f=open('/proc/ivh_cpu_stats','rb')
a=f.read(); b=f.read()      # second read, no seek
print(len(b))
f.close()
PY
)
ck "held fd second read returns EOF (got $n bytes)" "[ \"$n\" = 0 ]"

echo
echo "=== 2b. NO TRUNCATION (G-LOCK-52 fix) ==="
full=$(cat /proc/ivh_cpu_stats | wc -l)
awkn=$(awk 'NR>2' /proc/ivh_cpu_stats | wc -l)
ncpu=$(nproc)
ck "cat sees all CPUs ($((full-2)) of $ncpu)"  "[ $((full-2)) = $ncpu ]"
ck "awk sees all CPUs ($awkn of $ncpu) -- 1024B buffer" "[ $awkn = $ncpu ]"
hd=$(head -c 100 /proc/ivh_cpu_stats | wc -c)
ck "small read returns data, not EOF ($hd bytes)" "[ $hd = 100 ]"

echo
echo "=== 3. write path validation (all-or-nothing) ==="
ck "rejects out-of-range value 9999" '! (echo "9999;" > /proc/ivh_cap_write) 2>/dev/null'
ck "rejects zero"                    '! (echo "0;"    > /proc/ivh_cap_write) 2>/dev/null'
ck "rejects garbage"                 '! (echo "abc;"  > /proc/ivh_cap_write) 2>/dev/null'
ck "accepts a valid vector"          'python3 -c "open(\"/proc/ivh_cap_write\",\"w\").write(\";\".join([\"700\"]*16)+\";\")"'
w=$(awk 'NR>2 {s+=$13} END{print s+0}' /proc/ivh_cpu_stats)
ck "ucw_writes advanced (sum=$w)" "[ \"$w\" -gt 0 ]"

echo
echo "=== 4. writer=0 leaves the in-kernel estimator in charge ==="
echo 0 > $S/ivh_cap_writer
sleep 1
k=$(awk 'NR>2 {printf "%s ",$11}' /proc/ivh_cpu_stats)
echo "  kernel-computed capacity: $k"
ck "not the 700 we wrote (kernel still owns it)" "[ \"\$(echo $k | tr ' ' '\n' | grep -c '^700$')\" -lt 8 ]"

echo
echo "=== 5. staleness watchdog fails CLOSED ==="
echo 1 > $S/ivh_cap_writer
python3 -c 'open("/proc/ivh_cap_write","w").write(";".join(["700"]*16)+";")'
sleep 1
k=$(awk 'NR>2 {printf "%s ",$11}' /proc/ivh_cpu_stats)
echo "  after write, writer=1: $k"
ck "userspace value took effect" "[ \"\$(echo $k | tr ' ' '\n' | grep -c '^700$')\" -ge 8 ]"
age=$(cat $S/ivh_ucw_max_age_ns)
# vcap republishes every ~5.2s, so nothing can go stale while it runs. The
# first version of this check did not stop it and "failed" against vcap's
# own live values -- a broken test, not a broken watchdog.
echo "  stopping vcap, then waiting ${age}ns + margin; capacity must go to 1024..."
pkill -STOP -x vcap 2>/dev/null
sleep $(( age/1000000000 + 3 ))
k=$(awk 'NR>2 {printf "%s ",$11}' /proc/ivh_cpu_stats)
st=$(awk 'NR>2 {s+=$14} END{print s+0}' /proc/ivh_cpu_stats)
echo "  after staleness: $k"
ck "capacity expired to 1024 (fail-closed)" "[ \"\$(echo $k | tr ' ' '\n' | grep -c '^1024$')\" -ge 8 ]"
ck "ivh_ucw_stale_events rose (sum=$st)" "[ \"$st\" -gt 0 ]"
pkill -CONT -x vcap 2>/dev/null
echo 0 > $S/ivh_cap_writer

echo
echo "=== 6. Gate 2 EWMA has a writer now ==="
e=$(awk 'NR>2 {printf "%s ",$10}' /proc/ivh_cpu_stats)
echo "  ewma_act_ns: $e"
nz=$(echo $e | tr ' ' '\n' | grep -vc '^0$')
ck "ewma_act_ns non-zero on some CPUs ($nz)" "[ \"$nz\" -gt 0 ]"
ck "ivh_time_left_source accepts 2" "echo 2 > $S/ivh_time_left_source"
echo 1 > $S/ivh_time_left_source

echo
echo "=== 7. active-time write path (G-LOCK-52) ==="
ck "/proc/ivh_act_write present" '[ -e /proc/ivh_act_write ]'
ck "sysctl ivh_act_writer" "[ -e $S/ivh_act_writer ]"
echo 1 > $S/ivh_act_writer
python3 -c 'open("/proc/ivh_act_write","w").write(";".join(["5000000"]*16)+";")'
sleep 1
a=$(awk 'NR>2 {printf "%s ",$10}' /proc/ivh_cpu_stats)
ck "userspace active time took effect (5ms)" "[ \"\$(echo $a | tr ' ' '\n' | grep -c '^5000000$')\" -ge 8 ]"
echo "  stopping vcap; expiry must fall back to last_active, NOT stay frozen..."
pkill -STOP -x vcap 2>/dev/null
sleep $(( $(cat $S/ivh_ucw_max_age_ns)/1000000000 + 3 ))
a=$(awk 'NR>2 {printf "%s ",$10}' /proc/ivh_cpu_stats)
z=$(echo $a | tr ' ' '\n' | grep -c '^0$')
ck "stale ewma zeroed -> gate falls back ($z CPUs)" "[ \"$z\" -ge 8 ]"
pkill -CONT -x vcap 2>/dev/null
echo 0 > $S/ivh_act_writer

echo
echo "================================"
echo "  PASS=$pass  FAIL=$fail"
[ "$fail" = 0 ] || echo "  *** do not trust measurements until these pass ***"
