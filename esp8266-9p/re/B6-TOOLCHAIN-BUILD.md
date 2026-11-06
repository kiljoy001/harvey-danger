# B6 - toolchain pinned, libpp cross-built, test firmware built (not flashed)

Continues B5. Nothing was flashed; the board still runs stock AT.
The approval gate from DESIGN section 9 phase 2 (explicit approval
before flashing anything) is unchanged and unmet, so this stage
ends at a verified image on disk.

## 1. Toolchain (provenance in tools/PROVENANCE.md)

- Espressif's official xtensa-lx106-elf GCC 5.2.0
  (crosstool-NG 1.22.0-100-ge567ec7) from dl.espressif.com,
  tarball SHA-256 706a02853759c2f85d912f68df4f5b4566ecb
  41422de5afe35a45d064eb8e494, extracted under `tools/`
  (git-ignored; provenance and scripts are the record).
- Chosen because it is the era-correct compiler for the pinned
  NONOS SDK 2.0.0, so the test firmware links against the blobs
  the way the stock build did.
- Verified: `xtensa-lx106-elf-gcc --version` runs; produced ELF32
  Tensilica Xtensa objects.

## 2. libpp cross-build

    xtensa-lx106-elf-gcc -std=c99 -Os -Wall -Wextra -Werror \
      -mlongcalls -mtext-section-literals -c pp_lldesc.c pp_esf.c
    xtensa-lx106-elf-ar rcs libpp_clean.a ...

Clean under -Werror. Output `firmware/libpp/build-xtensa/
libpp_clean.a` (SHA-256 cfe2d2b9367c0f07d25daee430d8086cab798
677f7cd10a2f385859b5049ebb2), symbols as expected
(pp_desc_*, pp_buf_*, pp_pool_init). Host tests from B5 still
stand; the same sources compiled for the target without change,
including the 12-byte descriptor static assert on a true 32-bit
compile.

## 3. Minimal test firmware (firmware/sdkbuild/testapp)

NONOS SDK 2.0.0 legacy build, project layout copied from the
SDK's peripheral_test example with two documented deviations:

- `-lhal` removed from the link line: the SDK 2.0.0 tarball ships
  no libhal.a (its contents are in libmain in this release; the
  bundled examples still list it). Found by link failure, fixed
  in our copy only; the SDK reference tree is untouched.
- Packaging via esptool instead of the SDK's gen_appbin.py,
  which is Python-2 only and cannot run here. The SDK make does
  produce the linked ELF before that step; we package that ELF.

Tree: `firmware/sdkbuild/` holds symlinks (lib, ld, include,
tools, Makefile) into the pinned SDK extraction, so the SDK tree
itself stays pristine. `testapp/user/` is user_main.c plus the
libpp sources via symlinks. `build-testapp.sh` repeats the whole
build non-interactively.

user_main.c does the minimum behind blob init: sets UART0 to
115200, prints `ESP9-LIBPP-TEST boot sdk=...`, then runs the
libpp descriptor/pool checks on-device (chain linking, RX offer
owner/len with FCS reserve, reclaim, pool alloc/free) printing
`ESP9-LIBPP-TEST pass/FAIL <name>` lines and a final ALL-PASS
with free heap, for the CP2102 log at /dev/ttyUSB1 once flashing
is approved. It does not touch Wi-Fi or WDEV registers.

Build result (verified this stage):

- Link: `-lphy -lpp -lnet80211 -llwip -lwpa -lcrypto -lmain
  -ldriver` + libuser.a (with pp objects) -> eagle.app.v6.out;
  nm shows `user_init` (0x40206560), `call_user_start`, and the
  pp_ symbols linked in IRAM/IROM as expected.
- Image: `testapp/bin/user1.bin`, 223,716 bytes, esptool
  image-info: v2 image, entry 0x40100004, 4 MB/QIO/40 MHz,
  checksum valid. SHA-256 eb3d51cca3dc120a8c770fce859254311
  f5b215214e59bc10ab0ecbccb6bbc75. (Binary is git-ignored;
  rebuild with build-testapp.sh.)

Failures on the way, for the record: missing user_config.h
(added, minimal), UART_CLK_FREQ redefinition (SDK defines it),
os_printf_plus/uart_div_modify needing explicit externs (no
public header in this SDK), the -lhal issue above, and the
Python-2 packaging step. All were build-environment fixes; no
libpp or spec changes were needed.

## 4. Approval gate (unchanged)

Flashing this image would replace the stock AT application
(user1 at flash 0x1000, with an SDK boot image) on the real
board. That is DESIGN phase 2 and still needs: (a) your explicit
approval to flash, and (b) the still-open design choices from
DESIGN section 10 that affect later phases (they do not block
this test image). The verified stock backup makes the operation
reversible with the restore command in backup/BACKUP.md, but
reversible is not the same as approved.

What approval would let happen next, exactly: write SDK
boot_v1.6 + this user1.bin + blank init data with esptool,
reset over the CP2102, and read the ESP9-LIBPP-TEST lines from
/dev/ttyUSB1 at 115200. Nothing else.

## 5. Next after approval

1. Flash + UART-verify the test image; restore stock and verify
   AT answers again (the phase exit test from DESIGN).
2. Then the plan9ized OS work proper: DESIGN phase 1 host 9P
   mock, then phase 2 read-only firmware with the IL transport
   (spec in DESIGN section 4).
3. Blob-free WDEV bring-up stays later (SPEC section 5 opens).

