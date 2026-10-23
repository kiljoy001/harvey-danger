#!/usr/bin/env python3
"""Build an ELF with a symbol table from an Inferno/Plan9 a.out, for GDB.

Symbols in the a.out use the virtual (KZERO + phys) addresses, which is what
GDB sees through the guest MMU.
"""
import struct, sys

import os
HERE = os.path.dirname(os.path.abspath(__file__))
PURG = os.environ.get("PURGATORIO",
                      os.path.join(os.path.dirname(os.path.dirname(HERE)), "purgatorio"))
WORK = os.environ.get("NINEVM_WORK", "/tmp/9vm-inferno")
AOUT = os.path.join(PURG, "os/pc/ipc")
OUT = os.path.join(WORK, "ipc-sym.elf")
os.makedirs(WORK, exist_ok=True)

ahdr = open(AOUT, "rb").read(32)
magic, text, data, bss, syms, entry, spsz, pcsz = struct.unpack(">8I", ahdr)
print("a.out magic=0x%x text=%d data=%d bss=%d syms=%d entry=0x%x"
      % (magic, text, data, bss, syms, entry))

blob = open(AOUT, "rb").read()
symoff = 32 + text + data
symblob = blob[symoff:symoff+syms]
print("symblob len", len(symblob), "expected", syms)

syms_parsed = []
o = 0
n = len(symblob)
while o + 5 <= n:
    val = struct.unpack(">I", symblob[o:o+4])[0]
    tb = symblob[o+4]
    o += 5
    t = tb - 0x80
    if t < 0 or t > 0x7f:
        print("bad type byte 0x%x at %d" % (tb, o-1))
        break
    if chr(t) in "Zz":
        # file-scope name: one byte then pairs until a 0 pair
        o += 1
        while o + 1 < n and not (symblob[o] == 0 and symblob[o+1] == 0):
            o += 2
        o += 2
        name = ""
    else:
        e = symblob.find(b"\0", o)
        if e < 0:
            break
        name = symblob[o:e].decode("latin1")
        o = e + 1
    if name:
        syms_parsed.append((val, chr(t), name))
    if o >= n:
        break

print("parsed %d symbols; first 6:" % len(syms_parsed))
for s in syms_parsed[:6]:
    print("  0x%08x %s %s" % s)
print("last:", syms_parsed[-1] if syms_parsed else None)

# ---- emit ELF ----
KZERO = 0x80000000
# We reuse the a.out bytes (text at 0x100020, data at 0x217000) but describe
# the sections at virtual addresses so GDB symbols line up.
TEXT_VA = 0x80100020
DATA_VA = 0x80217000

textbytes = blob[32:32+text]
databytes = blob[32+text:32+text+data]

# Section data layout after the ELF header + program headers.
ehsize = 52
phnum = 2
phentsize = 32
phoff = ehsize
textoff = ehsize + phnum*phentsize
dataoff = textoff + len(textbytes)
symtaboff = dataoff + len(databytes)
# strtab
strtab = b"\0"
sym_entries = []
for val, t, name in syms_parsed:
    if not name:
        continue
    nameoff = len(strtab)
    strtab += name.encode("latin1") + b"\0"
    va = val
    if t in "Tt":  stt = 2  # FUNC
    elif t in "DdBb": stt = 1  # OBJECT
    else: stt = 0
    if va == 0:
        continue
    if va < DATA_VA:
        shndx = 1
    elif va < DATA_VA + len(databytes):
        shndx = 2
    else:
        shndx = 3
    sym_entries.append((nameoff, va, stt, shndx))
stroff = symtaboff + 16*(len(sym_entries)+1)
shstr = b"\0.text\0.data\0.bss\0.symtab\0.strtab\0.shstrtab\0"
names = {}
p = 1
for nm in [".text", ".data", ".bss", ".symtab", ".strtab", ".shstrtab"]:
    names[nm] = p
    p += len(nm) + 1
shstroff = stroff + len(strtab)
shoff = shstroff + len(shstr)
shnum = 7
shstrndx = 6

out = bytearray()
# ELF header
out += b"\x7fELF\x01\x01\x01" + b"\0"*9
out += struct.pack("<HHIIIIIHHHHHH",
                   2, 3, 1,
                   TEXT_VA,          # e_entry (virtual)
                   phoff, shoff, 0,
                   ehsize, phentsize, phnum,
                   40, shnum, shstrndx)
# program headers (virtual addrs)
out += struct.pack("<IIIIIIII", 1, textoff, TEXT_VA, TEXT_VA,
                   len(textbytes), len(textbytes), 5, 0x1000)
out += struct.pack("<IIIIIIII", 1, dataoff, DATA_VA, DATA_VA,
                   len(databytes), len(databytes), 6, 0x1000)
assert len(out) == textoff
out += textbytes
out += databytes
# symtab
assert len(out) == symtaboff
out += b"\0"*16  # null symbol
for nameoff, va, stt, shndx in sym_entries:
    out += struct.pack("<IIIBBH", nameoff, va, 0, (1 << 4) | (stt & 0xf), 0, shndx)
assert len(out) == stroff
out += strtab
assert len(out) == shstroff
out += shstr
# section headers
sh = []
sh.append(b"\0"*40)
sh.append(struct.pack("<IIIIIIIIII", names[".text"], 1, 6, TEXT_VA, textoff,
                      len(textbytes), 0, 0, 4, 0))
sh.append(struct.pack("<IIIIIIIIII", names[".data"], 1, 3, DATA_VA, dataoff,
                      len(databytes), 0, 0, 4, 0))
sh.append(struct.pack("<IIIIIIIIII", names[".bss"], 8, 3, DATA_VA+len(databytes)+ (0x1000 - (len(databytes)%0x1000))%0x1000,
                      0, bss, 0, 0, 4, 0))
sh.append(struct.pack("<IIIIIIIIII", names[".symtab"], 2, 0, 0, symtaboff,
                      16*len(sym_entries), 5, 1, 4, 16))
sh.append(struct.pack("<IIIIIIIIII", names[".strtab"], 3, 0, 0, stroff,
                      len(strtab), 0, 0, 1, 0))
sh.append(struct.pack("<IIIIIIIIII", names[".shstrtab"], 3, 0, 0, shstroff,
                      len(shstr), 0, 0, 1, 0))
assert len(out) == shoff
for s in sh:
    out += s

open(OUT, "wb").write(out)
print("wrote", OUT, len(out), "bytes,", len(sym_entries), "symbols")
