#!/usr/bin/env python3
"""Build a multi-board SD image with mk-sdimg.py and verify its contents."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MK = ROOT / "util/arm64/mk-sdimg.py"
PART_OFFSET = 2048 * 512

DTS = """
/dts-v1/;

/ {{
	#address-cells = <2>;
	#size-cells = <2>;
	compatible = "{compat}";

	chosen {{
		stdout-path = "/serial@fe660000";
	}};
}};
"""


def make_dtb(tmp: Path, name: str, compat: str) -> Path:
    dts = tmp / f"{name}.dts"
    dts.write_text(DTS.format(compat=compat))
    dtb = tmp / f"{name}.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                    "-o", str(dtb), str(dts)], check=True)
    return dtb


def props(path: Path, names: tuple[str, ...]) -> dict[str, list[str]]:
    out = {}
    for name in names:
        got = subprocess.check_output(
            ["fdtget", "-t", "x", str(path), "/chosen", name], text=True)
        out[name] = got.split()
    return out


def main() -> None:
    for tool in ("sfdisk", "mkfs.vfat", "mcopy", "mmd", "mtype",
                 "fdtget", "fdtput", "dtc"):
        if shutil.which(tool) is None:
            print(f"SD image test: skip, missing {tool}")
            return

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        kernel = tmp / "Image"
        kernel.write_bytes(b"K" * 4096)
        sdk = tmp / "sdk.tar"
        sdk.write_bytes(b"S" * 8192)

        one = make_dtb(tmp, "one", "example,sdimg-one")
        two = make_dtb(tmp, "two", "example,sdimg-two")
        original = {p: p.read_bytes() for p in (one, two)}

        out = tmp / "sd.img"
        parts = tmp / "parts"
        subprocess.run([
            sys.executable, str(MK),
            "--image", str(kernel), "--sdk", str(sdk), "--out", str(out),
            "--dtb", f"one={one}", "--dtb", f"two={two}",
            "--default-dtb", "one",
            "--ram", "0x40000000:0x80000000",
            "--teeshm", "0x9200000:0x200000",
            "--size-mb", "64", "--parts-dir", str(parts),
        ], check=True, stdout=subprocess.DEVNULL)

        for path, data in original.items():
            if path.read_bytes() != data:
                raise SystemExit(f"mk-sdimg modified the input DTB {path}")

        extract = tmp / "extract"
        extract.mkdir()
        names = ["Image", "sdk.tar", "board.dtb",
                 "dtbs/one.dtb", "dtbs/two.dtb", "boot.cmd"]
        if shutil.which("mkimage") is not None:
            names.append("boot.scr")
        for name in names:
            dest = extract / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["mcopy", "-i", f"{out}@@{PART_OFFSET}",
                            f"::{name}", str(dest)], check=True)

        if (extract / "Image").read_bytes() != kernel.read_bytes():
            raise SystemExit("Image content mismatch")
        if (extract / "sdk.tar").read_bytes() != sdk.read_bytes():
            raise SystemExit("sdk.tar content mismatch")
        if (extract / "board.dtb").read_bytes() != \
                (extract / "dtbs" / "one.dtb").read_bytes():
            raise SystemExit("board.dtb is not the default DTB")

        got = props(extract / "board.dtb",
                    ("harvey,bootarchive", "harvey,teeshm", "harvey,ram"))
        want = {
            "harvey,bootarchive": ["0", "50000000", "0", "2000"],
            "harvey,teeshm": ["0", "9200000", "0", "200000"],
            "harvey,ram": ["0", "40000000", "0", "80000000"],
        }
        if got != want:
            raise SystemExit(f"stamped properties mismatch: {got}")

        script = (extract / "boot.cmd").read_text()
        for needle in ("dtbs/one.dtb", "0x50000000", "sdk.tar", "booti"):
            if needle not in script:
                raise SystemExit(f"boot.cmd missing {needle!r}: {script}")

        part = parts / "sd.part.00"
        if not part.is_file() or part.stat().st_size != out.stat().st_size:
            raise SystemExit("parts split mismatch")

        print("SD multi-board image, FAT contents and stamped /chosen PASS")


if __name__ == "__main__":
    main()
