# `#P` -- the platform bus

`#P` is how a user-space driver learns what the firmware found and gets
the resources it needs.  It is produced from the device tree at boot and
is the only supported way for a driver to reach a controller: there are no
kernel drivers for these devices, and no private mappings.

## Layout

```
#P/<node>/reg            first MMIO base              (0444)
#P/<node>/size           first MMIO window size       (0444)
#P/<node>/irq            GIC interrupt id             (0444)
#P/<node>/irqflags       trigger flags                (0444)
#P/<node>/compatible     device-tree compatible       (0444)
#P/<node>/class          uart, i2c, mmc, usb, ...     (0444)
#P/<node>/family         serial, soc, mmc, usb, ...   (0444)
#P/<node>/console        firmware console flag        (0444)
#P/<node>/wait           blocks until one interrupt   (0444)
#P/<node>/claim          take ownership               (0666)
#P/<node>/owner          current owner, empty if free (0444)
#P/<node>/release        drop ownership               (0666)
```

`<node>` is the device-tree node name, e.g. `mmc@fe310000`.  The path is
built literally: `snprint(path, ..., "#P/%s/%s", node, file)`.

`wait` returns once per interrupt: read it before touching the device's
status registers, then acknowledge in the device.  Opening it enables the
GIC line.

## One owner per device

A driver claims its node before touching registers:

```c
fd = open("#P/mmc@fe310000/claim", OWRITE);
write(fd, "lkld", 4);
```

* the first claim wins;
* a second claim with a different name fails with `device already claimed`;
* the same name may claim repeatedly (idempotent, useful after a restart
  inside one service);
* `release` requires the matching name.

The owner string is chosen by the service, not the kernel.  Ownership is
kernel state, so it survives process restarts and is visible to every
process through `owner`.

## Platform identity

```
#P/platform             board and early ranges       (0444)
```

A single text file with one `<key> <value...>` line:

```
board rk3566
compatible rockchip,rk3566-roc-pc
ram 0x40000000 0x80000000
bootarchive 0x50000000 0x100000
teeshm 0x9200000 0x200000
scm 0x10f000 0x1000
framebuffer 0x5c000000 0x1000000 720 1600 2880 32
```

`board` is the SoC family selected from the root `compatible`: `qemu-virt`,
`rk3566`, `rk3568`, `rk3588`, `bengal`, or `unknown`.  Ranges are base and
size; a zero
base means the image did not provide one.  Drivers and init should select
board-specific sequences from this file, not from the UART address or other
accidentals.

If the firmware passes no `/memory` node, the kernel falls back to
`/chosen/harvey,ram`; `mk-sdimg.py --ram BASE:SIZE` and
`pack_boot.py --ram BASE:SIZE` stamp it.  A real `/memory` node always wins.

## DMA

```
#P/dma/base              arena physical base
#P/dma/size              arena size
#P/dma/alloc             write "<user-va> <len>", read back "<pa> <len>"
#P/dma/free              write "<user-va> <len>"
#P/dma/clean|inval|cleaninval   cache maintenance, "<user-va> <len>"
```

A driver maps the arena once with `segattach(0, "dma", 0, size)` and then
asks the kernel for aligned chunks, so two drivers cannot be handed the
same page.  DMA addresses are physical and must stay below 4 GiB while a
32-bit ADMA engine is in use.

## Worked example

The RK3566 eMMC controller on the Orange Pi 3B:

```
reg         0xfe310000
size        0x10000
irq         51
compatible  rockchip,rk3568-dwcmshc
class       mmc
family      mmc
```

That is the `sdhci-of-dwcmshc` variant: the generic `sdhci` core plus the
Rockchip glue (clock/reset, DLL, PHY tuning).  It is the first M2 target.

Mapping rule: `segattach` grants the **2 MiB block** that contains the
device, not the device window.  The window starts at
`base & (2*MiB - 1)` inside the grant:

```c
grant = (size + 0x1fffff) & ~0x1fffff;
m = segattach(0, node, 0, grant);
m += base & 0x1fffff;
```

`lklhost_mapdev()` applies this so `lkl_ops->ioremap()` and
`iomem_access()` can serve the guest driver out of the mapping.

Verified on the board: claim `mmc@fe310000`, map it, and read the SDHCI
host version register at `0xfe`; it returns `0x0005`.  The probe runs
before `lkl_init()` so a mapping failure is reported without the kernel.

Known quirk: `open()` with a path literal in `.rodata` fails with
`empty file name` in this 14 MiB binary, while a stack copy of the same
string works.  Paths are therefore built into stack buffers (`snprint`).

## Not done yet

* Family routing (`#P/<family>/<device>/...`) is not implemented; `family`
  is a file for now.  The flat node path is kept so existing consumers
  (`drvkit`, `o9xhci`, `uartfs`) keep working.
* `claim` is a contract, not yet enforced on `wait` or on `segattach`.
