#include <u.h>
#include <libc.h>

void
main(int argc, char *argv[])
{
	print("Hello from native Plan 9 C on Harvey OS!\n");
	print("Compiled natively with Tiny C Compiler (TCC)!\n");
	print("PID: %d\n", getpid());
	exits(nil);
}
