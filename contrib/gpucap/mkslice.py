import sys, struct, subprocess, os
va=int(sys.argv[1],0); ln=int(sys.argv[2],0); out=sys.argv[3]
src='libmali-g52-nocl.so'
subprocess.run(f"cp {src} {out}", shell=True, check=True)
open('chunk.bin','wb').write(open(src,'rb').read()[va:va+ln])
subprocess.run(f"llvm-objcopy --remove-section=.text --remove-section=.eh_frame --remove-section=.eh_frame_hdr --remove-section=.gcc_except_table {out}", shell=True, check=True)
subprocess.run(f"llvm-objcopy --add-section .text=chunk.bin --set-section-flags .text=alloc,code {out}", shell=True, check=True)
d=bytearray(open(out,'rb').read())
shoff=struct.unpack_from('<Q',d,0x28)[0]; shentsize=struct.unpack_from('<H',d,0x3a)[0]
shnum=struct.unpack_from('<H',d,0x3c)[0]; shstrndx=struct.unpack_from('<H',d,0x3e)[0]
def sh(i): return shoff+i*shentsize
strtab=struct.unpack_from('<Q',d,sh(shstrndx)+0x18)[0]
for i in range(shnum):
    no=struct.unpack_from('<I',d,sh(i))[0]
    nm=d[strtab+no:d.index(b'\0',strtab+no)].decode()
    if nm=='.text':
        h=sh(i)
        struct.pack_into('<Q',d,h+0x8, struct.unpack_from('<Q',d,h+0x8)[0]|0x6)
        struct.pack_into('<Q',d,h+0x10, va)
        struct.pack_into('<Q',d,h+0x30, 4)
open(out,'wb').write(bytes(d))
print(out, hex(va), hex(ln), os.path.getsize(out))
