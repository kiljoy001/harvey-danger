"""Small content/mtime build cache shared by the ARM64 build scripts.

The build scripts used to rebuild and relink everything on every run, which
made a one-line driver change cost a full kernel link and a board reboot.
Everything here is intentionally file based: an object is rebuilt when its
command line, its own source, or any header it could include changes.
"""

from pathlib import Path
import hashlib
import os
import subprocess


ROOT = Path(__file__).resolve().parents[2]

# Header trees that can affect a compiled object.  Kept coarse on purpose:
# over-invalidating is cheap, missing a change is not.
HEADER_ROOTS = [
    ROOT / "aarch64/include",
    ROOT / "sys/include",
    ROOT / "sys/src/9/port",
    ROOT / "sys/src/9/ip",
    ROOT / "sys/src/9/aarch64",
    ROOT / "sys/src/libc",
    ROOT.parent / "objective-9c",
]

_header_stamp: str | None = None


def header_stamp() -> str:
    """Newest header anywhere in the include/port/libc trees."""
    global _header_stamp
    if _header_stamp is not None:
        return _header_stamp
    newest = 0
    for root in HEADER_ROOTS:
        if not root.is_dir():
            continue
        for p in root.rglob("*"):
            if p.suffix not in (".h", ".json"):
                continue
            try:
                st = p.stat()
            except OSError:
                continue
            if st.st_mtime_ns > newest:
                newest = st.st_mtime_ns
    _header_stamp = str(newest)
    return _header_stamp


def file_stamp(path: Path) -> str:
    """Content hash for small inputs so regenerated-identical files do not
    invalidate the cache; size+mtime for anything large."""
    try:
        st = path.stat()
    except OSError:
        return "missing"
    if st.st_size <= 512 * 1024:
        try:
            return f"{path}:{hashlib.sha256(path.read_bytes()).hexdigest()}"
        except OSError:
            pass
    return f"{path}:{st.st_size}:{st.st_mtime_ns}"


def signature(command: list[str], sources=(), extra: str = "") -> str:
    h = hashlib.sha256()
    h.update("\0".join(command).encode())
    h.update(b"\0")
    h.update(extra.encode())
    for source in sources:
        h.update(file_stamp(Path(source)).encode())
        h.update(b"\0")
    h.update(header_stamp().encode())
    return h.hexdigest()


def up_to_date(stamp: Path, sig: str) -> bool:
    try:
        return stamp.read_text() == sig
    except OSError:
        return False


def record(stamp: Path, sig: str) -> None:
    stamp.write_text(sig)


def run_cached(command: list[str], outputs: list[Path], *, sources=(),
               extra: str = "", capture: bool = True,
               input_text: str | None = None, env=None, cwd=None):
    """Run command unless every output is newer than its signature.

    Returns True when the command actually ran.
    """
    outputs = [Path(o) for o in outputs]
    sig = signature(command, sources, extra + (input_text or ""))
    stamp = outputs[0].with_suffix(outputs[0].suffix + ".sig")
    if all(o.exists() for o in outputs) and up_to_date(stamp, sig):
        return False
    result = subprocess.run(command, text=True, capture_output=capture,
                            input=input_text, env=env, cwd=cwd)
    if result.returncode:
        raise RuntimeError(
            f"{' '.join(map(str, command))}\n"
            f"{(result.stderr or result.stdout or '').strip()}")
    for output in outputs:
        if not output.exists():
            raise RuntimeError(f"command did not produce {output}")
    record(stamp, sig)
    return True
