#!/usr/bin/env python3
"""Parse an ESP8266 full-flash dump (NONOS SDK layout).

Observed format (v1 images, "2nd boot 1.6" scheme):
  0x000000 boot image, 0x001000 user app image, parameter sectors at
  the tail of flash. This script is read-only: it reports the layout,
  verifies image checksums, extracts each image, and hashes regions.

Usage: parse_flash.py <flash.bin> <outdir>
Output: extracted images in <outdir>/../images, JSON + text report
in <outdir>. Findings are raw observations for the B0 inventory.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

FLASH_SIZE = 0x400000
IMAGE_OFFSETS = (0x000000, 0x001000)


def parse_image(data, offset):
    """Parse one ESP8266 v1 image. Returns dict or None."""
    if data[offset] != 0xE9:
        return None
    nseg = data[offset + 1]
    flash_mode = data[offset + 2]
    size_freq = data[offset + 3]
    (entry,) = struct.unpack_from("<I", data, offset + 4)
    pos = offset + 8
    checksum = 0xEF
    segments = []
    for _ in range(nseg):
        addr, length = struct.unpack_from("<II", data, pos)
        pos += 8
        seg = data[pos:pos + length]
        for b in seg:
            checksum ^= b
        segments.append({"load_addr": addr, "length": length,
                         "file_offset": pos - offset})
        pos += length
    # Pad so checksum byte lands on a 16-byte boundary (relative to file).
    end = pos
    while (end - offset + 1) % 16 != 0:
        end += 1
    stored = data[end]
    total = end - offset + 1
    return {
        "offset": offset, "segments_count": nseg,
        "flash_mode": flash_mode, "size_freq_byte": size_freq,
        "entry": entry, "segments": segments,
        "computed_length": total,
        "checksum_stored": stored, "checksum_computed": checksum,
        "checksum_valid": stored == checksum,
        "sha256": hashlib.sha256(data[offset:offset + total]).hexdigest(),
    }


def region_stats(data, start, end):
    chunk = data[start:end]
    return {
        "start": start, "end": end, "length": end - start,
        "all_ff": all(b == 0xFF for b in chunk),
        "sha256": hashlib.sha256(chunk).hexdigest(),
    }


def parse_v2_image(data, offset):
    """Parse an ESP8266 V2 image (magic 0xEA, loaded by 2nd boot).

    Layout, mirroring esptool's ESP8266V2FirmwareImage: common header
    (segment-count field is the constant 4), one IROM segment stored
    with load address 0 and excluded from the checksum, then a full
    v1 header and its segments, then checksum padding.
    """
    if data[offset] != 0xEA or data[offset + 1] != 4:
        return None
    (entry,) = struct.unpack_from("<I", data, offset + 4)
    pos = offset + 8
    irom_addr, irom_len = struct.unpack_from("<II", data, pos)
    pos += 8
    irom_fileoff = pos - offset
    pos += irom_len
    second = parse_image(data, pos)
    if second is None:
        return {"offset": offset, "error": "second E9 header not found",
                "irom_length": irom_len}
    total = (pos - offset) + second["computed_length"]
    return {
        "offset": offset, "format": "v2", "entry": entry,
        "irom_stored_addr": irom_addr, "irom_length": irom_len,
        "irom_file_offset": irom_fileoff,
        "irom_mapped_addr": 0x40200000 + offset + 8,
        "second_header_offset": pos,
        "second_image": second,
        "computed_length": total,
        "checksum_valid": second["checksum_valid"],
        "sha256": hashlib.sha256(data[offset:offset + total]).hexdigest(),
    }


def main():
    src = Path(sys.argv[1])
    outdir = Path(sys.argv[2])
    imgdir = outdir.parent / "images"
    outdir.mkdir(parents=True, exist_ok=True)
    imgdir.mkdir(parents=True, exist_ok=True)
    data = src.read_bytes()
    assert len(data) == FLASH_SIZE, f"expected {FLASH_SIZE} bytes, got {len(data)}"

    report = {"source": str(src), "size": len(data),
              "sha256": hashlib.sha256(data).hexdigest(), "images": []}
    covered = []
    boot = parse_image(data, 0x000000)
    if boot is not None:
        out = imgdir / "boot.bin"
        out.write_bytes(data[0:boot["computed_length"]])
        boot["extracted"] = str(out)
        boot["role"] = "2nd boot 1.6 (observed string at file 0xd01)"
        report["images"].append(boot)
        covered.append((0, boot["computed_length"]))
    app = parse_v2_image(data, 0x001000)
    if app is not None and "error" not in app:
        out = imgdir / "user1-app.bin"
        out.write_bytes(data[0x1000:0x1000 + app["computed_length"]])
        app["extracted"] = str(out)
        # IROM is memory-mapped code; save it alone for disassembly.
        irom = data[0x1000 + app["irom_file_offset"]:
                    0x1000 + app["irom_file_offset"] + app["irom_length"]]
        irom_out = imgdir / "user1-irom.bin"
        irom_out.write_bytes(irom)
        app["irom_extracted"] = str(irom_out)
        app["irom_sha256"] = hashlib.sha256(irom).hexdigest()
        second = app["second_image"]
        for seg in second["segments"]:
            seg_data = data[app["second_header_offset"] + seg["file_offset"]:
                            app["second_header_offset"] + seg["file_offset"] + seg["length"]]
            seg["sha256"] = hashlib.sha256(seg_data).hexdigest()
        report["images"].append(app)
        covered.append((0x1000, 0x1000 + app["computed_length"]))
    else:
        report["images"].append(app or {"offset": 0x1000, "error": "no V2 magic"})

    # Tail parameter area: last 16KB holds SDK system parameters.
    report["tail_16k"] = region_stats(data, FLASH_SIZE - 0x4000, FLASH_SIZE)
    # Gap between end of app image and tail, in 4KB sectors, blank or not.
    app_end = max(end for _, end in covered)
    app_end_aligned = (app_end + 0xFFF) & ~0xFFF
    blocks = []
    pos = app_end_aligned
    while pos < FLASH_SIZE - 0x4000:
        end = min(pos + 0x1000, FLASH_SIZE - 0x4000)
        blocks.append(region_stats(data, pos, end))
        pos = end
    report["app_end"] = app_end
    report["middle_blocks"] = blocks

    (outdir / "layout.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = [f"source {report['source']} sha256 {report['sha256']}"]
    for img in report["images"]:
        if "error" in img:
            lines.append(f"@{img['offset']:#08x} ERROR {img['error']}")
            continue
        if img.get("format") == "v2":
            lines.append(
                f"@{img['offset']:#08x} {img['extracted']} V2 len={img['computed_length']}"
                f" entry={img['entry']:#010x} irom_len={img['irom_length']}"
                f" irom_mapped={img['irom_mapped_addr']:#010x}"
                f" checksum_valid={img['checksum_valid']} sha256={img['sha256']}")
            for s in img["second_image"]["segments"]:
                lines.append(f"  seg load={s['load_addr']:#010x} len={s['length']}"
                             f" sha256={s['sha256']}")
            continue
        lines.append(
            f"@{img['offset']:#08x} {img['extracted']} len={img['computed_length']}"
            f" entry={img['entry']:#010x} segs={img['segments_count']}"
            f" checksum_valid={img['checksum_valid']} sha256={img['sha256']}")
        for s in img["segments"]:
            lines.append(f"  seg load={s['load_addr']:#010x} len={s['length']}"
                         f" fileoff={s['file_offset']:#x}")
    t = report["tail_16k"]
    lines.append(f"tail16k all_ff={t['all_ff']} sha256={t['sha256']}")
    nonblank = [b for b in blocks if not b["all_ff"]]
    lines.append(f"middle blocks: {len(blocks)} total, {len(nonblank)} non-blank")
    for b in nonblank:
        lines.append(f"  nonblank {b['start']:#08x}-{b['end']:#08x} sha256={b['sha256']}")
    (outdir / "layout.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
