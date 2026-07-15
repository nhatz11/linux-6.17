#!/bin/bash
# Build and cache vsched_module.ko for any installed kernel version.
#
# Extracts the real symbol CRCs directly from the kernel's compressed vmlinuz
# image so the module vermagic and CRCs match exactly, regardless of what
# the source tree's Module.symvers currently says.
#
# Usage: sudo ./build_cached_module.sh [kernel-version]
#   kernel-version defaults to $(uname -r)
#
# Example: sudo ./build_cached_module.sh 6.17.0-rseqport34+
set -e

BASE=/home/nick/kernels/linux-6.17-rseqport
KO="$BASE/custom_modules/vsched_module.ko"
TARGET_VER="${1:-$(uname -r)}"
VMLINUZ="/boot/vmlinuz-${TARGET_VER}"
DEST="/lib/modules/$TARGET_VER/extra"

if [ ! -f "$VMLINUZ" ]; then
    echo "ERROR: $VMLINUZ not found"
    exit 1
fi

echo "Building vsched_module.ko for $TARGET_VER ..."
echo "  Extracting real symbol CRCs from $VMLINUZ ..."

# Decompress the kernel image to a temp ELF
TMPELF=$(mktemp /tmp/vmlinux-XXXXXX.elf)
trap "rm -f $TMPELF ${BASE}/Module.symvers.bak" EXIT
"$BASE/scripts/extract-vmlinux" "$VMLINUZ" > "$TMPELF" 2>/dev/null

# Patch Module.symvers with the CRCs actually in the running kernel
python3 << PYEOF
import struct, subprocess, sys

vmlinux = '$TMPELF'
symvers_path = '$BASE/Module.symvers'

out = subprocess.check_output(['readelf', '-S', '--wide', vmlinux], text=True)
secs = {}
for line in out.splitlines():
    parts = line.split()
    for i, p in enumerate(parts):
        if p in ('__ksymtab', '__kcrctab', '__ksymtab_strings'):
            try:
                secs[p] = {'addr': int(parts[i+2],16), 'offset': int(parts[i+3],16), 'size': int(parts[i+4],16)}
            except (ValueError, IndexError): pass

if len(secs) < 3:
    print("ERROR: could not find kernel symbol tables in vmlinux", file=sys.stderr)
    sys.exit(1)

with open(vmlinux, 'rb') as f:
    raw = {}
    for name, s in secs.items():
        f.seek(s['offset']); raw[name] = (f.read(s['size']), s['addr'])

ksymtab, ksymtab_addr = raw['__ksymtab']
kcrctab, kcrctab_addr = raw['__kcrctab']
strings, strings_addr  = raw['__ksymtab_strings']

kernel_crcs = {}
for i in range(len(ksymtab) // 12):
    _, name_off, _ = struct.unpack_from('<iii', ksymtab, i*12)
    name_abs = (ksymtab_addr + i*12 + 4) + name_off
    str_off = name_abs - strings_addr
    if 0 <= str_off < len(strings):
        sym = strings[str_off:str_off+64].split(b'\x00')[0].decode('ascii','replace')
        kernel_crcs[sym] = struct.unpack_from('<I', kcrctab, i*4)[0]

with open(symvers_path) as f:
    lines = f.readlines()

patched = 0
out_lines = []
for line in lines:
    parts = line.rstrip('\n').split('\t')
    if len(parts) >= 2:
        sym = parts[1]
        if sym in kernel_crcs:
            new_crc = f'0x{kernel_crcs[sym]:08x}'
            if parts[0] != new_crc:
                parts[0] = new_crc
                patched += 1
    out_lines.append('\t'.join(parts) + '\n')

with open(symvers_path, 'w') as f:
    f.writelines(out_lines)
print(f"  Patched {patched} CRC entries in Module.symvers")
PYEOF

RELEASE_FILE="$BASE/include/config/kernel.release"
UTS_FILE="$BASE/include/generated/utsrelease.h"
SYMVERS_FILE="$BASE/Module.symvers"

ORIG_RELEASE=$(cat "$RELEASE_FILE" 2>/dev/null || echo "")
ORIG_UTS=$(cat "$UTS_FILE" 2>/dev/null || echo "")
cp "$SYMVERS_FILE" "${SYMVERS_FILE}.bak"

echo "$TARGET_VER" > "$RELEASE_FILE"
printf '#define UTS_RELEASE "%s"\n' "$TARGET_VER" > "$UTS_FILE"

make -C "$BASE/custom_modules" clean 2>/dev/null || true
make -C "$BASE/custom_modules"

# Restore everything
cp "${SYMVERS_FILE}.bak" "$SYMVERS_FILE"
rm "${SYMVERS_FILE}.bak"
trap - EXIT
[ -n "$ORIG_RELEASE" ] && echo "$ORIG_RELEASE" > "$RELEASE_FILE"
[ -n "$ORIG_UTS" ]     && printf '%s\n' "$ORIG_UTS" > "$UTS_FILE"

mkdir -p "$DEST"
cp "$KO" "$DEST/vsched_module.ko"
depmod -a "$TARGET_VER"

echo "Cached: $DEST/vsched_module.ko"
