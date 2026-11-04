/*
 * aarch64-none-elf has no <machine/endian.h>; LLVM's ADT/bit.h falls back
 * to it when the target is not recognised as Linux/Apple/etc.  Harvey's
 * ARM64 target is little-endian.
 */
#ifndef _MACHINE_ENDIAN_H
#define _MACHINE_ENDIAN_H

#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define PDP_ENDIAN 3412
#define BYTE_ORDER LITTLE_ENDIAN

#endif
