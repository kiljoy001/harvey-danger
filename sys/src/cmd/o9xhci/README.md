# Prism xHCI driver

`xhci.c` is the portable, polled xHCI engine. `binding.c` connects it to the
exclusive `#X/N` hardware transport, and `service.o9` exposes requests through
an Objective-9C view. The trusted kernel backend owns MMIO and pinned DMA
memory; USB command, event and transfer rings are managed in user space.

This is the **L2 controller/bus** level of the four-level model in
`doc/drivers/four-level-model.md`. The UAC1 path is an initial **L3 class**
driver. See `INTERFACES.md` for the request protocol.

This is a GPL-2.0-only implementation developed with the vendor Linux xHCI
and DWC3 sources in `../9rock/linux-rockchip/drivers/usb/host/` and
`../9rock/linux-rockchip/drivers/usb/dwc3/` as references. Rockchip board
setup also references the vendor PHY and clock drivers and Orange Pi DTs.

## Supported scope

- Directly attached root-port devices, standard USB speeds through SuperSpeed.
- Device addressing, control transfers, bulk/interrupt endpoints, and
  alternate-setting selection for USB Audio Class streaming.
- Full-speed attachment first reads eight device-descriptor bytes and learns
  `bMaxPacketSize0` before allowing larger control reads. Otherwise a device
  with a 64-byte EP0 can cause a babble error against the provisional MPS=8.
- Explicit attach/detach and endpoint stall recovery.
- UAC1 Format Type I parsing and isochronous IN transfer setup.

USB hubs, streams, automatic hotplug management, and general USB
audio/HID/storage class services are not implemented yet. The isochronous
transfer path is new and has parser/unit coverage; it still needs a board
transport and an audio service.
The Orange Pi EHCI/OHCI companion code is a board probe, not a complete USB2
transfer driver. Use the blue port for xHCI testing, including USB2 devices.

Timeouts currently poison the controller and revoke the transport. That is
conservative for DMA safety, but unsuitable for an ordinary idle interrupt read.
Pending reads and safe endpoint cancellation remain necessary for normal HID use.

## Validation

From the repository root:

```
sh util/quality/xhci.sh
python3 util/9vm/test_xhci.py
python3 util/9vm/test_xhci_guest.py
```

The native gate covers descriptor fuzzing, sanitizers, I/O fault injection,
ring wrap, mutation checks and full-speed EP0 bootstrap. QEMU tests use actual
emulated xHCI hardware for descriptor/configuration reads, bulk transfers,
interrupt reports and reconnects. They do not validate Rockchip PHY setup.

For the Orange Pi, build with `python3 util/arm64/build_kernel.py`, stage the
image on the existing router TFTP server at `192.168.2.1`, and boot it using
the sequence in `util/arm64/README.md`. At the Harvey rc prompt:

```
/boot/test-xhci
```

The probe reports device identity, EP0 packet size, configuration interfaces,
endpoint types and xHCI completion codes. `#X/0` is the board's USB3 host block;
`/boot/test-xhci1` probes the separate USB2-only DWC3 block at `#X/1`.
`/boot/test-audio` configures the UAC1 microphone and reads isochronous input
frames. No persistent flash or SD-card write is required for this workflow.

## Orange Pi microphone result (2026-10-23)

Tested on the real board with the microphone directly in the blue port:

- Before the EP0 bootstrap fix: Address Device succeeded; the first 18-byte
  GET_DESCRIPTOR failed with xHCI completion code 3 (Babble Detected).
- After the fix: the initial eight-byte read discovered EP0 MPS=16,
  Evaluate Context succeeded, and the full device/configuration reads passed.
- Device: USB 1.10, VID `3142`, PID `0001`, configuration length 235 bytes.
- Audio streaming: OUT `0x01` and IN `0x82`, isochronous, maximum packet 200,
  interval 1; both appear on alternate setting 1 of their respective interfaces.
- HID: interrupt IN `0x87`, maximum packet 16, interval 2.

This confirms device enumeration and control transfers on the board.

## UAC1 capture path

The engine now has:

- alternate-setting selection for audio streaming interfaces;
- isochronous endpoint contexts and isochronous IN transfers;
- UAC1 Format Type I parsing (`iface`, `alt`, endpoint, packet, interval,
  channels, subframe size, bit depth, sample rate).

`/boot/test-audio` exercises the path: it configures the device, prints the
UAC1 format, and reads isochronous IN frames from endpoint `0x82`.

This is not yet a complete audio service. It still needs a board transport
(the kernel `#X` transport was removed) and a 9P PCM reader that clients can
open.
