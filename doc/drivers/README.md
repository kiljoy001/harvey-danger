# Harvey driver documentation

Start here:

- [four-level-model.md](four-level-model.md) — the normative L1–L4 driver
  contract.
- [boot-and-service-model.md](boot-and-service-model.md) — QNX-style boot
  manifest, readiness and failure policy.
- [health-class.md](health-class.md) — shared DriverHealth class for all
  managed drivers.
- [usb-platform-split.md](usb-platform-split.md) — L1 USB power/PHY service
  versus the L2 xHCI controller driver.
- [support-matrix.md](support-matrix.md) — what is supported, partial or
  unsupported per board.
- [../boards/orangepi-3b/](../boards/orangepi-3b/) — reference board dossier
  with schematic/DTS/register maps and the UAC1 driver notes.

The model:

```text
L1  #P/{family}/{n}     resource discovery and grants
L2  controller/bus      xHCI, EHCI, PCIe, I2C/SPI, ...
L3  class/device        HID, UAC1/UAC2, storage, CDC, ...
L4  service/URI         /srv/..., /dev/..., usb://..., audio://...
```

Kernel mechanisms — address spaces, IRQ entry/EOI, timers, IPC, DMA/IOMMU
grants, `#P` itself — are not drivers. They are the substrate the four levels
use.

## Required per-driver documentation

Each driver directory should contain:

```text
README.md       level, scope, IDs, resources, limitations
interfaces.md   L2 pipes or L3 9P/URI interface
test.md         unit, fake/QEMU, board procedure
board/          board-revision notes and register observations
```

## Support policy

Native drivers are the supported path for on-board SoC and soldered devices.
Common USB classes are native where practical. Add-on devices are best effort,
third-party, or paid ports. Unsupported hardware is listed explicitly in the
board support matrix.

The four levels are not an implementation detail; they are the contract that
lets a driver be replaced without changing its clients.
