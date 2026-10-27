/*
 * Small 9front compatibility helpers for the plan9front Rio port:
 * min/max and badrect live in 9front libc/libdraw; Harvey lacks them.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <frame.h>

int
min(int a, int b)
{
	return a < b ? a : b;
}

int
max(int a, int b)
{
	return a > b ? a : b;
}

/* check for zero, negative size or insanely huge rectangle (from 9front libdraw) */
int
badrect(Rectangle r)
{
	int x, y;
	uint z;

	x = Dx(r);
	y = Dy(r);
	if(x > 0 && y > 0){
		z = x*y;
		if(z/x == y && z < 0x10000000)
			return 0;
	}
	return 1;
}
