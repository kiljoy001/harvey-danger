/*
 * This file is part of the Harvey operating system.  It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution and at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * No part of Harvey operating system, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms
 * contained in the LICENSE.gpl file.
 */

#ifndef HARVEY_AARCH64_U_H
#define HARVEY_AARCH64_U_H

#define nil ((void *)0)

typedef signed char i8;
typedef unsigned char u8;
typedef signed short i16;
typedef unsigned short u16;
typedef signed int i32;
typedef unsigned int u32;
typedef signed long long i64;
typedef unsigned long long u64;
typedef u64 usize;
typedef i64 isize;
typedef u8 u8int;
typedef u16 u16int;
typedef u32 u32int;
typedef u64 u64int;
typedef i8 s8int;
typedef i16 s16int;
typedef i32 s32int;
typedef i64 s64int;
typedef u8 uchar;
typedef u16 ushort;
typedef u32 uint;
typedef u64 ulong;
typedef u64 uvlong;
typedef i64 vlong;
typedef u64 uintptr;
typedef i64 intptr;
typedef u32 Rune;

#define YYSIZE_T usize
#define alignas _Alignas
#define static_assert _Static_assert

typedef union FPdbleword FPdbleword;
union FPdbleword
{
	double x;
	struct {	// little endian
		u32 lo;
                u32 hi;
	};
};

typedef u64 jmp_buf[32];

#define JMPBUFDPC       0
#define JMPBUFPC        11
#define JMPBUFSP        13
#define JMPBUFARG1      22
#define JMPBUFARG2      23

typedef u32	mpdigit;	// for /sys/include/mp.h

typedef __builtin_va_list va_list;

#define va_start(v,l)   __builtin_va_start(v,l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v,l)     __builtin_va_arg(v,l)
#define va_copy(v,l)    __builtin_va_copy(v,l)

#define getcallerpc()	((usize)__builtin_return_address(0))

#endif
