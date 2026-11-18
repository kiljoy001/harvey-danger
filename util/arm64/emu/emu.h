/*
 * Machine-specific declarations for emu on Harvey/ARM64: the FPU save
 * area, the error-jump primitive, and the current-process pointer.
 *
 * Harvey's libc provides setjmp/longjmp and a jmp_buf wide enough for
 * the AArch64 context (u.h: jmp_buf is 32 u64 slots).
 */
extern	Proc**	Xup;
#define	up	(*Xup)

typedef	struct	FPU	FPU;

/* Must agree with FPsave/FPrestore in asm-arm64.S. */
struct FPU
{
	u64	fpsr;
	u64	fpcr;
};

typedef jmp_buf osjmpbuf;
#define	ossetjmp(buf)	setjmp(buf)
