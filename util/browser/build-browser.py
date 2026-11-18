#!/usr/bin/env python3
"""Build the browser pipeline test for Harvey/ARM64.

This links the stage servers against the same libc/lib9p/libthread set the
filesystem servers use.  The harness is util/browser/test-browser.py.

    python3 util/browser/build-browser.py
"""

from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "util" / "arm64"))

import build_userland  # noqa: E402

# A free slot in the ARM64 userland address map (see build_userland.py).
BROWSER_BASE = 0xCE00000


def main() -> None:
    out = ROOT / "build" / "arm64-browser"
    out.mkdir(parents=True, exist_ok=True)
    libc = ROOT / "build" / "arm64-libc" / "libc.a"
    o9 = ROOT / "build" / "arm64-o9"
    if not libc.exists():
        sys.exit(f"missing {libc}; run util/arm64/build_libc.py first")

    src = ROOT / "sys" / "src" / "cmd" / "browser"
    sources = [
        src / "browsertest.c",
        src / "ringfs.c",
        src / "htmlfs.c",
        src / "cssfs.c",
        src / "jsfs.c",
        src / "layoutfs.c",
        src / "paintfs.c",
        src / "textfs.c",
    ]
    extra = [
        o9 / "lib9p.a",
        o9 / "libthread.a",
        o9 / "libauth.a",
        o9 / "libauthsrv.a",
        o9 / "libsec.a",
        o9 / "libndb.a",
        o9 / "libbio.a",
    ]
    image, entry = build_userland.build_elf(
        out, libc, "browsertest", sources, BROWSER_BASE, extra,
        cflags=["-D__HARVEY__"],
    )
    print(f"built {out / 'browsertest'} ({len(image)} bytes, entry {entry:#x})")


if __name__ == "__main__":
    main()
