#!/bin/bash
# Build vsched_module.ko and cache it for the currently booted kernel.
#
# Run this ONCE right after building+installing a new kernel, while still
# booted into the OLD kernel — Module.symvers in the source tree will match
# the kernel you just built, so the CRCs will be correct for it.
#
# Alternatively, boot the new kernel and run this script immediately (before
# rebuilding for a newer version).
set -e

BASE=/home/nick/kernels/linux-6.17-rseqport
RESOLVE_BTFIDS="$BASE/tools/bpf/resolve_btfids/resolve_btfids"
KO="$BASE/custom_modules/vsched_module.ko"

TARGET_VER="${1:-}"
if [ -z "$TARGET_VER" ]; then
    # Default: build for whatever kernel the source tree is currently set to.
    # Read it from the generated utsrelease.h (reflects last full kernel build).
    TARGET_VER=$(grep -o '"[^"]*"' "$BASE/include/generated/utsrelease.h" | tr -d '"')
fi

DEST="/lib/modules/$TARGET_VER/extra"

echo "Building vsched_module.ko for kernel $TARGET_VER ..."

[ -f "$RESOLVE_BTFIDS" ] || make -C "$BASE/tools/bpf/resolve_btfids"

RELEASE_FILE="$BASE/include/config/kernel.release"
UTS_FILE="$BASE/include/generated/utsrelease.h"
ORIG_RELEASE=$(cat "$RELEASE_FILE" 2>/dev/null || echo "")
ORIG_UTS=$(cat "$UTS_FILE" 2>/dev/null || echo "")

# Patch vermagic to the target version
echo "$TARGET_VER" > "$RELEASE_FILE"
printf '#define UTS_RELEASE "%s"\n' "$TARGET_VER" > "$UTS_FILE"

# Use the explicit M= external-module form rather than `make -C
# custom_modules`: the latter needs a wrapper Makefile inside
# custom_modules/ that only exists once something has generated it, and
# custom_modules/ is otherwise gitignored like any other build directory —
# a fresh clone won't have that wrapper. M= needs nothing pre-existing.
make -C "$BASE" M="$BASE/custom_modules" clean 2>/dev/null || true
make -C "$BASE" M="$BASE/custom_modules" modules

# Restore
[ -n "$ORIG_RELEASE" ] && echo "$ORIG_RELEASE" > "$RELEASE_FILE"
[ -n "$ORIG_UTS" ]     && printf '%s\n' "$ORIG_UTS" > "$UTS_FILE"

mkdir -p "$DEST"
cp "$KO" "$DEST/vsched_module.ko"
depmod -a "$TARGET_VER"

echo "Cached: $DEST/vsched_module.ko"
