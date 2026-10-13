#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""xbeinfo.py default.xbe

Prints an XBE's header: base and entry point (retail and debug keys), the
certificate (title ID, name, region, version), sections, the XDK libraries
it was linked with, and its kernel imports by ordinal (names: xboxkrnl.def)."""
import struct,sys,hashlib
d=open(sys.argv[1],'rb').read()
u32=lambda o:struct.unpack_from('<I',d,o)[0]
base=u32(0x104)
def va2off(va): return va-base
def cstr(va):
    o=va2off(va); return d[o:d.index(b'\0',o)].decode('latin1')
hdr_size,img_size,imghdr_size,ts=u32(0x108),u32(0x10c),u32(0x110),u32(0x114)
cert=u32(0x118); nsec=u32(0x11c); secva=u32(0x120)
entry=u32(0x128); tls=u32(0x12c); kthunk=u32(0x158)
nlib=u32(0x160); libva=u32(0x164)
# retail XOR keys
EK=0xA8FC57AB; KK=0x5B6D40B6; DK=0x94859D4B; KD=0xEFB1F152
print("base %08x imgsize %08x"%(base,img_size))
for name,ek,kk in (("retail",EK,KK),("debug",DK,KD)):
    print(name,"entry %08x thunk %08x"%(entry^ek,kthunk^kk))
co=va2off(cert)
title=d[co+0xc:co+0xc+80].decode('utf-16le').rstrip('\0')
print("titleid %08x name %r region %x version %x"%(u32(co+8),title,u32(co+0xa0),u32(co+0xac)))
print("sections:")
for i in range(nsec):
    o=va2off(secva)+i*0x38
    fl,va,vs,ra,rs,na=struct.unpack_from('<6I',d,o)
    print("  %-10s va %08x vsz %08x raw %08x rsz %08x flags %x"%(cstr(na),va,vs,ra,rs,fl))
print("libraries:")
for i in range(nlib):
    o=va2off(libva)+i*16
    n=d[o:o+8].rstrip(b'\0').decode(); ma,mi,bu,fl=struct.unpack_from('<4H',d,o+8)
    print("  %-10s %d.%d.%d"%(n,ma,mi,bu))
th=kthunk^KK; o=None
# thunk is inside a section; map va->raw
def va2raw(va):
    for i in range(nsec):
        so=va2off(secva)+i*0x38
        fl,sva,vs,ra,rs,na=struct.unpack_from('<6I',d,so)
        if sva<=va<sva+rs: return ra+va-sva
o=va2raw(th); imps=[]
while u32(o): imps.append(u32(o)&0x7fffffff); o+=4
print("kernel imports (%d):"%len(imps), imps)
