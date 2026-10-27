# o9xhci driver interface

Level: **L2 controller/bus** (xHCI) with an initial **L3 UAC1** path.

See `doc/drivers/four-level-model.md`.

## Transport

`binding.c` opens one exclusive hardware transport:

```text
#X/N
```

The transport returns a text line:

```text
xhci-transport/1 <dma-base-hex> <dma-size-hex> <mmio-size-hex>
```

All controller calls are serialized. MMIO is at `0x100000 + offset`; DMA is at
`0x100000000 + address - dma-base`.

## XHC1 requests

Request header: 24 bytes.

```text
0..3    "XHC1"
4..7    operation
8..11   handle (slot)
12..15  endpoint or interface
16..19  count / alternate setting
20..23  reserved, must be 0
24..    optional payload
```

Reply header: 16 bytes, then optional payload.

```text
0..3    result (negative error or byte count)
4..7    handle
8..11   payload length
12..15  last xHCI completion code
```

Operations:

| Op | Name | Inputs | Result |
| --- | --- | --- | --- |
| 1 | PORT | handle = root port | 4-byte PORTSC |
| 2 | ATTACH | handle = root port | new handle |
| 3 | DETACH | handle | 0 |
| 4 | CONTROL | handle, setup+data | IN bytes |
| 5 | CONFIGURE | handle, configuration descriptor | 0 |
| 6 | TRANSFER | handle, endpoint, data | byte count |
| 7 | CLEAR_HALT | handle, endpoint | 0 |
| 8 | NOOP | none | 0 |
| 16 | SET_INTERFACE | handle, interface, alternate | 0 |
| 17 | ISOC | handle, endpoint, count, frame | byte count |
| 18 | FRAME | none | 4-byte MFINDEX |
| 19 | UAC1_PARSE | configuration descriptor | 9 words |

`UAC1_PARSE` returns:

```text
iface, alt, ep, packet, interval,
channels, subframe, bits, rate
```

## Ownership

- L2 owns xHCI state, rings, DMA and the root-port device table.
- L3 owns the interface configuration for its class.
- The service wrapper must never expose raw MMIO or DMA.
- A failed transport poisons the controller; `o9xhci_stop()` releases it.

## Known limitations

- USB hubs and non-root devices are not implemented.
- Transfer is one outstanding operation per controller.
- Isochronous capture is a probe path, not a buffered service.
- Timeout handling is conservative and unsuited to idle interrupt reads.
- Board transport: the kernel `#X` device was removed. A user-space transport
  over `#P` MMIO/DMA grants is required for on-board use.
