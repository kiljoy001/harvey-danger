#!/usr/bin/env python3
import os, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
O9 = Path(os.environ.get("O9_REPO", ROOT.parent / "objective-9c"))
SRC = ROOT / "boot/aspera_init.o9"
C = ROOT / "build/aspera_init.o9.c"
OBJ = ROOT / "build/aspera_init.o"
OUT = ROOT / "build/aspera-init"
CFLAGS = ["-c","-O2","-static","-mno-red-zone","-mno-implicit-float","-ffreestanding",
 "-fno-builtin","-fno-stack-protector","-mcmodel=small","-Wall","-fcommon",
 f"-I{ROOT/'amd64/include'}",f"-I{ROOT/'sys/include'}",f"-I{O9}",f"-I{O9/'libtab'}"]
LIBS = ["libo9.a","libthread.a","lib9p.a","libauth.a","libauthsrv.a","libsec.a","libndb.a","libbio.a","libc.a"]
def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(C,"w") as f:
        subprocess.run([str(O9/"o9c/o9c"),str(SRC)],stdout=f,check=True)
    subprocess.run(["clang",*CFLAGS,str(C),"-o",str(OBJ)],check=True)
    subprocess.run(["ld.lld","-e_main","-static","-z","noseparate-code","-z","max-page-size=0x200000",
                    str(OBJ),*[str(ROOT/"build"/l) for l in LIBS],"-o",str(OUT)],check=True)
    print(f"created {OUT}")
if __name__=="__main__": main()
