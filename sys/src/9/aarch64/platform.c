/* Common ARM64 kernel services backed by architectural instructions. */
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "ureg.h"
#include "../port/error.h"

extern void arm64uartputs(char *);

u64
PADDR(void *p)
{
	/* The ARM64 bring-up kernel still uses an identity-mapped DMA window. */
	return (u64)(usize)p;
}

KMap *
kmap(Page *page)
{
	/* QEMU virt's bootstrap map covers RAM at its physical address. */
	return (KMap *)KADDR(page->pa);
}

int
ainc(int *p)
{
	return __atomic_add_fetch(p, 1, __ATOMIC_ACQ_REL);
}

int
adec(int *p)
{
	return __atomic_sub_fetch(p, 1, __ATOMIC_ACQ_REL);
}

int
CASW(u32 *p, u32 old, u32 new)
{
	return __atomic_compare_exchange_n(p, &old, new, 0,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

u64
rdtsc(void)
{
	u64 n;
	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(n));
	return n;
}

u64
fastticks(u64 *hz)
{
	if(hz != nil)
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(*hz));
	return rdtsc();
}

u64
perfticks(void)
{
	return rdtsc();
}

u32
ms(void)
{
	u64 hz;
	u64 now;

	now = fastticks(&hz);
	return hz == 0 ? 0 : (u32)((now / hz) * 1000 +
		((now % hz) * 1000) / hz);
}

void
microdelay(int us)
{
	u64 hz, start, delta;

	if(us <= 0)
		return;
	start = fastticks(&hz);
	delta = ((u64)us * hz + 999999) / 1000000;
	while(rdtsc() - start < delta)
		;
}

void
delay(int ms)
{
	while(ms-- > 0)
		microdelay(1000);
}

void
timerset(u64 when)
{
	/* Port clock deadlines use the same virtual counter as fastticks. */
	__asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb"
		:: "r"(when), "r"((u64)1) : "memory");
}

void
mmuflush(void)
{
	Proc *p;

	p = externup();
	if(p != nil)
		mmurelease(p);
	else
		mmuflushtlb();
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
		if(fp < lo || fp + 2 * sizeof(usize) > hi || (fp & 15))
			return i + 1;
		next = *(usize *)fp;
		pc = *(usize *)(fp + sizeof(usize));
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

	__asm__ volatile("mov %0, x29" : "=r"(fp));
	pc = (usize)__builtin_return_address(0);
	n = backtrace_list(pc, fp, pcs, nelem(pcs));
	print("cpu%d stack:", machp()->machno);
	for(i = 0; i < n; i++)
		print(" %#p", (void *)pcs[i]);
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
	u64 n;

	n = va_arg(f->args, u64);
	return fmtprint(f, "%#llx", n);
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
	arm64uartputs(s == nil ? "<nil>" : s);
}

void
die(char *s)
{
	hi("die: ");
	hi(s);
	hi("\n");
	for(;;)
		__asm__ volatile("wfi");
}

void
hardhalt(void)
{
	__asm__ volatile("wfi");
}

void
exit(int panic)
{
	USED(panic);
	__asm__ volatile("msr daifset, #3" ::: "memory");
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
	error("no application cores on this ARM64 machine");
	return nil;
}

void
intrac(Proc *p)
{
	if(p->ac != nil)
		panic("ARM64 application-core interrupt without SMP support");
}

void
stopac(void)
{
	if(externup() != nil && externup()->ac != nil)
		panic("ARM64 application-core stop without SMP support");
}

void
runacore(void)
{
	panic("ARM64 application cores are not enabled");
}

void
DONE(void)
{
	exit(1);
}

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
	error("ARM64 floating-point state is unavailable");
	return -1;
}

void
kbdputsc(int c, int external)
{
	USED(c);
	USED(external);
	error("ARM64 keyboard scan-code input is unavailable");
}
