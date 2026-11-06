#!/bin/bash
# Build the esp9 RTOS-substrate firmware. Build only - never flashes.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
export IDF_PATH="$ROOT/rtos/ESP8266_RTOS_SDK"
export PATH="$ROOT/.venv/bin:$ROOT/tools/xtensa-lx106-elf-rtos/bin:$PATH"
make defconfig >/dev/null
make -j4
echo "built: firmware/rtossrv/build/esp9-rtos.bin (NOT flashed)"
