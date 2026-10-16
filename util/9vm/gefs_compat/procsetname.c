#include <u.h>
#include <libc.h>

void
procsetname(char *fmt, ...)
{
	char path[64], name[128];
	va_list ap;
	int fd, n;

	snprint(path, sizeof path, "/proc/%d/args", getpid());
	fd = open(path, OWRITE|OCEXEC);
	if(fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprint(name, sizeof name, fmt, ap);
	va_end(ap);
	write(fd, name, n+1);
	close(fd);
}
