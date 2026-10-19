/* EL0 transition, syscall, and process-fault handling. */
#ifdef ARM64_FULL_KERNEL
#include <u.h>
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "../port/error.h"
#endif
#include "user.h"

extern u8 arm64usercode[], arm64usercodeend[];
extern u8 arm64userfaultcode[], arm64userfaultcodeend[];
extern u8 arm64userwritecode[], arm64userwritecodeend[];
extern u8 arm64userexecstack[], arm64userexecstackend[];
extern char arm64userresume[];
#ifdef ARM64_FULL_KERNEL
extern void arm64processresume(void) __attribute__((noreturn));
#endif
extern u64 arm64enteruser(u64, u64, u64, u64, u64);
extern void arm64fault(void) __attribute__((noreturn));

enum {
	Userpagesz = 4096,
};

static volatile u32 useractive, trapkind;
static volatile u32 usercalls;
static volatile u64 trapesr, trapfar;
static volatile u64 expectedfault;
static volatile u32 expectedec;
static volatile u32 diagstage;
static volatile u64 diagresult;
#ifdef ARM64_FULL_KERNEL
static volatile u32 processactive;
#endif

static void
synccode(u64 base, usize n)
{
	u64 ctr, dline, p, end;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	dline = 4ULL << ((ctr>>16)&15);
	end = base+n;
	for(p = base & ~(dline-1); p < end; p += dline)
		__asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	/* The page executes at ARM64USERBASE, not at its identity-map VA.
	 * Invalidate both aliases without depending on the I-cache index. */
	__asm__ volatile("ic iallu" ::: "memory");
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

int
arm64userload(u64 page, u8 *start, u8 *end)
{
	return copycode(page, start, end);
}

static u16
get16(const u8 *p)
{
	return (u16)p[0] | (u16)p[1]<<8;
}

static u32
get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static u64
get64(const u8 *p)
{
	return (u64)get32(p) | (u64)get32(p+4)<<32;
}

int
arm64userloadelfpages(u64 codepage, u64 datapage, usize pagesz, u64 base,
	const u8 *image, usize n, u64 *entry)
{
	const u8 *ph;
	u64 filesz, memsz, offset, phoff, va, vend;
	u32 flags, i, j, phnum;
	int executable, loaded;
	u16 phentsize;
	u8 *dst;

	if(codepage == 0 || datapage == 0 || image == nil || entry == nil ||
	   pagesz < Userpagesz || (pagesz & (pagesz-1)) != 0 ||
	   (codepage|datapage|base) & (pagesz-1) || n < 64 ||
	   image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' ||
	   image[3] != 'F' || image[4] != 2 || image[5] != 1 ||
	   image[6] != 1 || get16(image+16) != 2 || get16(image+18) != 183 ||
	   get32(image+20) != 1 || get16(image+52) != 64)
		return -1;
	phoff = get64(image+32);
	phentsize = get16(image+54);
	phnum = get16(image+56);
	if(phentsize != 56 || phnum == 0 || phnum > 16 || phoff > n ||
	   (u64)phnum*phentsize > n-phoff)
		return -1;
	*entry = get64(image+24);
	for(i = 0; i < pagesz; i++){
		((u8 *)codepage)[i] = 0;
		((u8 *)datapage)[i] = 0;
	}
	executable = 0;
	loaded = 0;
	for(i = 0; i < phnum; i++){
		ph = image+phoff+(u64)i*phentsize;
		if(get32(ph) != 1)
			continue;
		flags = get32(ph+4);
		offset = get64(ph+8);
		va = get64(ph+16);
		filesz = get64(ph+32);
		memsz = get64(ph+40);
		if(filesz > memsz || offset > n || filesz > n-offset ||
		   memsz > 2*pagesz || va < base ||
		   va-base > 2*pagesz-memsz || (flags & 3) == 3)
			return -1;
		vend = va+memsz;
		if(va >= base+pagesz){
			if(vend > base+2*pagesz || (flags & 2) == 0)
				return -1;
			dst = (u8 *)datapage+(va-(base+pagesz));
		}else{
			if(vend > base+pagesz || (flags & 2) != 0)
				return -1;
			dst = (u8 *)codepage+(va-base);
		}
		for(j = 0; j < filesz; j++)
			dst[j] = image[offset+j];
		loaded = 1;
		if((flags & 1) != 0 && *entry >= va && *entry < vend)
			executable = 1;
	}
	if(!loaded || !executable)
		return -1;
	synccode(codepage, pagesz);
	return 0;
}

int
arm64userloadelf(u64 codepage, u64 datapage, const u8 *image, usize n,
	u64 *entry)
{
	return arm64userloadelfpages(codepage, datapage, Userpagesz,
		ARM64USERBASE, image, n, entry);
}

void
arm64usertrap(Arm64ureg *r)
{
	u32 ec;

	ec = (r->esr>>26)&63;
#ifdef ARM64_FULL_KERNEL
	Proc *p;
	int ftype, insyscall;

	p = externup();
	if(p != nil && p->kp == 0 && ec == 0x15 && (r->esr&0xffff) == 0){
		arm64syscall(r);
		return;
	}
	if(processactive != 0 && ec == 0x15 && (r->esr&0xffff) == 3 &&
	   r->x[0] == 57){
		processactive = 0;
		r->elr = (u64)arm64processresume;
		r->spsr = 0x3c5;
		return;
	}
	if(p != nil && p->kp == 0 &&
	   (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25)){
		ftype = (ec == 0x20 || ec == 0x21) ? FT_EXEC :
			((r->esr & (1ULL<<6)) != 0 ? FT_WRITE : FT_READ);
		insyscall = p->insyscall;
		p->insyscall = 1;
		if(fault(r->far, r->elr, ftype) < 0){
			p->insyscall = insyscall;
			print("ARM64 user fault pid %d pc %#llx far %#llx esr %#llx: %s\n",
				p->pid, r->elr, r->far, r->esr, p->errstr);
			if((ec == 0x21 || ec == 0x25) && insyscall && p->nerrlab != 0)
				error("invalid address in system call");
			pexit("user fault", 1);
		}
		p->insyscall = insyscall;
		return;
	}
#endif
	if(useractive == 0)
		arm64fault();
	trapesr = r->esr;
	trapfar = r->far;
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

#ifdef ARM64_FULL_KERNEL
void
arm64runprocessargs(u64 entry, u64 stack, u64 arg0, u64 arg1, u64 arg2)
{
	processactive = 1;
	arm64enteruser(entry, stack, arg0, arg1, arg2);
	arm64fault();
}

void
arm64runprocess(u64 entry, u64 stack, u64 arg)
{
	arm64runprocessargs(entry, stack, arg, 0, 0);
}
#endif

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
	result = arm64enteruser(ARM64USERBASE, ARM64USERBASE+8192, arg, 0, 0);
	useractive = 0;
	diagresult = result;
	if(trapkind != 2 || result != ~(u64)1 || trapfar != arg)
		return -1;
	return 0;
}

int
arm64userprobe(u64 codepage, u64 stackpage, u64 kernelpage)
{
	u64 result, mapped;

	diagstage = 1;
	if(arm64mmutranslate(ARM64USERBASE+4096, &mapped) < 0 ||
	   mapped != stackpage)
		return -1;
	diagstage = 2;
	if(copycode(codepage, arm64usercode, arm64usercodeend) < 0)
		return -1;
	trapkind = 0;
	usercalls = 0;
	useractive = 1;
	result = arm64enteruser(ARM64USERBASE, ARM64USERBASE+8192, 0, 0, 0);
	useractive = 0;
	diagresult = result;
	diagstage = 3;
	if(trapkind != 1 || usercalls != 1 || result != 42)
		return -1;
	diagstage = 4;
	if(faultprobe(codepage, arm64userfaultcode, arm64userfaultcodeend,
	              kernelpage, 0x24) < 0)
		return -1;
	diagstage = 5;
	if(faultprobe(codepage, arm64userwritecode, arm64userwritecodeend,
	              ARM64USERBASE, 0x24) < 0)
		return -1;
	diagstage = 6;
	if(faultprobe(codepage, arm64userexecstack, arm64userexecstackend,
	              ARM64USERBASE+4096, 0x20) < 0)
		return -1;
	diagstage = 0;
	return 0;
}

u64
arm64userdiag(void)
{
	return trapesr;
}

u64
arm64userdiagdetails(void)
{
	return (u64)diagstage<<48 | (u64)trapkind<<32 |
	       (u64)usercalls<<16 | (diagresult & 0xffff);
}
