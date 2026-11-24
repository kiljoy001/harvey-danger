#!/usr/bin/env python3
"""Build an SD image with mk-sdimg.py and verify its FAT and DTB contents."""

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

/ {
	#address-cells = <2>;
	#size-cells = <2>;
	compatible = "example,sdimg-test";

	chosen {
		stdout-path = "/serial@fe660000";
	};
};
"""


def props(path: Path, names: tuple[str, ...]) -> dict[str, list[str]]:
    out = {}
    for name in names:
        got = subprocess.check_output(
            ["fdtget", "-t", "x", str(path), "/chosen", name], text=True)
        out[name] = got.split()
    return out


def main() -> None:
    for tool in ("sfdisk", "mkfs.vfat", "mcopy", "fdtget", "fdtput", "dtc"):
        if shutil.which(tool) is None:
            print(f"SD image test: skip, missing {tool}")
            return

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        kernel = tmp / "Image"
        kernel.write_bytes(b"K" * 4096)
        sdk = tmp / "sdk.tar"
        sdk.write_bytes(b"S" * 8192)

        dts = tmp / "board.dts"
        dts.write_text(DTS)
        dtb = tmp / "board.dtb"
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", str(dtb), str(dts)], check=True)
        original = dtb.read_bytes()

        out = tmp / "sd.img"
        parts = tmp / "parts"
        subprocess.run([
            sys.executable, str(MK),
            "--image", str(kernel), "--dtb", str(dtb), "--sdk", str(sdk),
            "--out", str(out), "--ram", "0x40000000:0x80000000",
            "--teeshm", "0x9200000:0x200000", "--size-mb", "64",
            "--parts-dir", str(parts),
        ], check=True, stdout=subprocess.DEVNULL)

        if dtb.read_bytes() != original:
            raise SystemExit("mk-sdimg modified the input DTB")

        extracted = tmp / "extract"
        extracted.mkdir()
        for name in ("Image", "board.dtb", "sdk.tar"):
            subprocess.run(["mcopy", "-i", f"{out}@@{PART_OFFSET}",
                            f"::{name}", str(extracted / name)], check=True)

        if (extracted / "Image").read_bytes() != kernel.read_bytes():
            raise SystemExit("Image content mismatch")
        if (extracted / "sdk.tar").read_bytes() != sdk.read_bytes():
            raise SystemExit("sdk.tar content mismatch")

        got = props(extracted / "board.dtb",
                    ("harvey,bootarchive", "harvey,teeshm", "harvey,ram"))
        want = {
            "harvey,bootarchive": ["0", "50000000", "0", "2000"],
            "harvey,teeshm": ["0", "9200000", "0", "200000"],
            "harvey,ram": ["0", "40000000", "0", "80000000"],
        }
        if got != want:
            raise SystemExit(f"stamped properties mismatch: {got}")

        part = parts / "sd.part.00"
        if not part.is_file() or part.stat().st_size != out.stat().st_size:
            raise SystemExit("parts split mismatch")

        print("SD image build, FAT contents and stamped /chosen PASS")


if __name__ == "__main__":
    main()
