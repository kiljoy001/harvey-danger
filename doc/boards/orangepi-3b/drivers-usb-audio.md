# USB audio input driver — design and status

Target: FIFINE K670 on the Orange Pi 3B blue USB3 port.

## Device

```text
VID 3142, PID 0001
USB 1.10, full speed, EP0 MPS 16
UAC1: interface 2 alt 1, isochronous IN 0x82
      wMaxPacketSize 200, bInterval 1
      48 kHz, 16-bit, 2 channels
```

## Engine path

`sys/src/cmd/o9xhci/xhci.c` now supports:

- `xhci_validate_config()` accepts isochronous endpoints and audio
  alternate settings.
- `xhci_configure()` configures UAC1 alt 1 endpoints and issues
  `SET_INTERFACE` for each audio streaming interface.
- `xhci_setinterface()` sends the standard `SET_INTERFACE` request.
- `xhci_frame()` reads the xHCI microframe index.
- `xhci_isoc()` submits a Normal TRB with the isochronous frame ID and
  waits for the transfer event.
- `xhci_uac1_parse()` finds UAC1 Format Type I (channels, subframe size,
  bit depth, sample rate) and the matching isochronous IN endpoint.

## Probe

`/boot/test-audio`:

1. starts the xHCI engine on `#X/0`;
2. attaches port 1;
3. reads the 235-byte configuration descriptor;
4. parses the UAC1 format;
5. configures the device and selects alt 1;
6. reads 16 isochronous frames from endpoint `0x82`.

Expected output:

```text
audio: UAC1 iface 2 alt 1 ep 82 packet 200 interval 1 2ch/16b/48000Hz
audio: frame ... got 192 ...
AUDIO PASS
```

A successful read of 192 bytes is one millisecond of 48 kHz/16-bit/stereo
PCM. The endpoint packet is 200 bytes; the extra space is isochronous
overhead.

## What is still missing

The kernel `#X` transport was removed when the board drivers moved to user
space. `test-audio` therefore needs a user-space xHCI transport:

1. discover the USB controller and PHY from `#P`;
2. map MMIO with `segattach`;
3. obtain a DMA arena with kernel-granted physical addresses;
4. deliver the controller IRQ or poll it;
5. instantiate `Xio` callbacks over those grants.

The board register values needed for that transport are in `../usb.md`,
`../pinmux.md`, `../clocks.md`, and `../refs/trm-usb2phy.md`.

## Service shape

The eventual driver should export a 9P audio service, for example:

```text
/srv/audio/ctl      format, start, stop, gain
/srv/audio/data     read PCM frames
/srv/audio/status   underruns, frame count, rate
```

Clients should never see `#P`, `#X`, or the xHCI engine.
