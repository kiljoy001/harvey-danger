#!/usr/bin/env python3
"""Convert a Plan 9 (Inferno) a.out kernel to a Multiboot-loadable ELF32.

The Plan 9 a.out stores text+data contiguously in the file, but the kernel
expects data page-aligned (a gap between text and data). We emit two PT_LOAD
segments using the addresses recorded in the embedded Multiboot header.
"""
import struct, sys

def main(path, out):
    with open(path, 'rb') as f:
        ahdr = f.read(32)
    if len(ahdr) < 32:
        sys.exit("short a.out header")
    magic, text, data, bss, syms, entry, spsz, pcsz = struct.unpack('>8I', ahdr)
    print(f"magic=0x{magic:x} text={text} data={data} bss={bss} "
          f"syms={syms} entry=0x{entry:x}")

    with open(path, 'rb') as f:
        f.seek(32)
        textbytes = f.read(text)
        databytes = f.read(data)
    assert len(textbytes) == text and len(databytes) == data

    # Locate the embedded Multiboot header (magic 0x1BADB002 little-endian).
    mb = b'\x02\xb0\xad\x1b'
    mo = textbytes.find(mb)
    if mo < 0 or mo + 32 > len(textbytes):
        sys.exit("no multiboot header found in text")
    mhdr = struct.unpack('<8I', textbytes[mo:mo+32])
    load_addr = mhdr[4]      # load_addr
    edata = mhdr[5]          # load_end_addr
    end = mhdr[6]            # bss_end_addr
    entry = mhdr[7]          # entry_addr
    data_load = edata - data
    print(f"mb: load_addr=0x{load_addr:x} edata=0x{edata:x} end=0x{end:x} "
          f"entry=0x{entry:x} data_load=0x{data_load:x}")

    # ELF32 header (52) + 2 phdrs (64) = 116, then text, then data.
    ehsize = 52
    phentsize = 32
    phnum = 2
    phoff = ehsize
    text_off = ehsize + phnum * phentsize
    data_off = text_off + text

    elf = bytearray()
    elf += b'\x7fELF'
    elf += bytes([1, 1, 1, 0]) + bytes(8)
    elf += struct.pack('<HHIIIIIHHHHHH',
                       2, 3, 1, entry, phoff, 0, 0,
                       ehsize, phentsize, phnum, 0, 0, 0)
    # phdr 0: text (R+X)
    elf += struct.pack('<IIIIIIII',
                       1, text_off, load_addr, load_addr, text, text, 5, 0x1000)
    # phdr 1: data+bss (R+W)
    elf += struct.pack('<IIIIIIII',
                       1, data_off, data_load, data_load, data, data + bss, 6, 0x1000)
    assert len(elf) == text_off
    elf += textbytes
    assert len(elf) == data_off
    elf += databytes

    with open(out, 'wb') as f:
        f.write(elf)
    print(f"wrote {out}: {len(elf)} bytes "
          f"(text@0x{load_addr:x}, data@0x{data_load:x}, entry=0x{entry:x})")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
