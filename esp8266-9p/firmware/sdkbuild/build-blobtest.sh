#!/bin/bash
# Build the esp9 RTOS-blob-on-Plan-9 test firmware (p9 primitives +
# OSI surface on the Xtensa substrate). Build only - never flashes.
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
export PATH="$ROOT/tools/xtensa-lx106-elf/bin:$PATH"
export COMPILE=gcc
cd blobtest
# fix_cache_bug/phy_get_bb_evm run with the flash cache off and
# must be in IRAM; rename their sections in a copy of libphy.a
# (the SDK archive itself is never modified).
mkdir -p blobphy
cp "$ROOT/rtos/ESP8266_RTOS_SDK/components/esp8266/lib/libphy.a" blobphy/libphy.a
(cd blobphy && "$ROOT/tools/xtensa-lx106-elf/bin/xtensa-lx106-elf-ar" x libphy.a phy_chip_v6_cal.o \
  && "$ROOT/tools/xtensa-lx106-elf/bin/xtensa-lx106-elf-objcopy" \
       --rename-section .text.fix_cache_bug=.phyiram.text.fix_cache_bug \
       --rename-section .literal.fix_cache_bug=.phyiram.literal.fix_cache_bug \
       --rename-section .text.phy_get_bb_evm=.phyiram.text.phy_get_bb_evm \
       --rename-section .literal.phy_get_bb_evm=.phyiram.literal.phy_get_bb_evm \
       phy_chip_v6_cal.o \
  && "$ROOT/tools/xtensa-lx106-elf/bin/xtensa-lx106-elf-ar" r libphy.a phy_chip_v6_cal.o)
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
cp eagle.app.flash.bin bin/user1-blobtest.bin
sha256sum bin/user1-blobtest.bin
echo "built: firmware/sdkbuild/blobtest/bin/user1-blobtest.bin (NOT flashed)"
