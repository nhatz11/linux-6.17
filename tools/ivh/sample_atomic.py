#!/usr/bin/env python3
"""Poll a global atomic_t at high rate. Resolves the address ONCE.

Usage: sample_atomic.py <symbol> <seconds> [interval_s]
"""
import struct, sys, time, collections, signal

# The harness kills this early (the workload finishes before `dur`), so results
# must be emitted on SIGTERM/SIGINT too -- otherwise a killed sampler prints
# nothing and the caller silently records zeros.
_STOP = {'now': False}
def _on_sig(signum, frame):
    _STOP['now'] = True
signal.signal(signal.SIGTERM, _on_sig)
signal.signal(signal.SIGINT, _on_sig)
name, dur = sys.argv[1], float(sys.argv[2])
iv = float(sys.argv[3]) if len(sys.argv) > 3 else 0.001
addr = None
for l in open('/proc/kallsyms'):
    p = l.split()
    if len(p) > 2 and p[2] == name:
        addr = int(p[0], 16); break
if not addr: sys.exit(f"{name} not in kallsyms")
f = open('/proc/kcore', 'rb')
e = f.read(64)
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
if off is None: sys.exit("not mapped in kcore")
# NOTE: /proc/kcore reads are served from the page cache if the fd stays open
# and is merely re-seeked -- a held-open fd reports a FROZEN value. Observed
# 2026-09-28: 119 samples of ivh_migrations_done all identical while
# migcount.py (which reopens) showed the counter advancing by 7,537. Reopen
# every sample. os.pread on a fresh fd is what actually sees live memory.
import os
c = collections.Counter(); end = time.time() + dur; n = 0
f.close()
while time.time() < end and not _STOP['now']:
    fd = os.open('/proc/kcore', os.O_RDONLY)
    try:
        c[struct.unpack('<i', os.pread(fd, 4, off))[0]] += 1
    finally:
        os.close(fd)
    n += 1
    if iv: time.sleep(iv)
print(f"  {name}: samples={n} max={max(c)} mean={sum(k*v for k,v in c.items())/n:.4f}")
print(f"  distribution: {dict(sorted(c.items()))}")
