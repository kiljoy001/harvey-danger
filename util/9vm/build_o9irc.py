#!/usr/bin/env python3
"""Build o9irc for Aspera without changing the 9front o9irc checkout."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from prism import O9_REPO, compile_o9

ROOT = Path(__file__).resolve().parents[2]
STDLIB = Path(O9_REPO) / "stdlib"

TLS_START = "function IrcDialTLSRaw("
TLS_END = "\nclass IrcNetwork {"
TLS_GUEST = '''function IrcDialTLSRaw(string address, string host, string trustPath) string {
    return "error:TLS unavailable in Aspera guest";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT.parent / "o9irc")
    args = parser.parse_args()
    source = args.source.resolve()
    if not (source / "ircfs.o9").is_file():
        parser.error(f"o9irc source not found: {source}")
    if not (STDLIB / "file.o9").is_file():
        parser.error(f"Objective-9C stdlib not found: {STDLIB}")
    subprocess.run([sys.executable, str(ROOT / "util/9vm/build_libflate.py")],
                   check=True)
    with tempfile.TemporaryDirectory(prefix="aspera-o9irc-", dir=ROOT / "build") as name:
        stage = Path(name)
        for path in source.glob("*.o9"):
            if path.name == "ircfs.o9":
                content = path.read_text()
                start = content.index(TLS_START)
                end = content.index(TLS_END, start)
                (stage / path.name).write_text(content[:start] + TLS_GUEST +
                                               content[end:])
            else:
                (stage / path.name).symlink_to(path)
        for path in STDLIB.glob("*.o9"):
            dest = stage / path.name
            if not dest.exists():
                dest.symlink_to(path)
        for name, output in (("ircfs", "o9irc-ircfs"),
                             ("gui", "o9irc-gui"),
                             ("irc", "o9irc-cli")):
            dest = ROOT / "build" / output
            compile_o9(stage / f"{name}.o9", dest)
            subprocess.run(["llvm-strip", "--strip-debug", str(dest)], check=True)
            print(f"Built {dest}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            ValueError) as error:
        print(f"build_o9irc: {error}", file=sys.stderr)
        raise SystemExit(1)
