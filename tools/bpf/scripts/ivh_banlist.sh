#!/bin/bash
# ivh_banlist.sh — manage the IVH migration banlist (banlist_tgid BPF map)
#
# Tasks in the banlist are never selected as IVH migration candidates.
# The map is keyed by TGID (process group ID), so adding any PID in a
# process bans the entire process.
#
# USAGE
#   sudo bash ivh_banlist.sh add <pid|tgid>   # ban a process
#   sudo bash ivh_banlist.sh del <pid|tgid>   # unban a process
#   sudo bash ivh_banlist.sh list             # show current banlist
#
# The key is a little-endian u32.  bpftool expects hex bytes in LE order.

set -e

die() { echo "ERROR: $*" >&2; exit 1; }

map_name="banlist_tgid"

# Convert a decimal TGID to 4 little-endian hex bytes for bpftool
tgid_to_hex() {
    local tgid=$1
    printf '%02x %02x %02x %02x' \
        $(( tgid        & 0xff )) \
        $(( (tgid >> 8) & 0xff )) \
        $(( (tgid >>16) & 0xff )) \
        $(( (tgid >>24) & 0xff ))
}

# Resolve PID to TGID (cat /proc/<pid>/status | grep Tgid)
resolve_tgid() {
    local pid=$1
    local tgid
    tgid=$(awk '/^Tgid:/{print $2}' /proc/"$pid"/status 2>/dev/null) || die "cannot read /proc/$pid/status"
    echo "$tgid"
}

cmd=${1:-list}
pid=${2:-}

case "$cmd" in
add)
    [[ -n "$pid" ]] || die "usage: $0 add <pid>"
    tgid=$(resolve_tgid "$pid")
    comm=$(cat /proc/"$pid"/comm 2>/dev/null || echo "?")
    hex=$(tgid_to_hex "$tgid")
    sudo bpftool map update name "$map_name" key hex $hex value hex 01
    echo "banned tgid=$tgid ($comm)"
    ;;
del)
    [[ -n "$pid" ]] || die "usage: $0 del <pid>"
    tgid=$(resolve_tgid "$pid")
    comm=$(cat /proc/"$pid"/comm 2>/dev/null || echo "?")
    hex=$(tgid_to_hex "$tgid")
    sudo bpftool map delete name "$map_name" key hex $hex 2>/dev/null || echo "(not in banlist)"
    echo "unbanned tgid=$tgid ($comm)"
    ;;
list)
    echo "=== IVH banlist (tgid -> comm) ==="
    sudo bpftool map dump name "$map_name" 2>/dev/null | awk '
    /key:/ {
        split($0, a, "key: ")
        split(a[2], b, " ")
        # LE bytes to decimal
        tgid = strtonum("0x"b[4]) * 16777216 + strtonum("0x"b[3]) * 65536 + \
               strtonum("0x"b[2]) * 256 + strtonum("0x"b[1])
        comm = ""
        if ((getline line < ("/proc/" tgid "/comm")) > 0) comm = line
        close("/proc/" tgid "/comm")
        printf "  tgid=%-6d  %s\n", tgid, (comm ? comm : "(gone)")
    }'
    ;;
*)
    die "unknown command '$cmd'; use add|del|list"
    ;;
esac
