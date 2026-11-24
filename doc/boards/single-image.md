# One image for several arm64 boards

Harvey's goal for adoption is one OS image per ISA, not one per board.
The firmware still comes from the board vendor; the image below carries
the kernel, the userland archive and a DTB per supported board.

## What the kernel takes from the DTB

The parser in `sys/src/9/aarch64/fdt.c` needs a usable console, a GIC and
an armv8 timer.  A board DTB normally provides those.  It also reads:

| Node / property | Use |
| --- | --- |
| `/memory` | RAM banks (preferred) |
| `/chosen/harvey,ram` | RAM base/size when the firmware passes no `/memory` |
| `/chosen/stdout-path` | console UART |
| `/chosen/harvey,bootarchive` | boot archive base/size |
| `/chosen/harvey,teeshm` | OP-TEE shared memory |
| `/chosen/harvey,scm` | SCMI shared memory |
| `simple-framebuffer` | early framebuffer console |
| root `/compatible` | board class (`rk3566`, `rk3568`, `rk3588`, ...) |

The `harvey,*` properties are optional; the kernel has board fallbacks
for the current RK356x boards.  Stamping them makes the image explicit
and removes the guesses.

User space sees the same summary in `#P/platform`, one `key value...`
line per field (`board`, `compatible`, `ram`, `bootarchive`, `teeshm`,
`scm`, `framebuffer`).  Drivers should branch on that, not on a UART
address.

## Build the image

```sh
python3 util/arm64/mk-sdimg.py \
    --image build/arm64-kernel/harvey.img \
    --sdk /tmp/opencode/sdk-board.boot \
    --dtb orangepi-3b=/home/scott/tftp/orangepi-3b-v1.1.dtb \
    --dtb rk3588-evb=/path/rk3588-evb.dtb \
    --default-dtb orangepi-3b \
    --sdk-load 0x50000000 \
    --ram 0x40000000:0x80000000 \
    --teeshm 0x9200000:0x200000 \
    --out /tmp/opencode/sd.img
```

`--ram`, `--teeshm` and `--scm` are `BASE:SIZE`; omit any that the board
does not need.  The DTBs are copied, stamped, and never modified in
place.  `--parts-dir` also splits the image into U-Boot TFTP chunks.

The FAT partition then holds:

```
Image              kernel, ARM64 booti format
sdk.tar            userland boot archive
board.dtb          alias for the default board
dtbs/<name>.dtb    one per --dtb
boot.cmd           U-Boot script source (multi-board images)
boot.scr           the same, when mkimage was available
```

## Boot it

Write the image from U-Boot:

```
mmc dev 1
mmc write 0x50000000 0 <blocks>
```

Then either use the generated script:

```
setenv harvey_dtb dtbs/orangepi-3b.dtb
fatload mmc 1:1 0x40080000 boot.scr
source 0x40080000
```

or type the equivalent:

```
fatload mmc 1:1 0x40080000 Image
fatload mmc 1:1 0x48000000 dtbs/orangepi-3b.dtb
fatload mmc 1:1 0x50000000 sdk.tar
booti 0x40080000 - 0x48000000
```

The SDK load address must match `harvey,bootarchive` (and `--sdk-load`).
The script defaults to `--default-dtb`; change `harvey_dtb` to pick
another board from the same card.

## Testing before hardware

```sh
python3 util/arm64/test_fdt.py
python3 util/arm64/test_sdimg.py
build/arm64/test-fdt /path/to/board.dtb inspect
```

`test-fdt ... inspect` prints the selected board class and every device
the kernel would discover.  A vendor/U-Boot DTB may have no `/memory`;
stamp `harvey,ram` first or add a synthetic node before inspecting.

## Adding a board

1. Add its DTB to the image with `--dtb NAME=PATH`.
2. Run `test-fdt PATH inspect`; confirm the UART, GIC, timer, RAM and the
   devices the kernel drives.
3. If the root compatible is a new SoC family, add the prefix to
   `boardid()` and the peripheral compatible strings to `classtab`.
4. If it needs an undocumented clock/reset/power sequence, add a
   `platform_<soc>.c` and one board-selected call; do not put the
   sequence in `main.c` or `boot.c`.
5. Stamp `--ram`, `--teeshm` and `--scm` if the firmware DTB lacks them.
6. Boot it and compare `/dev/pnp` and `#P/platform` with the inspect
   output.

## Limits

- The firmware/U-Boot is still per board; the image assumes the board's
  firmware can load `Image` and start it with a DTB in `x0`.
- `devxhci` and `ethergmac` resolve their addresses from the DTB now but
  have not run on the Orange Pi yet.
- The kernel manages the RAM bank that contains the image.  Multi-bank
  RAM (high memory above a hole) is not merged yet.
