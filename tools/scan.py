#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""scan.py default.xbe     (needs Capstone: pip install capstone)

Disassembles the XBE's code sections and counts the instructions that touch
Xbox-only state: fs: segment accesses (the kernel's per-CPU block) and
memory-mapped device addresses / port I/O. This is how the loader's fs:
patches and device traps were scoped."""
import struct,sys,re,collections
from capstone import *
d=open(sys.argv[1],'rb').read()
u32=lambda o:struct.unpack_from('<I',d,o)[0]
secva=u32(0x120); base=0x10000
secs=[]
for i in range(u32(0x11c)):
    o=secva-base+i*0x38
    fl,va,vs,ra,rs,na=struct.unpack_from('<6I',d,o)
    nm=d[na-base:d.index(b'\0',na-base)].decode(); secs.append((nm,va,ra,rs,fl))
md=Cs(CS_ARCH_X86,CS_MODE_32)
fs=collections.Counter(); mmio=collections.Counter(); simd=collections.Counter(); n=0
for nm,va,ra,rs,fl in secs:
    if not fl&4: continue
    code=d[ra:ra+rs]; off=0
    while off<len(code):
        got=False
        for ins in md.disasm(code[off:],va+off):
            got=True; n+=1; off=ins.address-va+ins.size
            s=ins.op_str
            if 'fs:' in s: fs[(nm,ins.mnemonic+' '+s)]+=1
            for m in re.findall(r'0x(f[d-f][0-9a-f]{6})\b',s): mmio[(nm,m[:4])]+=1
            if ins.mnemonic.startswith(('in','out')) and ins.mnemonic in('in','out','insd','outsd'): mmio[(nm,'port '+ins.mnemonic)]+=1
        if not got: off+=1
print("instructions",n)
print("FS usage:"); [print(' ',k,v) for k,v in fs.most_common(30)]
print("MMIO/ports:"); [print(' ',k,v) for k,v in mmio.most_common(30)]
