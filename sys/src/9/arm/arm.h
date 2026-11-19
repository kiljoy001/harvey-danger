/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in LICENSE.gpl found in the top-level
 * directory of this distribution at http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * ARMv7-A machine definitions shared by C and assembly.
 */
#ifndef HARVEY_ARM_H
#define HARVEY_ARM_H

#define USED(x)		((void)(x))

/* Program status register: modes and bits. */
#define PsrMusr		0x10
#define PsrMfiq		0x11
#define PsrMirq		0x12
#define PsrMsvc		0x13
#define PsrMabt		0x17
#define PsrMund		0x1b
#define PsrMsys		0x1f

#define PsrDfiq		0x40
#define PsrDirq		0x80

/* Coprocessor 15 register numbers used by the early boot path. */
#define CpSC		15
#define CpCONTROL	1
#define CpTTB		2
#define CpDAC		3

/* Cache and TLB maintenance. */
#define CpCACHE		7
#define CpTLB		8

/* Level-1 descriptor types (short-descriptor format). */
#define Fault		0x0
#define Coarse		0x1
#define Section		0x2
#define Fine		0x3

#define Dom0		0
#define Krw		1
#define Urw		3

/* ARMv7 section attribute bits. */
#define L1wralloc	(1 << 12)	/* TEX[2]: write-allocate */
#define L1sharable	(1 << 16)	/* S: shareable */
#define L1noexec	(1 << 4)	/* XN */

/* Exception entry points, in l.S. */
extern void arm32vectors(void);
extern void arm32vectors_end(void);
void arm32setttbr0(u32);
void arm32setdacr(u32);
void arm32sethighvectors(void);
void arm32tlbflush(void);
void arm32cacheflush(void);
void arm32mmuenable(void);
u32 arm32cntfrq(void);
void arm32setcntvctl(u32);
u32 arm32cntvtval(void);
u32 arm32cntvctl(void);
void arm32setcntvtval(u32);
u32 arm32id(void);
u32 arm32dfar(void);
u32 arm32dfsr(void);

/* mmu.c */
extern u32 arm32l1[4096];
void arm32mmuinit(void);

/* arch.c */
void uartinit(void);
void uartputc(int);
int uartgetc(void);
void putstr(char *);
void arm32conswrite(char *, int);
void delay(int);
void microdelay(int);
void coherence(void);
void dcacheclean(void *, usize);
void dcachecleanall(void);
void hardhalt(void);

/* main.c */
void arm32main(u32);
void printk(char *, ...);
void arm32irqhandler(void);
void arm32svchandler(void);
void arm32unexpected(u32);
void arm32prefetchabort(u32);
void arm32dataabort(u32, u32, u32);

#endif
