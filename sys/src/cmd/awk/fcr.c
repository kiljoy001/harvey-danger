#include <u.h>
#include <libc.h>
#include <mmintrin.h>

u32
getfcr(void)
{
	u32 csr = _mm_getcsr();
	return (csr & 0xFFC0) ^ (0x3F<<7);
}

void
setfcr(u32 v)
{
	u32 csr = _mm_getcsr();
	csr = (csr & ~0xFFC0) | ((v ^ (0x3F<<7)) & 0xFFC0);
	_mm_setcsr(csr);
}
