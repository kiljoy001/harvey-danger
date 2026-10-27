# USB-related regulators — Orange Pi 3B

## Fixed 5 V rails

From `rk3566-orangepi-3b.dtsi`:

| Rail | Control | Polarity | Input | Consumers |
| --- | --- | --- | --- | --- |
| `vcc5v0_sys` | none, always on | — | board 5 V input | `vcc5v0_usb_host`, `vcc5v0_usb_otg` |
| `vcc5v0_usb_host` | GPIO0_A6 | active high | `vcc5v0_sys` | USB2 PHY0 host, PHY1 host, PHY1 otg |
| `vcc5v0_usb_otg` | GPIO0_A5 | active high | `vcc5v0_sys` | USB2 PHY0 otg |

The regulators are `regulator-fixed`, 5.0 V, with `enable-active-high`.
They are enabled when a PHY consumer powers on; the board does not have a
separate always-on host VBUS rail.

## DTS

```dts
vcc5v0_usb_host: regulator-5v0-vcc-usb-host {
    compatible = "regulator-fixed";
    enable-active-high;
    gpios = <&gpio0 RK_PA6 GPIO_ACTIVE_HIGH>;
    pinctrl-names = "default";
    pinctrl-0 = <&usb_host_pwren_h>;
    regulator-name = "vcc5v0_usb_host";
    regulator-min-microvolt = <5000000>;
    regulator-max-microvolt = <5000000>;
    vin-supply = <&vcc5v0_sys>;
};

vcc5v0_usb_otg: regulator-5v0-vcc-usb-otg {
    compatible = "regulator-fixed";
    enable-active-high;
    gpios = <&gpio0 RK_PA5 GPIO_ACTIVE_HIGH>;
    pinctrl-names = "default";
    pinctrl-0 = <&usb_otg_pwren_h>;
    regulator-name = "vcc5v0_usb_otg";
    regulator-min-microvolt = <5000000>;
    regulator-max-microvolt = <5000000>;
    vin-supply = <&vcc5v0_sys>;
};
```

## PHY supply mapping

```dts
&usb2phy0_host {
    phy-supply = <&vcc5v0_usb_host>;
};

&usb2phy0_otg {
    phy-supply = <&vcc5v0_usb_otg>;
};

&usb2phy1_host {
    phy-supply = <&vcc5v0_usb_host>;
};

&usb2phy1_otg {
    phy-supply = <&vcc5v0_usb_host>;
};
```

## Observed Linux regulator state

With a device connected, `regulator_summary` showed:

```text
vcc5v0_usb_host   3 consumers  5000mV
    phy-fe8b0000.usb2phy.3-phy
    phy-fe8b0000.usb2phy.2-phy
    phy-fe8a0000.usb2phy.0-phy
vcc5v0_usb_otg    1 consumer   5000mV
    phy-fe8a0000.usb2phy.1-phy
```

The logical regulator state can be "enabled" while the physical GPIO is not
actually driven, if the GPIO write mask is wrong. Always verify the GPIO
direction and output registers, not only the regulator summary.

## Other board rails

These are not USB VBUS but are referenced by the schematic:

- `vcc3v3_sys`, `vcc_3v3`, `vcc_1v8`
- `vccio_phy1` on v2.1 (1.8 V Ethernet PHY IO)
- `vcc3v3_pcie30`
- `vdd_cpu`, `vdd_logic`, `vdd_gpu`, `vdd_npu`
- `vcca_1v8`, `vcca1v8_pmu`, `vcca1v8_image`

The RK809 PMIC manages the SoC rails; the USB 5 V switches are discrete GPIO
load switches.
