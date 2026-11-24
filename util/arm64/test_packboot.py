#!/usr/bin/env python3
"""Build an Android boot image with pack_boot.py and verify its contents."""

from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PACK = ROOT / "util/arm64/pack_boot.py"
PAGE = 4096

DTS = """
/dts-v1/;

/ {
	#address-cells = <2>;
	#size-cells = <2>;
	compatible = "example,packboot-test";

	chosen {
		stdout-path = "/serial@fe660000";
	};
};
"""


def main() -> None:
    for tool in ("dtc", "fdtget", "fdtput"):
        if shutil.which(tool) is None:
            print(f"pack_boot test: skip, missing {tool}")
            return

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        dts = tmp / "board.dts"
        dts.write_text(DTS)
        dtb = tmp / "board.dtb"
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb",
                        "-o", str(dtb), str(dts)], check=True)

        kernel = tmp / "kernel.img"
        kernel.write_bytes(b"K" * 4096)
        dtb_data = dtb.read_bytes()

        # Minimal Android boot image: header, 4 KiB kernel, then the DTB.
        header = bytearray(PAGE)
        struct.pack_into("<I", header, 8, len(kernel.read_bytes()))
        struct.pack_into("<I", header, 1648, len(dtb_data))
        template = tmp / "template.img"
        template.write_bytes(bytes(header) + kernel.read_bytes() + dtb_data)
        original_template = template.read_bytes()

        out = tmp / "boot.img"
        subprocess.run([
            sys.executable, str(PACK),
            "--template", str(template),
            "--kernel", str(kernel),
            "--output", str(out),
            "--bootarchive", "0x49000000:0x1000",
            "--teeshm", "0x52000000:0x200000",
            "--scm", "0x53000000:0x1000",
            "--ram", "0x40000000:0x80000000",
        ], check=True, stdout=subprocess.DEVNULL)

        if template.read_bytes() != original_template:
            raise SystemExit("pack_boot modified the template")

        data = out.read_bytes()
        ks = struct.unpack("<I", data[8:12])[0]
        off = PAGE + ((ks + PAGE - 1) // PAGE) * PAGE
        ds = struct.unpack("<I", data[1648:1652])[0]
        if ks != len(kernel.read_bytes()):
            raise SystemExit(f"kernel size {ks}")
        packed = tmp / "packed.dtb"
        packed.write_bytes(data[off:off + ds])

        want = {
            "harvey,bootarchive": ["0", "49000000", "0", "1000"],
            "harvey,teeshm": ["0", "52000000", "0", "200000"],
            "harvey,scm": ["0", "53000000", "0", "1000"],
            "harvey,ram": ["0", "40000000", "0", "80000000"],
        }
        for prop, cells in want.items():
            got = subprocess.check_output(
                ["fdtget", "-t", "x", str(packed), "/chosen", prop],
                text=True).split()
            if got != cells:
                raise SystemExit(f"{prop}: {got} != {cells}")

        arena = subprocess.check_output(
            ["fdtget", "-t", "x", str(packed), "/chosen",
             "harvey,lkl-arena-size"], text=True).split()
        if arena != ["0", "14000000"]:
            raise SystemExit(f"lkl-arena-size: {arena}")

        print("pack_boot image, stamped /chosen and arena size PASS")


if __name__ == "__main__":
    main()
