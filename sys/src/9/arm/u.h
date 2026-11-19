/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution and at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * 32-bit ARM (ARMv7-A) definitions.
 */
#ifndef HARVEY_ARM_U_H
#define HARVEY_ARM_U_H

#define nil ((void *)0)

typedef signed char i8;
typedef unsigned char u8;
typedef signed short i16;
typedef unsigned short u16;
typedef signed int i32;
typedef unsigned int u32;
typedef signed long long i64;
typedef unsigned long long u64;

/*
 * A 32-bit machine word.  Plan 9 keeps ulong at the machine word size,
 * so the shared port code's "pointer-sized" types stay 4 bytes here.
 */
typedef u32 usize;
typedef i32 isize;
typedef u32 uintptr;
typedef i32 intptr;

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
typedef u32 ulong;
typedef u32 u32int;
typedef u64 uvlong;
typedef i64 vlong;
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

typedef u32 jmp_buf[32];

#define JMPBUFDPC	0

#endif
