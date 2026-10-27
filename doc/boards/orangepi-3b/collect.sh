#!/bin/sh
# Collect live hardware state from a Linux boot on the board.
# Run as root on the Orange Pi 3B; output goes to a directory.
#
#   sh collect.sh /tmp/opi3b-state
#
set -eu
out=${1:-/tmp/opi3b-state}
mkdir -p "$out"

dump() {
    name=$1
    path=$2
    if [ -r "$path" ]; then
        cat "$path" >"$out/$name" 2>/dev/null || true
    fi
}

dump uname /proc/version
dump cmdline /proc/cmdline
dump interrupts /proc/interrupts
dump iomem /proc/iomem
dump modules /proc/modules
dump devices /sys/kernel/debug/usb/devices
dump clk_summary /sys/kernel/debug/clk/clk_summary
dump regulator_summary /sys/kernel/debug/regulator/regulator_summary
dump gpio /sys/kernel/debug/gpio

for p in /sys/kernel/debug/pinctrl/*/pinmux-pins; do
    [ -r "$p" ] || continue
    n=$(echo "$p" | sed 's#/sys/kernel/debug/pinctrl/##; s#/pinmux-pins##')
    dump "pinctrl-$n" "$p"
done

for p in /sys/kernel/debug/regmap/*/registers; do
    [ -r "$p" ] || continue
    n=$(echo "$p" | sed 's#/sys/kernel/debug/regmap/##; s#/registers##')
    dump "regmap-$n" "$p"
done

# USB topology and descriptors when lsusb is available.
if command -v lsusb >/dev/null 2>&1; then
    lsusb >"$out/lsusb" 2>&1 || true
    lsusb -t >"$out/lsusb-tree" 2>&1 || true
    lsusb -v >"$out/lsusb-verbose" 2>&1 || true
fi

# Device-tree view as the kernel sees it.
if command -v dtc >/dev/null 2>&1 && [ -r /sys/firmware/fdt ]; then
    dtc -I dtb -O dts /sys/firmware/fdt >"$out/live.dts" 2>/dev/null || true
fi

echo "wrote $out"
