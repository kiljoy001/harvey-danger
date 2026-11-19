/*
 * This file is part of the Harvey operating system. It is subject to the
 * license terms of the GNU GPL v2 in the top-level LICENSE.gpl file.
 *
 * Architecture services the shared kernel expects: physical/virtual
 * mapping (identity), atomics, time, cache maintenance and the small
 * stubs that let the port layer link before user processes exist.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "ureg.h"
#include "../port/error.h"

KMap *
kmap(Page *p)
{
	return (KMap*)KADDR(p->pa);
}

int
CASW(u32 *p, u32 old, u32 new)
{
	return __atomic_compare_exchange_n(p, &old, new, 0,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/*
 * There is no free-running counter in this guest: CNTVCT/CNTPCT trap
 * unless a hypervisor sets CNTHCTL.  Drive the clock from timer ticks,
 * which are microseconds by convention here (HZ=100 gives 10 ms).
 */
enum {
	Fasttickhz = 1000000,
	Ustotick = Fasttickhz / HZ,
};

u64
rdtsc(void)
{
	return arm32fastnow();
}

u64
fastticks(u64 *hz)
{
	if(hz != nil)
		*hz = Fasttickhz;
	return rdtsc();
}

u64
perfticks(void)
{
	return rdtsc();
}

void
cycles(u64 *t)
{
	*t = rdtsc();
}

u32
ms(void)
{
	return (u32)(rdtsc() / (Fasttickhz / 1000));
}

void
microdelay(int us)
{
	volatile int i;

	for(i = 0; i < us * 20; i++)
		;
}

void
delay(int ms)
{
	while(ms-- > 0)
		microdelay(1000);
}

void
coherence(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

void
dcacheclean(void *va, usize len)
{
	u32 ctr, line, p, end;

	if(va == nil || len == 0)
		return;
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
	line = 4u << ((ctr >> 16) & 0xf);
	p = (u32)va & ~(line - 1);
	end = (u32)va + len;
	for(; p < end; p += line)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");
	coherence();
}

void
dcachecleanall(void)
{
	/* Not needed while everything runs on the identity map. */
	coherence();
}

void
mmuflush(void)
{
}

int
backtrace_list(usize pc, usize fp, usize *pcs, usize n)
{
	usize lo, hi, next;
	int i;
	Proc *p;

	p = externup();
	lo = p == nil ? 0 : (usize)p->kstack;
	hi = lo + KSTACK;
	for(i = 0; i < n && pc != 0; i++){
		pcs[i] = pc;
		if(fp < lo || fp + 2 * sizeof(usize) > hi || (fp & 7))
			return i + 1;
		next = *(usize*)fp;
		pc = *(usize*)(fp + sizeof(usize));
		if(next <= fp)
			return i + 1;
		fp = next;
	}
	return i;
}

void
stacksnippet(void)
{
	usize fp, pc, pcs[8];
	int i, n;

	__asm__ volatile("mov %0, r11" : "=r"(fp));
	pc = (usize)__builtin_return_address(0);
	n = backtrace_list(pc, fp, pcs, nelem(pcs));
	print("cpu%d stack:", machp()->machno);
	for(i = 0; i < n; i++)
		print(" %#p", (void*)pcs[i]);
	print("\n");
}

void
dumpstack(void)
{
	stacksnippet();
}

static int
fmtaddr(Fmt *f)
{
	usize n;

	n = va_arg(f->args, usize);
	return fmtprint(f, "%#lux", n);
}

void
archfmtinstall(void)
{
	fmtinstall('P', fmtaddr);
	fmtinstall('L', fmtaddr);
	fmtinstall('R', fmtaddr);
	fmtinstall('W', fmtaddr);
}

void
hi(char *s)
{
	putstr(s == nil ? "<nil>" : s);
}

void
die(char *s)
{
	hi("die: ");
	hi(s);
	hi("\n");
	hardhalt();
}

void
exit(int panic)
{
	USED(panic);
	__asm__ volatile("cpsid if" ::: "memory");
	for(;;)
		__asm__ volatile("wfi");
}

void
reboot(void *entry, void *code, i32 size)
{
	USED(entry);
	USED(code);
	USED(size);
	exit(0);
}

Mach *
getac(Proc *p, int core)
{
	USED(p);
	USED(core);
	error("no application cores on this ARM machine");
	return nil;
}

void
intrac(Proc *p)
{
	if(p != nil && p->ac != nil)
		panic("application-core interrupt without SMP support");
}

void
stopac(void)
{
	if(externup() != nil && externup()->ac != nil)
		panic("application-core stop without SMP support");
}

void
runacore(void)
{
	panic("application cores are not enabled");
}

void
DONE(void)
{
	exit(1);
}

/*
 * Swap the caller's buffer with the current process's error string, the
 * kernel-side convention the libc's %r formatting expects.
 */
void
errstr(char *s, int n)
{
	Proc *p;
	int i;
	char c;

	p = externup();
	if(p == nil || s == nil || n <= 0)
		return;
	if(n > ERRMAX)
		n = ERRMAX;
	for(i = 0; i < n - 1; i++){
		c = s[i];
		s[i] = p->syserrstr[i];
		p->syserrstr[i] = c;
		if(c == 0 && s[i] == 0)
			break;
	}
	s[n - 1] = 0;
	p->syserrstr[n - 1] = 0;
}

int
fpudevprocio(Proc *p, void *buf, i32 n, usize off, int write)
{
	USED(p);
	USED(buf);
	USED(n);
	USED(off);
	USED(write);
	error("floating-point state is unavailable");
	return -1;
}

void
kbdputsc(int c, int external)
{
	USED(c);
	USED(external);
	error("keyboard scan-code input is unavailable");
}

/*
 * Interrupt locks: taslock.c is not linked (its lock() would be used by
 * the kernel libc's formatter, which has a smaller Lock), so provide the
 * interrupt-disabling variants here.
 */
void
ilock(Lock *l)
{
	if(l != nil)
		lock(l);
}

void
iunlock(Lock *l)
{
	if(l != nil)
		unlock(l);
}

void
hardhalt(void)
{
	for(;;)
		__asm__ volatile("wfi");
}

int
islo(void)
{
	return 0;
}

int
userureg(Ureg *u)
{
	USED(u);
	return 1;
}

void
sysrforkchild(Proc *p, Proc *up)
{
	USED(p);
	USED(up);
	panic("sysrforkchild: user processes are not supported yet");
}

usize
sysexecstack(usize n, int up)
{
	USED(n);
	USED(up);
	return 0;
}

void *
sysexecregs(usize entry, u32 sargs, void *v)
{
	USED(entry);
	USED(sargs);
	USED(v);
	return nil;
}

void
sysprocsetup(Proc *p)
{
	USED(p);
}

void
dumpgpr(Ureg *u)
{
	USED(u);
}

int
infected_with_std(void)
{
	return 0;
}

void
disinfect_std(void)
{
}

void
arm32ipi(void)
{
}

