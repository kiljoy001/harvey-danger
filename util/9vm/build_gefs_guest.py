#!/usr/bin/env python3
"""Build 9front GEFS for the Harvey amd64 guest without modifying 9front."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
COMPAT = HERE / "gefs_compat"
SOURCES = ("blk", "cache", "check", "cons", "dump", "error", "fs",
           "hash", "load", "main", "pack", "ream", "snap", "tree", "user")


def prepare(source, dest):
    dest.mkdir(parents=True, exist_ok=True)
    for name in SOURCES:
        shutil.copy2(source / f"{name}.c", dest / f"{name}.c")
    for name in ("dat.h", "fns.h", "atomic.h"):
        shutil.copy2(source / name, dest / name)
    shutil.copy2(source.parents[2] / "include/avl.h", dest / "avl.h")

    path = dest / "dat.h"
    text = path.read_text()
    assert text.count("Mount\t*next;") == 1
    path.write_text(text.replace("Mount\t*next;", "Mount\t*mnext;"))

    # GCC promotes the original byte shifts to signed int. A high bit in the
    # low word then sign-extends the result and rejects valid superblocks.
    path = dest / "fns.h"
    text = path.read_text()
    start = text.index("#define\tUNPACK8(p)")
    end = text.index("void*\temalloc", start)
    text = text[:start] + """static inline u32int
gefsunpack32(void *p)
{
    uchar *b = p;
    return ((u32int)b[0]<<24) | ((u32int)b[1]<<16) |
           ((u32int)b[2]<<8) | b[3];
}

static inline u64int
gefsunpack64(void *p)
{
    uchar *b = p;
    return ((u64int)gefsunpack32(b)<<32) | gefsunpack32(b+4);
}

static inline void
gefspack64(void *p, u64int v)
{
    uchar *b = p;
    b[0] = v>>56; b[1] = v>>48; b[2] = v>>40; b[3] = v>>32;
    b[4] = v>>24; b[5] = v>>16; b[6] = v>>8; b[7] = v;
}

#define UNPACK8(p) (((uchar*)(p))[0])
#define UNPACK16(p) (((uchar*)(p))[0]<<8 | ((uchar*)(p))[1])
#define UNPACK32(p) gefsunpack32(p)
#define UNPACK64(p) gefsunpack64(p)
#define PACK8(p,v) do{((uchar*)(p))[0]=(v);}while(0)
#define PACK16(p,v) do{((uchar*)(p))[0]=(v)>>8;((uchar*)(p))[1]=(v);}while(0)
#define PACK32(p,v) do{u32int x=(v); ((uchar*)(p))[0]=x>>24; \\
    ((uchar*)(p))[1]=x>>16; ((uchar*)(p))[2]=x>>8; ((uchar*)(p))[3]=x;}while(0)
#define PACK64(p,v) gefspack64(p, (u64int)(v))

""" + text[end:]
    path.write_text(text)

    path = dest / "snap.c"
    text = path.read_text()
    old = """\t\tdeltree = 1;
\t\tm[nm].op = Orelink;
\t\tretag2kv(t->pred, succ, 0, 0, &m[nm], buf[nm], sizeof(buf[nm]));
\t\tnm++;
"""
    new = """\t\tdeltree = 1;
\t\tif(t->pred != -1){
\t\t\tm[nm].op = Orelink;
\t\t\tretag2kv(t->pred, succ, 0, 0, &m[nm], buf[nm], sizeof(buf[nm]));
\t\t\tnm++;
\t\t}
"""
    assert text.count(old) == 1
    text = text.replace(old, new)
    old = """\ti = 0;
\tm[i].op = Orelink;
\tif(o->nlbl == 0 && o->nref == 1){
\t\tt->pred = o->pred;
\t\tretag2kv(t->pred, t->gen, 0, 0, &m[i], buf[i], sizeof(buf[i]));
\t}else{
\t\tt->pred = o->gen;
\t\tretag2kv(t->pred, t->gen, -1, 1, &m[i], buf[i], sizeof(buf[i]));
\t}
\ti++;
"""
    new = """\ti = 0;
\tif(o->nlbl == 0 && o->nref == 1){
\t\tt->pred = o->pred;
\t\tif(t->pred != -1){
\t\t\tm[i].op = Orelink;
\t\t\tretag2kv(t->pred, t->gen, 0, 0, &m[i], buf[i], sizeof(buf[i]));
\t\t\ti++;
\t\t}
\t}else{
\t\tt->pred = o->gen;
\t\tm[i].op = Orelink;
\t\tretag2kv(t->pred, t->gen, -1, 1, &m[i], buf[i], sizeof(buf[i]));
\t\ti++;
\t}
"""
    assert text.count(old) == 1
    text = text.replace(old, new)
    path.write_text(text)

    for name in ("fs.c", "snap.c"):
        path = dest / name
        text = path.read_text().replace("mnt->next", "mnt->mnext")
        text = text.replace("me->next", "me->mnext")
        if name == "fs.c":
            assert text.count("d = *o->dent;") == 1
            text = text.replace("d = *o->dent;",
                                "memmove(&d, &o->dent->Xdir, sizeof(d));")
            assert text.count('tmfmt(tm, "YYYY.MM.DD[_]hh:mm:ss")') == 1
            text = text.replace('seprint(p, e, "%s@%s.%τ",\n\t\tname, c->tag,\n\t\ttmfmt(tm, "YYYY.MM.DD[_]hh:mm:ss"));',
                'seprint(p, e, "%s@%s.%04d.%02d.%02d_%02d:%02d:%02d",\n'
                '\t\tname, c->tag, tm->year+1900, tm->mon+1, tm->mday,\n'
                '\t\ttm->hour, tm->min, tm->sec);')
            text = text.replace("tmnow(&tm, nil);", "guesttmnow(&tm);")
            text = text.replace("now = tmnorm(&tm);", "now = time(nil);")
        path.write_text(text)

    path = dest / "main.c"
    text = path.read_text()
    assert text.count("\ttmfmtinstall();") == 1
    text = text.replace("\ttmfmtinstall();", "")
    assert text.count("(void*)ctlfd") == 1
    text = text.replace("(void*)ctlfd", "(void*)(uintptr)ctlfd")
    assert text.count("\tlongjmp(c->errlab[--c->nerrlab], -1);") == 2
    text = text.replace("\tlongjmp(c->errlab[--c->nerrlab], -1);",
                        "\tlongjmp(c->errlab[--c->nerrlab], -1);\n"
                        "\t__builtin_unreachable();")
    path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT.parent / "plan9front",
                        help="9front source checkout")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/gefs-guest")
    args = parser.parse_args()
    source = args.source.resolve() / "sys/src/cmd/gefs"
    if not (source / "main.c").is_file():
        parser.error("9front GEFS source not found")
    for tool in ("gcc", "ld.lld"):
        if shutil.which(tool) is None:
            parser.error(tool + " is required")
    build = ROOT / "build/gefs-guest-build"
    prepare(source, build)
    flags = ["-c", "-O2", "-g", "-fplan9-extensions", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fcommon",
             "-mno-red-zone", "-mno-sse", "-Wno-unknown-pragmas",
             "-Wno-incompatible-pointer-types", "-I" + str(COMPAT),
             "-I" + str(build), "-I" + str(ROOT / "sys/include")]
    objects = []
    for name in SOURCES:
        src = build / f"{name}.c"
        obj = build / f"{name}.o"
        subprocess.run(["gcc", *flags, str(src), "-o", str(obj)], check=True)
        objects.append(obj)
    for name, src in (("avl", source.parents[1] / "libavl/avl.c"),
                      ("atomic", COMPAT / "atomic.c"),
                      ("procsetname", COMPAT / "procsetname.c"),
                      ("time", COMPAT / "time.c")):
        obj = build / f"{name}.o"
        subprocess.run(["gcc", *flags, str(src), "-o", str(obj)], check=True)
        objects.append(obj)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = ["ld.lld", "-e_main", "-static", "-z", "noseparate-code",
               "-z", "max-page-size=0x200000", *map(str, objects),
               str(ROOT / "build/libthread.a"), str(ROOT / "build/libauth.a"),
               str(ROOT / "build/libbio.a"), str(ROOT / "build/libc.a"),
               "-o", str(args.output)]
    subprocess.run(command, check=True)
    print("GEFS guest binary:", args.output)


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, AssertionError) as error:
        print("build_gefs_guest:", error, file=sys.stderr)
        raise SystemExit(1)
