# Pinmux and GPIO — USB VBUS

## VBUS pins

| Signal | Pin | Function | Pull | Consumer |
| --- | --- | --- | --- | --- |
| `USB_HOST_PWREN_H` | GPIO0_A6 | GPIO (func 0) | none | `vcc5v0_usb_host` |
| `USB_OTG_PWREN_H` | GPIO0_A5 | GPIO (func 0) | none | `vcc5v0_usb_otg` |

The board DTS pinctrl entries are:

```dts
usb_host_pwren_h: usb-host-pwren-h {
    rockchip,pins = <0 RK_PA6 RK_FUNC_GPIO &pcfg_pull_none>;
};

usb_otg_pwren_h: usb-otg-pwren-h {
    rockchip,pins = <0 RK_PA5 RK_FUNC_GPIO &pcfg_pull_none>;
};
```

## Register locations

| Block | Base | Register | Notes |
| --- | --- | --- | --- |
| PMUGRF | `0xfdc20000` | `0x0004` | GPIO0_A0..A7 mux |
| GPIO0 | `0xfdd60000` | `0x0000` | SWPORT_DR (output data) |
| GPIO0 | `0xfdd60000` | `0x0008` | SWPORT_DDR (direction, 1 = output) |
| PMUCRU | `0xfdd00000` | `0x0184` | PMUGATE_CON1; bit 9 = PCLK_GPIO0, bit 10 = DBCLK_GPIO0, 0 = enabled |

GPIO0_A5 is in the lower half of the PMUGRF mux register:

```text
PMUGRF 0x0004, bits 4..7   = GPIO0_A5 mux
PMUGRF 0x0004, bits 8..11  = GPIO0_A6 mux
```

Both are function 0 (GPIO).

## RK356x V2 GPIO write mask

The GPIO controller uses the RK356x V2 layout: the upper 16 bits of the
register are a write-enable mask. A plain write of `0x40` is ignored. To write
one bit, set its mask bit in the upper half as well.

Set GPIO0_A6 high without disturbing other pins:

```c
/* DR: set bit 6, mask only bit 6 */
*(volatile u32 *)(gpio0 + 0x0000) = (1U << 6) << 16 | (1U << 6);

/* DDR: make bit 6 an output, mask only bit 6 */
*(volatile u32 *)(gpio0 + 0x0008) = (1U << 6) << 16 | (1U << 6);
```

Set GPIO0_A5 high:

```c
*(volatile u32 *)(gpio0 + 0x0000) = (1U << 5) << 16 | (1U << 5);
*(volatile u32 *)(gpio0 + 0x0008) = (1U << 5) << 16 | (1U << 5);
```

A read-modify-write that writes all 16 bits is also valid if the current
value is preserved:

```c
v = *(volatile u32 *)gpio0 & 0xffff;
v |= 1U << 6;
*(volatile u32 *)gpio0 = 0xffff0000U | v;
```

The second form is what `devxhci.c:gpioout()` uses.

## Mux write mask

PMUGRF also uses a write-mask scheme. To set GPIO0_A6 to function 0:

```c
/* mask bits 8..11, value 0 */
*(volatile u32 *)(pmugrf + 0x0004) = (0xfU << 8) << 16 | 0;
```

For GPIO0_A5:

```c
*(volatile u32 *)(pmugrf + 0x0004) = (0xfU << 4) << 16 | 0;
```

The observed reset value of `PMUGRF 0x0004` is `0x1001`, with bits 4..11
already zero, so the pins are GPIO by default. The GPIO clock gate must still
be enabled before writing DDR/DR.

## Clock gate

`PMUCRU 0x0184` is `PMUGATE_CON1`. Bits 9 and 10 gate `PCLK_GPIO0` and
`DBCLK_GPIO0`. The RK356x gate convention is 0 = enabled:

```c
*(volatile u32 *)(pmucru + 0x0184) = (3U << 9) << 16 | 0;
```

Observed value after U-Boot/Linux: `0x00000000`.

## Failure mode seen during bring-up

Writing `0x0000c060` directly to `0xfdd60000`/`0xfdd60008` had no effect; the
pins stayed inputs and VBUS stayed off. The readable value remained
`0x0000c000`. The missing upper-half write mask was the bug. With
`0xffffc060` (or a single-bit mask such as `0x00400040`) the direction and
output bits changed and the USB ports powered.
