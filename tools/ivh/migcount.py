#!/usr/bin/env python3
"""Read the global ivh_migrations_done atomic_t from /proc/kcore."""
import struct,sys,re
addr=None
for l in open('/proc/kallsyms'):
    p=l.split()
    if len(p)>2 and p[2]=='ivh_migrations_done': addr=int(p[0],16); break
if not addr or addr==0:
    sys.exit("ivh_migrations_done not in kallsyms (need CAP_SYSLOG / kptr_restrict=0)")
import os
f=open('/proc/kcore','rb')
# parse ELF program headers to map the virtual address to a file offset
f.seek(0); e=f.read(64)
assert e[:4]==b'\x7fELF'
phoff=struct.unpack('<Q',e[32:40])[0]; phentsize=struct.unpack('<H',e[54:56])[0]
phnum=struct.unpack('<H',e[56:58])[0]
off=None
for i in range(phnum):
    f.seek(phoff+i*phentsize); ph=f.read(phentsize)
    ptype=struct.unpack('<I',ph[0:4])[0]
    if ptype!=1: continue
    p_offset,p_vaddr,_,p_filesz=struct.unpack('<QQQQ',ph[8:40])
    if p_vaddr<=addr<p_vaddr+p_filesz: off=p_offset+(addr-p_vaddr); break
if off is None: sys.exit("address not mapped in kcore")
f.seek(off); print(struct.unpack('<i',f.read(4))[0])
