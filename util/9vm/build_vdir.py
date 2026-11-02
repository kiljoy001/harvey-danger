#!/usr/bin/env python3
"""Build Harvey's graphical directory browser for amd64."""
import argparse
from pathlib import Path
import subprocess
import tempfile

import prism


def build(repo, output, draw=None):
    repo, output = Path(repo), Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    source = repo / "sys/src/cmd/vdir"
    with tempfile.TemporaryDirectory(prefix="vdir-build-") as tmp:
        objects = []
        for name in ("vdir", "alert", "confirm", "theme", "enter"):
            obj = Path(tmp) / f"{name}.o"
            flags = [f for f in prism.CFLAGS if f != "-mno-implicit-float"]
            subprocess.run([prism.CC, *flags, "-mstackrealign",
                            str(source / f"{name}.c"), "-o", str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run([prism.LD, *prism.LDFLAGS, *objects,
                        str(draw or repo / "build/libdraw.a"),
                        str(repo / "build/libmemdraw.a"),
                        str(repo / "build/libthread.a"),
                        str(repo / "build/libplumb.a"),
                        str(repo / "build/libbio.a"),
                        str(repo / "build/libc.a"), "-o", str(output)], check=True)
    print(f"Built {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=Path(prism.REPO_ROOT) / "build/aspera-vdir")
    parser.add_argument("--draw", type=Path)
    args = parser.parse_args()
    build(prism.REPO_ROOT, args.out, args.draw)
