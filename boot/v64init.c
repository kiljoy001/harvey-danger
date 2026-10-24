#include <u.h>
#include <libc.h>

void
main(int argc, char **argv)
{
	USED(argc); USED(argv);
	print("HALATHA: Harvey userland on v64\n");
	print("HALATHA: entering idle\n");
	for(;;)
		sleep(10000);
}
