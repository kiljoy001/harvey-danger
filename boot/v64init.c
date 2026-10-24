#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	char buf[128];
	int fd, n;

	USED(argc); USED(argv);
	print("HALATHA: Harvey userland on v64\n");
	fd = open("/README.md", OREAD);
	if(fd < 0)
		print("HALATHA: open failed: %r\n");
	else {
		n = read(fd, buf, sizeof buf - 1);
		if(n < 0)
			n = 0;
		buf[n] = 0;
		print("HALATHA: root read %d: %s", n, buf);
		close(fd);
	}
	print("HALATHA: entering idle\n");
	for(;;)
		sleep(10000);
}
