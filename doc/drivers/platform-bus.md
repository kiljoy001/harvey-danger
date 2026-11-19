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

## DMA

`#P` owns the DMA arena, a physically contiguous reservation below the
kernel's page allocator.  A driver gets memory from it as **chunks**, each
held through a file:

```
#P/dma/clone             open: a new chunk; the fd is now #P/dma/N/ctl
#P/dma/avail             "<free bytes> <largest free piece>"
#P/dma/N/ctl             write "alloc <size> [<align>] [cached|uncached]"
                         write "map"         maps the chunk into the caller
                         write "clean|inval|cleaninval <off> <len>"
                         read  "<N> pa <pa> len <len> <type> va <va>"
#P/dma/N/data            read and write the chunk through the kernel
```

```c
ctl = open("#P/dma/clone", ORDWR);
write(ctl, "alloc 65536 4096 cached\n", 24);
write(ctl, "map\n", 4);
pread(ctl, buf, sizeof buf - 1, 0);	/* pa for the device, va for us */
```

The rules:

* **The ctl is the capability.**  A chunk's files open by name only for a
  process that already holds its ctl open: the clone fd, or a copy a child
  inherited.  No process can map another's chunk, and the arena as a whole
  has no segment to attach.
* **A chunk is mapped only into who asks, and only the chunk.**  `map`
  attaches exactly the chunk's pages; a chunk whose address and size are
  2 MiB aligned is mapped with 2 MiB pages.
* **The memory type is chosen per chunk.**  `cached` (normal write-back,
  the default) for data the CPU works on, maintained with `clean`/`inval`
  around ownership changes; `uncached` (normal non-cacheable) for
  descriptor rings and buffers a device and the CPU share.  RAM is never
  mapped Device.  The kernel's own alias of the arena stays write-back, and
  an uncached chunk is cleaned and invalidated through it when handed out.
* **New chunks are zeroed**, so a chunk never shows its previous owner's
  data.
* **Lifetime is the files and the mappings.**  A chunk returns to the arena
  once its last file is closed and its last mapping gone: a driver that
  exits or is killed gives its memory back, never while it is still mapped.
  Nothing is freed by address.

`clean`, `inval` and `cleaninval` take offsets into the chunk.  DMA
addresses are physical and must stay below 4 GiB while a 32-bit ADMA engine
is in use.  The arena's top 2 MiB is the kernel's own (ethergmac's
descriptors) and is never handed out.

The OP-TEE static shared memory is not in the arena: its cache maintenance
is `#P/tee/clean`, `/inval` and `/cleaninval`, taking `"<pa> <len>"` inside
it.

This isolates chunks from the CPU's side only: a device can still be
programmed with any physical address.  Per-device IOMMU contexts are what
close that.

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
