#ifndef KEYROOT_MP_U_H
#define KEYROOT_MP_U_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t uchar;
typedef uint32_t u32int;
typedef uint64_t u64int;
typedef uint64_t ulong;
typedef uint32_t uint;
typedef uint64_t usize;
typedef uint32_t mpdigit;

#ifndef nil
#define nil ((void*)0)
#endif
#define nelem(x) (sizeof(x)/sizeof((x)[0]))
#define USED(x) (void)(x)

#endif
