#!/bin/bash
# Build the esp9 Phase C kernel test firmware (p9 primitives +
# OSI surface on the Xtensa substrate). Build only - never flashes.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
export PATH="$ROOT/tools/xtensa-lx106-elf/bin:$PATH"
export COMPILE=gcc
cd kerntest
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
cp eagle.app.flash.bin bin/user1-kerntest.bin
sha256sum bin/user1-kerntest.bin
echo "built: firmware/sdkbuild/kerntest/bin/user1-kerntest.bin (NOT flashed)"
