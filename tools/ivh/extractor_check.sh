#!/bin/bash
# Run each of the 14 once and confirm its extractor yields a number.
set -u
source /root/ivh_tools/suite14.sh
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
for e in "${SUITE14[@]}"; do
  IFS='|' read -r n d m c x <<< "$e"
  [ "$c" = "MEMTIER_CMD" ] && c="$MT"
  prep14 "$n" >/dev/null 2>&1 || { printf "  %-20s PREP FAILED\n" "$n"; continue; }
  s=$(date +%s.%N)
  if [ "$m" = "TIME" ]; then
    ( cd "$d" && eval "$c" ) >/dev/null 2>&1; v="(timed)"
  else
    v=$( ( cd "$d" && eval "$c" 2>&1 ) | eval "$x" | tail -1 )
  fi
  t=$(date +%s.%N)
  ok="OK"; [ "$m" = "TIME" ] || { case "$v" in ''|*[!0-9.]*) ok="!! EXTRACTOR FAILED";; esac; }
  python3 -c "print(f'  {\"$n\":22} {\"$m\":11} wall={$t-$s:6.1f}s  value={\"$v\"[:18]:>18}  $ok')"
done
