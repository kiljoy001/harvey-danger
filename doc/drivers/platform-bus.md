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
#P/<node>/wait           blocks until one interrupt   (0444, holder only)
#P/<node>/claim          hold the device while open   (0666)
#P/<node>/owner          the holder, empty if free    (0444)
#P/<node>/regs           one register per read/write  (0666, holder only)
```

`<node>` is the device-tree node name, e.g. `mmc@fe310000`.  The path is
built literally: `snprint(path, ..., "#P/%s/%s", node, file)`.

`wait` returns once per interrupt: read it before touching the device's
status registers, then acknowledge in the device.  Opening it enables the
GIC line.

## One owner per device

A driver claims its node before touching it, and holds it for as long as
it keeps the claim file open:

```c
fd = open("#P/mmc@fe310000/claim", OWRITE);
write(fd, "emmc", 4);		/* keep fd open: closing releases */
```

* **The open file is the claim.**  Closing it, or exiting, releases the
  device; a child holds it through the file table it inherits.  A name
  proves nothing: writing someone else's name gets nothing.
* **Only a holder** may map the device's window (`segattach` by node name),
  open its `wait`, or use its `regs`.  Everyone may read the discovery
  files.
* **Exclusive by default.**  A second claim fails with `device already
  claimed` while one is held.  `"<name> shared"` claims coexist with other
  shared claims, under any name, and keep exclusive ones out: it is for
  blocks several drivers touch on purpose (the clock and reset unit, the
  GRFs, GPIO banks used for pins, a PMIC arbiter two LKL guests drive).
  `drvkit`, `lklboot` and the native drivers claim those classes shared
  and the controller they drive exclusively; an LKL family marks a device
  it shares with another family with a trailing `+` in
  `util/arm64/lkl-families.json`.
* **A restarted service** reclaims once the old process is gone, or takes
  the claim file over through `/srv` if the old one posted it.

`owner` shows the first holder's name, with ` shared` for a shared claim.

## Registers through files

`#P/<node>/regs` reads or writes one aligned 4- or 8-byte register at its
offset in the device's first window: `pread(fd, &v, 4, 0xff0)`.  It needs
no mapping, so a driver (or rc, with `dd`) can program a controller
entirely through 9P, and a bus abort on the access (a powered-down or
unclocked block) comes back as `regs: bus error` instead of a fault loop.
The kernel makes the access on its own page table, with interrupts off:
an asynchronous error, as from a posted write the interconnect refuses
later, is not caught.  Mapping the window remains the fast path for a
driver that needs it.

## Platform identity

```
#P/platform             board and early ranges       (0444)
```

A single text file with one `<key> <value...>` line:

```
board rk3566
model Rockchip RK3566 OPi 3B
compatible rockchip,rk3566-roc-pc
ram 0x40000000 0x80000000
bootarchive 0x50000000 0x100000
teeshm 0x9200000 0x200000
scm 0x10f000 0x1000
framebuffer 0x5c000000 0x1000000 720 1600 2880 32
memory 0x40000000 0x80000000
```

`board` is the SoC family selected from the root `compatible`: `qemu-virt`,
`rk3566`, `rk3568`, `rk3588`, `bengal`, `sun50i-h6`, or `unknown`.  `ram`
is the bank that holds the kernel; the `memory` lines list every usable
bank.  Ranges are base and size; a zero base means the image did not
provide one.  Drivers and init should select board-specific sequences
from this file, not from the UART address or other accidentals.

If the firmware passes no `/memory` node, the kernel falls back to
`/chosen/harvey,ram`; `mk-sdimg.py --ram BASE:SIZE` and
`pack_boot.py --ram BASE:SIZE` stamp it.  A real `/memory` node always wins.

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
* A device's window is granted as the whole 2 MiB block that contains it,
  so a holder can reach the registers of other devices in the same block.
  Granting the window alone (page granular) changes the mapping rule
  above for every driver.
* Device-side isolation: a device can be programmed with any physical
  address.  Per-claim IOMMU contexts (rkiommu on the RK3566, an ARM SMMU
  driver for Qualcomm parts) are what close that.
