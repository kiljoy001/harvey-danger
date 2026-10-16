#include <u.h>
#include <libc.h>

void
guesttmnow(Tm *tm)
{
	Tm *now;

	now = gmtime(time(nil));
	if(now != nil)
		*tm = *now;
	else
		memset(tm, 0, sizeof *tm);
}
