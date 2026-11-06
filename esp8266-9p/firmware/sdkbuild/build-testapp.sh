#!/bin/bash
# Build the esp9 libpp NONOS test firmware. Build only - this
# script never flashes. Run from anywhere:
#   esp8266-9p/firmware/sdkbuild/build-testapp.sh
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)

export PATH="$ROOT/tools/xtensa-lx106-elf/bin:$PATH"

# NONOS SDK legacy build (BOOT=new APP=1, 4 MB map 1024+1024).
# Its final gen_appbin.py step is Python-2 only and unavailable;
# the linked ELF it produces before that step is the real output.
cd testapp
make COMPILE=gcc BOOT=new APP=1 SPI_SPEED=40 SPI_MODE=QIO \
  SPI_SIZE_MAP=6 || true
ELF=.output/eagle/debug/image/eagle.app.v6.out
test -f "$ELF"

# Package with the Python 3 port of the SDK's gen_appbin.py
# (boot_mode=2, QIO, clk div 0, size_map 6, user bin 1). The
# esptool elf2image format does not boot with boot 1.6: it puts
# a flash-size code in the first header byte where boot 1.6
# expects the user-bin number (see re/B7-FLASH-TEST.md).
export COMPILE=gcc
python3 ../gen_appbin_py3.py "$ELF" 2 0 0 6 1
mkdir -p bin
cp eagle.app.flash.bin bin/user1-gen.bin
sha256sum bin/user1-gen.bin
echo "built: firmware/sdkbuild/testapp/bin/user1-gen.bin (NOT flashed)"
