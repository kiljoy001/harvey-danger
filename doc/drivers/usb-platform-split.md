# USB platform split

The xHCI driver is not allowed to own board-level USB resources. The board
contract is split into three layers.

## L1: `usbpower`

`/boot/usbpower` is a platform service. It owns:

- PMU power domains
- CRU clocks and resets
- USB2 PHY0/PHY1 GRFs
- USB2 PHY tuning
- the shared USB VBUS enable

VBUS is delegated to the existing `gpiofs` service. `usbpower` mounts
`/srv/gpio` and writes GPIO0_A5/A6, rather than mapping GPIO0 itself.

The PMU/CRU/PHY mappings are one-shot. `usbpower` programs the registers,
detaches the mappings, and posts:

```text
/srv/usbpower/status   "ready" or "off"
/srv/usbpower/ctl      "on" or "off"
```

## L2: `o9xhci`

`o9xhci` owns only:

- DWC3 controller mode setup
- controller MMIO
- DMA arena
- xHCI command and transfer engine

It waits for `/srv/usbpower/status` to say `ready`, then maps the controller
and DMA. It does not map PMU, PMUCRU, CRU, GPIO, PHY GRF, or PHY registers.

## L3: UAC1

The UAC1 descriptor parser, alternate-setting selection, and isochronous
stream remain above xHCI.

## Boot order

`drvman` launches services in registry order:

```text
uart
gpio
i2c
usb (usbpower)
```

The USB platform service therefore starts after `gpiofs`, which it uses for
VBUS. `o9xhci` starts only after `usbpower` is ready.

## Why

The previous draft put PMU, CRU, GPIO, PHY, DWC3, and xHCI in one process. It
also bypassed `gpiofs`. That caused:

- unnecessary segment pressure
- duplicated board knowledge
- no reuse by EHCI/OHCI or a second USB controller
- the wrong service owning USB health and power

The split keeps the microphone driver from becoming the board bring-up
driver.
