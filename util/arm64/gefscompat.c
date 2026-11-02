/*
 * The few libc functions 9front has and this libc does not, for gefs.
 */
#include <u.h>
#include <libc.h>

void
tmnow(Tm *tm, vlong *nsec)
{
	*tm = *localtime(time(nil));
	if(nsec != nil)
		*nsec = 0;
}

/* Normalise the fields and return the time in seconds. */
vlong
tmnorm(Tm *tm)
{
	vlong t;

	t = tm2sec(tm);
	*tm = *localtime((i32)t);
	return t;
}

void
tmfmtinstall(void)
{
}

void
procsetname(char *fmt, ...)
{
	USED(fmt);
}
