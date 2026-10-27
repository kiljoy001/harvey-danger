# The four-level driver model

This is the contract for Harvey board support. It keeps the kernel small,
makes drivers replaceable, and gives third-party driver authors a stable
target.

```text
L1  Resource       what hardware exists, what may be mapped
L2  Controller     one bus/controller: xHCI, EHCI, PCIe, I2C, SPI, ...
L3  Class/device   one device or interface class: HID, UAC1, storage, ...
L4  Service/URI    what clients open: /srv/audio, usb://0, /dev/mic
```

Kernel mechanisms (`#P` grants, IRQ entry/EOI, DMA/IOMMU, IPC rings) are
outside the four levels. They are not drivers; they are what the levels are
allowed to use.

## Level summary

| Level | Knows about | Must not know about | Exports |
| --- | --- | --- | --- |
| L1 Resource | SoC/board topology, MMIO, IRQ, DMA, node metadata | device protocols, class semantics | `#P/{family}/{n}` |
| L2 Controller | one controller's registers, rings, bus enumeration, pipes | audio/HID/storage semantics | child devices and pipe operations |
| L3 Class | one interface/class protocol | controller registers, other classes | a 9P service or a typed pipe |
| L4 Service | names, mounts, policy | hardware, DMA, controller state | `/srv/...`, `/dev/...`, URIs |

## L1 — Resource

Purpose: publish hardware facts and grant access. It is read-only discovery
plus capability grants.

Example:

```text
#P/usb/0/
    node        fe8a0000.usb2phy
    class       usb
    compatible  rockchip,rk3568-usb2phy
    reg         0xfe8a0000
    size        0x10000
    irq         19
    irqflags    4
    mmio        usb/0
    dma         none
```

Rules:

- L1 never programs a device.
- L1 is the only place allowed to describe MMIO, IRQ and DMA grants.
- L1 records are kernel-owned and read-only to clients.
- A driver gets only the grant for its device/family; no ambient `#P`.
- Numbering is stable within a boot; `node` and aliases identify the revision.
- DMA is a reserved range or an IOMMU mapping, not arbitrary physical memory.

## L2 — Controller/bus

Purpose: drive one controller and enumerate the bus behind it. It turns
hardware into devices, interfaces and pipes.

Examples:

- xHCI controller, USB core, hubs, device/interface/endpoint model.
- EHCI/OHCI companion controllers.
- PCIe root complex.
- I2C/SPI controller and address decoding.

L2 owns:

- MMIO mapping, controller reset, rings, DMA descriptors.
- IRQ handling or polling.
- Bus enumeration, addresses, configurations, interfaces, endpoints.
- Pipe operations: control, bulk, interrupt, isochronous.
- Hotplug and detach.
- Power and port state for its bus.

L2 must not know what a microphone, keyboard or disk is. It exposes a generic
device/interface/pipe interface. A microphone and a keyboard are both “an
interface with endpoints” to L2.

## L3 — Class/device

Purpose: understand one protocol and make the device useful.

Examples:

- HID: keyboard, mouse, gamepad.
- UAC1/UAC2: microphone, speaker, headset.
- Mass storage / UAS.
- CDC-ACM.
- UVC.
- GPIO expander, I2C sensor, PMIC.

L3:

- binds by class/subclass/protocol, not by VID/PID unless a quirk requires it;
- uses L2 pipes and standard requests;
- handles class-specific descriptors, alternate settings, feedback, errors;
- exports a stable 9P service or typed interface;
- may be composite: one physical device can host audio + HID interfaces, each
  bound by a different L3 driver.

L3 must not map controller registers, program DMA rings, or handle bus
enumeration. If it needs a new primitive, that belongs in L2.

## L4 — Service and URI

Purpose: what clients use. It hides the lower levels entirely.

Examples:

```text
/srv/audio           PCM capture/playback
/srv/hid             keyboard/mouse events
/srv/storage         block device
/srv/usb             USB topology and device management
/dev/mic             stable name for the default microphone
usb://0/audio        URI for the audio function of USB instance 0
audio://default      URI for the default audio device
```

L4:

- maps names to services and instances;
- enforces policy and permissions;
- can provide defaults and aliases;
- may be implemented as a user-space URI resolver or as namespace composition;
- never exposes MMIO, DMA, IRQ or controller internals.

The URI layer is optional policy. The kernel only sees numbered instances and
9P services, never URI syntax.

## Example: FIFINE K670 on the Orange Pi 3B

```text
L1  #P/usb/0          xHCI controller fd000000, PHY, IRQ, DMA grant
L2  /srv/usb          xHCI + USB core: root port, interface 2 alt 1,
                       isochronous IN pipe 0x82
L3  /srv/audio        UAC1 parser: 48 kHz, 16-bit, 2 channels
L4  /dev/mic          default microphone
    usb://0/audio     URI for the same device
```

Client:

```c
fd = open("/dev/mic", OREAD);
n = read(fd, pcm, sizeof pcm);
```

The client never sees `#P`, xHCI, UAC1 or the ring.

## Lifecycle and ownership

- L1 creates a resource record at boot or hotplug.
- L2 claims the controller grant, enumerates, and publishes child interfaces.
- L3 claims an interface, configures it, and exports a service.
- L4 publishes names and defaults.
- On unplug or crash, ownership unwinds in reverse order.
- A crashed L3 must not take down L2 or the kernel; L2 detaches the device.
- A crashed L2 must be restartable; L1 grants remain valid.

## Security rules

- L3 never gets MMIO/DMA/IRQ directly.
- L2 gets only its controller’s grants.
- DMA must be bounded by IOMMU or a reserved range.
- A class driver cannot DMA into another device’s memory.
- The URI/namespace layer enforces who may open which service.
- No ambient `#P`: a driver’s namespace contains only its grant.

## Testing and qualification

Every driver should ship with:

- a descriptor/parser unit test;
- a fake transport or QEMU test where possible;
- a board test procedure;
- a support-matrix entry with board revision;
- known limitations and error behavior.

For L1/L2, register dumps and scope captures are part of the test evidence.
For L3, class conformance tests and real-device captures are the evidence.
For L4, namespace/URI tests must not require hardware.

## Documentation checklist per driver

```text
name, version, license
level (L1/L2/L3/L4)
device/class IDs
resources: MMIO, IRQ, DMA
descriptors and formats
state machine and error recovery
9P/URI interface
board revisions tested
known quirks
firmware requirements
security assumptions
```

## Anti-patterns

- A class driver that maps controller registers.
- A controller driver that knows audio or HID semantics.
- A kernel driver for a device that can be user space.
- `#P` exposing arbitrary physical memory.
- A URI parser inside the kernel.
- One driver that handles every USB class.
- A native driver that silently depends on a Linux BSP behavior without
  documenting it.

## Support policy

```text
on-board SoC/soldered devices   native L2/L3 drivers, supported per board
common USB classes              native where practical, otherwise Linux host
add-on devices                  best effort, third-party driver, or paid port
unsupported hardware            explicitly listed
```

The four levels are the product boundary. If a device is supported, its
levels and interfaces are documented. If it is not, the support matrix says so
instead of pretending the controller driver is all that is needed.
