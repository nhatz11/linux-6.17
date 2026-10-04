#!/usr/bin/env python3
"""Read a global atomic_t / int kernel symbol out of /proc/kcore by name."""
import struct, sys
name = sys.argv[1]
addr = None
for l in open('/proc/kallsyms'):
    p = l.split()
    if len(p) > 2 and p[2] == name:
        addr = int(p[0], 16); break
if not addr:
    sys.exit(f"{name} not in kallsyms (need CAP_SYSLOG / kptr_restrict=0)")
f = open('/proc/kcore', 'rb')
e = f.read(64); assert e[:4] == b'\x7fELF'
phoff = struct.unpack('<Q', e[32:40])[0]
phentsize = struct.unpack('<H', e[54:56])[0]
phnum = struct.unpack('<H', e[56:58])[0]
off = None
for i in range(phnum):
    f.seek(phoff + i*phentsize); ph = f.read(phentsize)
    if struct.unpack('<I', ph[0:4])[0] != 1: continue
    p_offset, p_vaddr, _, p_filesz = struct.unpack('<QQQQ', ph[8:40])
    if p_vaddr <= addr < p_vaddr + p_filesz:
        off = p_offset + (addr - p_vaddr); break
if off is None: sys.exit("address not mapped in kcore")
f.seek(off); print(struct.unpack('<i', f.read(4))[0])
