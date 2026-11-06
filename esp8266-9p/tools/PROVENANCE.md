# Xtensa LX106 toolchain - provenance

Fetched to cross-build firmware/libpp and the NONOS test firmware.
Recorded before its output was trusted.

- Source: Espressif official download server
  https://dl.espressif.com/dl/xtensa-lx106-elf-linux64-1.22.0-100-ge567ec7-5.2.0.tar.gz
- Version: crosstool-NG 1.22.0-100-ge567ec7, GCC 5.2.0
  (`xtensa-lx106-elf-gcc (crosstool-NG crosstool-ng-1.22.0-100-ge567ec7) 5.2.0`)
- Tarball: xtensa-lx106-elf-1.22.0-100.tar.gz (37,049,855 bytes)
  SHA-256: 706a02853759c2f85d912f68df4f5b4566ecb41422de5afe35a45d064eb8e494
- Extracted to: tools/xtensa-lx106-elf/ (ignored by git, below)
- Why this one: it is the era-correct toolchain for the pinned
  NONOS SDK 2.0.0 (2016-era SDK, GCC 5.2), so test firmware links
  against the SDK blobs the same way the stock build did. It is a
  build tool only; none of it ships in firmware.

The tarball and extracted tree are in .gitignore; this file and
the build scripts are the reproducible record. Re-fetch with the
URL above and check the SHA-256 before use.

## RTOS toolchain (added B14)

The RTOS SDK v3.4 needs a newer toolchain than the NONOS one
above (it links `-lc_nano`, absent from GCC 5.2).

- Source: https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz
- Version: crosstool-NG esp-2020r3-49-gd5524c1, GCC 8.4.0
- Tarball: xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz
  (88,885,561 bytes)
  SHA-256: 0a1804b5e2231c6db8b72af6bc2a0f9a5b6994cfba29956d412651109f13fe7e
  (matches the sha256 recorded in the RTOS SDK's tools/tools.json)
- Extracted to: tools/xtensa-lx106-elf-rtos/
- Used only by firmware/rtossrv/build-rtossrv.sh. The NONOS
  builds keep using the GCC 5.2 toolchain above.

