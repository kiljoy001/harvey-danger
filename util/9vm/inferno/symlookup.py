#!/usr/bin/env python3
"""Look up kernel symbols for addresses using ipc-sym.elf."""
import struct, sys

import os
path = os.environ.get("SYMELF",
    os.path.join(os.environ.get("NINEVM_WORK", "/tmp/9vm-inferno"), "ipc-sym.elf"))
d = open(path, "rb").read()
# parse ELF header
e_shoff = struct.unpack_from("<I", d, 0x20)[0]
e_shentsize = struct.unpack_from("<H", d, 0x2e)[0]
e_shnum = struct.unpack_from("<H", d, 0x30)[0]
sections = []
for i in range(e_shnum):
    off = e_shoff + i*e_shentsize
    name, typ, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from("<IIIIIIIIII", d, off)
    sections.append((name, typ, addr, offset, size, link, entsize))
# find symtab/strtab
symtab = None; strtab = None
for s in sections:
    if s[1] == 2: symtab = s
    if s[1] == 3 and strtab is None: strtab = s
symoff = symtab[3]; symsize = symtab[4]; symentsize = symtab[6]
stroff = strtab[3]; strsize = strtab[4]
syms = []
for o in range(symoff, symoff+symsize, symentsize or 16):
    nm, val, sz, info, other, shndx = struct.unpack_from("<IIIBBH", d, o)
    if nm == 0 or val == 0: continue
    e = d.find(b"\0", stroff+nm)
    name = d[stroff+nm:e].decode("latin1")
    syms.append((val, name))
syms.sort()
import bisect

def lookup(addr):
    i = bisect.bisect_right(syms, (addr, "\xff")) - 1
    if i < 0: return "?"
    val, name = syms[i]
    return "%s+0x%x" % (name, addr - val) if addr != val else name

for a in sys.argv[1:]:
    a = int(a, 0)
    print("0x%08x -> %s" % (a, lookup(a)))
