# drvkit — userspace drivers on `#P`

`drvkit` is the small library between a device and a 9P service. A driver
finds its node in `#P`, maps the register window, waits for interrupts, takes
DMA from the kernel's arena, and publishes files; the kit does the boilerplate
so the driver is its register logic and its file semantics.

It is **linked into each driver**, not reached through a service. `segattach`,
a read from `#P/<node>/wait` and a write to `#P/dma/alloc` are cheap, and a
driver that cannot reach the kernel's grants is not a driver. The current
consumers are `sys/src/cmd/uartfs/uartfs.c` (the worked example) and, in
progress, the storage and network drivers.

```
sys/include/drv.h          the API
sys/src/cmd/drvkit/drv.c   the implementation
sys/src/cmd/uartfs/        the worked example
```

## The shape of a driver

```c
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "drv.h"

static Drv dev;
static DrvFile files[] = {
	{ "data",   0666, &dev, fsread, fswrite },
	{ "status", 0444, &dev, fsread, nil },
};

void
main(void)
{
	if(drvfind(&dev, "uart", 1) < 0 || drvmap(&dev) < 0)
		drvfail("uartfs: no uart: %r");

	drvsrvpost(drvsrv("uart", files, nelem(files)));
	drvhealthrun("uartfs");
}
```

`drvfind` reads `class`, `compatible`, `reg`, `size`, `irq` and `console` from
every `#P` node and picks the first match; `wantconsole` prefers the device
the firmware marked as its console. `"*"` matches any node with registers.
`drvfindexact` skips the scan when the board's node name is known.

Handlers get the `aux` pointer from their `DrvFile`, and `drvfile(r)` returns
the file the request is on, so one read handler can serve several files.

## What replaces what

| 9front kernel driver | drvkit |
|---|---|
| `pcimatch`, BAR decode | `drvfind` / `drvfindexact` (device tree today; PCI config space is not in `#P` yet) |
| `KADDR(BAR)`, `vmap` | `drvmap`, `drd`, `drw` |
| `intrenable`, `intrdisable` | `drvwait` on `#P/<node>/wait`; the driver still acks the device |
| `dmaalloc`, cache maintenance | `drvdma`, `drvdmafree`, `drvdmacache`, `drvpa` |
| `addethercard`, `sdif`, `vga` | a `Drvsrv` with `ctl`/`data`/`stats` |
| `print`/`panic` | `drvlog`/`drvfail` |
| driver health in the kernel | `drvhealthrun`, `drvhealthreport`, `drvhealthstop` |

## Porting checklist

1. **Keep the register code verbatim.** Offsets, bit names, errata workarounds
   and the reset sequence are the valuable part and do not change.
2. **Replace discovery.** Delete `pcimatch`/`pcienable`; call `drvfind` with
   the class name from `sys/src/9/aarch64/fdt.h`. If the device has several
   instances, call `drvfindexact` per node name from the board's DTB.
3. **Replace mapping.** `KADDR`/`vmap` become `drvmap`; register accesses
   become `drd`/`drw`.
4. **Replace interrupts.** `intrenable` becomes one `read` on the wait file.
   The handler still has to clear the device's status and, on shared lines,
   ack the controller.
5. **Replace DMA.** `dmaalloc` becomes `drvdma`; the return value is the user
   address and the out-parameter is the physical address to program. Use
   `drvdmacache` around ownership changes on non-coherent devices.
6. **Replace the kernel interface.** `addethercard`, `sdif` and `vga` have no
   userspace equivalent; the driver becomes a 9P service. That is where the
   work is: a NIC needs a packet interface, storage needs a block interface,
   display needs the draw protocol.
7. **Add health.** `drvhealthrun` keeps the driver in `drvman`'s table, so it
   can be restarted and watched like the kernel drivers were.

## Limits worth knowing before porting

- **A wait is a wakeup, not an interrupt handler.** There is no per-IRQ
  context, no EOI, and one read per interrupt. That is right for sensors,
  RTCs, ECs, PHYs and UARTs. For a NIC or NVMe it is the wrong shape: those
  need a shared-memory ring and one wakeup per batch, or the 9P round trip
  dominates. Reuse the register tables; write the ring.
- **The DMA arena is shared, not isolated.** Any driver can program any
  address in it. An IOMMU domain per device is the safety story that makes
  userspace drivers worth their cost; `rkiommu` exists, wiring grants through
  it is not done.
- **PCI is missing on amd64.** `#P` enumerates from the device tree, which is
  why this kit is ready for the RK3566 and for virtio on ARM, and needs a
  `#pci` grant (or a devpnp extension) before PC drivers can find their BARs.
- **One process per device.** The 9P server forks, so open the wait file
  lazily inside the process that waits (the kit does). Do not cache
  descriptors across `drvsrvpost`.

## Testing

The arm64 boot runs `uartfs` and `drvman` and prints:

```
HARVEY-ARM64 UARTFS PASS
HARVEY-ARM64 DRVMAN uart PASS
HARVEY-ARM64 DRIVER-MANAGER PASS
```

From the guest's `rc`, the service is a normal 9P server:

```
mkdir /mnt/u
mount /srv/uart /mnt/u
cat /mnt/u/status        # pl011
ls /mnt/u                # data status ctl
```

`util/arm64/build_userland.py` compiles `drv.c` into every driver built by
`build_fs`, so a new consumer only adds its source to that call.
