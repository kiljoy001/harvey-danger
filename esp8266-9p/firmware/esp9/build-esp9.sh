#!/bin/bash
# Build the esp9 firmware (RTOS-generation blobs on the
# Plan 9 kernel shim). Build only - never flashes.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
export IDF_PATH="$ROOT/rtos/ESP8266_RTOS_SDK"
export PATH="$ROOT/.venv/bin:$ROOT/tools/xtensa-lx106-elf-rtos/bin:$PATH"
make defconfig >/dev/null
make -j4

# The clean-room libpp objects are linked loose
# (EXTRA_LDFLAGS) so they win over libpp.a in the
# --start-group. Loose objects bypass the ldgen
# archive-member placement that puts all of libpp.a in
# IRAM, and Wi-Fi init/MAC ISR call pp with the flash
# cache off (B37: flash-resident pp hung esp_wifi_init
# before pp_attach). Rename their text/literal sections
# into .iram1.* so the base script's IRAM rule catches
# them, then relink.
OBJCOPY="$ROOT/tools/xtensa-lx106-elf-rtos/bin/xtensa-lx106-elf-objcopy"
OBJDUMP="$ROOT/tools/xtensa-lx106-elf-rtos/bin/xtensa-lx106-elf-objdump"
for o in esf_buf pp_esf pp_lldesc rate_control trc trc_tables pp pm if_hwctrl lmac; do
	f="build/freertos/$o.o"
	[ -f "$f" ] || continue
	args=""
	for s in $("$OBJDUMP" -h "$f" | awk '{print $2}' |
			grep -E '^\.(text|literal)\.'); do
		args="$args --rename-section $s=.iram1$s"
	done
	[ -n "$args" ] && "$OBJCOPY" $args "$f"
done
make -j4

# Memory budget gate (DESIGN section 6.2): the B19 wedge was
# BSS growth starving lwIP/Wi-Fi heap, found on-device after
# a day of flake-chasing. Fail the build instead. The heap
# lives between _bss_end and the top of DRAM (0x40000000,
# less the boot stack, hence the conservative floor).
NM="$ROOT/tools/xtensa-lx106-elf-rtos/bin/xtensa-lx106-elf-nm"
ELF=build/esp9.elf
BSS_END=$("$NM" "$ELF" | awk '/ _bss_end$/{print $1}')
BSS_START=$("$NM" "$ELF" | awk '/ _bss_start$/{print $1}')
MIN_HEAP=${ESP9_MIN_HEAP:-24576}
python3 - "$BSS_START" "$BSS_END" "$MIN_HEAP" <<'EOF'
import sys
bss = int(sys.argv[2], 16) - int(sys.argv[1], 16)
heap = 0x40000000 - int(sys.argv[2], 16)
print("memory budget: bss=%d bytes, heap headroom=%d bytes (min %d)"
      % (bss, heap, int(sys.argv[3])))
if heap < int(sys.argv[3]):
    print("memory budget: FAIL - heap headroom below floor; "
          "shrink statics/stacks before flashing")
    sys.exit(1)
EOF
echo "built: firmware/esp9/build/esp9.bin (NOT flashed)"
