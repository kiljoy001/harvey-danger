#!/bin/bash
# Build the esp9 Phase A firmware (9P/TCP 564, SoftAP bootstrap).
# Build only - never flashes. Run from anywhere.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
export PATH="$ROOT/tools/xtensa-lx106-elf/bin:$PATH"
export COMPILE=gcc
cd esp9srv
touch .build-start
make COMPILE=gcc BOOT=new APP=1 SPI_SPEED=40 SPI_MODE=QIO SPI_SIZE_MAP=6 || true
ELF=.output/eagle/debug/image/eagle.app.v6.out
test -f "$ELF"
# The SDK make fails at its Python-2 packaging step after linking;
# make sure the ELF is from this run, not a stale one.
test "$ELF" -nt .build-start
rm -f eagle.app.flash.bin
python3 ../gen_appbin_py3.py "$ELF" 2 0 0 6 1
mkdir -p bin
cp eagle.app.flash.bin bin/user1-phaseA.bin
sha256sum bin/user1-phaseA.bin
echo "built: firmware/sdkbuild/esp9srv/bin/user1-phaseA.bin (NOT flashed)"
