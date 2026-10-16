/* EL0 transition and temporary syscall/permission-fault verification. */
#include "user.h"

extern u8 arm64usercode[], arm64usercodeend[];
extern u8 arm64userfaultcode[], arm64userfaultcodeend[];
extern u8 arm64userwritecode[], arm64userwritecodeend[];
extern u8 arm64userexecstack[], arm64userexecstackend[];
extern char arm64userresume[];
extern u64 arm64enteruser(u64, u64, u64);
extern void arm64fault(void);

static volatile u32 useractive, trapkind;
static volatile u32 usercalls;
static volatile u64 trapesr, trapfar;
static volatile u64 expectedfault;
static volatile u32 expectedec;

static void
synccode(u64 base, usize n)
{
	u64 ctr, dline, iline, p, end;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	dline = 4ULL << ((ctr>>16)&15);
	iline = 4ULL << (ctr&15);
	end = base+n;
	for(p = base & ~(dline-1); p < end; p += dline)
		__asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	for(p = base & ~(iline-1); p < end; p += iline)
		__asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish; isb" ::: "memory");
}

static int
copycode(u64 page, u8 *start, u8 *end)
{
	u8 *dst;
	usize n, i;

	n = (usize)end-(usize)start;
	if(n == 0 || n > 4096)
		return -1;
	dst = (u8 *)page;
	for(i = 0; i < n; i++)
		dst[i] = start[i];
	synccode(page, n);
	return 0;
}

void
arm64usertrap(Arm64ureg *r)
{
	u32 ec;

	if(useractive == 0)
		arm64fault();
	trapesr = r->esr;
	trapfar = r->far;
	ec = (r->esr>>26)&63;
	if(ec == 0x15 && (r->esr&0xffff) == 1){
		/* Return a value and resume at the instruction following SVC. */
		r->x[0]++;
		usercalls++;
		return;
	}else if(ec == 0x15 && (r->esr&0xffff) == 0){
		trapkind = 1;
	}else if(ec == expectedec && r->far == expectedfault){
		trapkind = 2;
		r->x[0] = ~(u64)1;
	}else{
		trapkind = 3;
		r->x[0] = ~(u64)0;
	}
	r->elr = (u64)arm64userresume;
	r->spsr = 0x3c5;
}

static int
faultprobe(u64 codepage, u8 *start, u8 *end, u64 arg, u32 ec)
{
	u64 result;

	if(copycode(codepage, start, end) < 0)
		return -1;
	expectedfault = arg;
	expectedec = ec;
	trapkind = 0;
	useractive = 1;
	result = arm64enteruser(ARM64USERBASE, ARM64USERBASE+8192, arg);
	useractive = 0;
	if(trapkind != 2 || result != ~(u64)1 || trapfar != arg)
		return -1;
	return 0;
}

int
arm64userprobe(u64 codepage, u64 stackpage, u64 kernelpage)
{
	u64 result, mapped;

	if(arm64mmutranslate(ARM64USERBASE+4096, &mapped) < 0 ||
	   mapped != stackpage)
		return -1;
	if(copycode(codepage, arm64usercode, arm64usercodeend) < 0)
		return -1;
	trapkind = 0;
	usercalls = 0;
	useractive = 1;
	result = arm64enteruser(ARM64USERBASE, ARM64USERBASE+8192, 0);
	useractive = 0;
	if(trapkind != 1 || usercalls != 1 || result != 42)
		return -1;
	if(faultprobe(codepage, arm64userfaultcode, arm64userfaultcodeend,
	              kernelpage, 0x24) < 0)
		return -1;
	if(faultprobe(codepage, arm64userwritecode, arm64userwritecodeend,
	              ARM64USERBASE, 0x24) < 0)
		return -1;
	if(faultprobe(codepage, arm64userexecstack, arm64userexecstackend,
	              ARM64USERBASE+4096, 0x20) < 0)
		return -1;
	return 0;
}

u64
arm64userdiag(void)
{
	return trapesr;
}
