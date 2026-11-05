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

## Not done yet

* Family routing (`#P/<family>/<device>/...`) is not implemented; `family`
  is a file for now.  The flat node path is kept so existing consumers
  (`drvkit`, `o9xhci`, `uartfs`) keep working.
* `claim` is a contract, not yet enforced on `wait` or on `segattach`.
