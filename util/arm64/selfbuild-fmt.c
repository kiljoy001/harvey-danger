/* Guest driver: build the fmt library and a program with the shipped tools. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

extern int wait4(int, int *, int, void *);

static int
run(const char *label, char *const argv[])
{
	pid_t pid;
	int st;

	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if(pid == 0){
		execv(argv[0], argv);
		fprintf(stderr, "FMTLIB %s exec: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0 || wait4(pid, &st, 0, 0) < 0)
		return -1;
	if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){
		printf("FMTLIB %s FAIL %d\n", label,
		    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return -1;
	}
	printf("FMTLIB %s OK\n", label);
	fflush(stdout);
	return 0;
}

int
main(void)
{
	char *cc = "/initramfs/sdk/bin/clang++";
	char *ar = "/initramfs/sdk/bin/llvm-ar";
	char *cfg = "--config=/initramfs/sdk/bin/clang++.cfg";
	char *inc = "-I/initramfs/fmt/include";

	char *c1[] = { cc, "--no-default-config", cfg, "-std=c++17", inc,
	    "-c", "/initramfs/fmt/src/format.cc", "-o",
	    "/initramfs/format.o", 0 };
	char *c2[] = { cc, "--no-default-config", cfg, "-std=c++17", inc,
	    "-c", "/initramfs/t/fmtlibprobe.cpp", "-o",
	    "/initramfs/fmtlibprobe.o", 0 };
	char *a1[] = { ar, "rcs", "/initramfs/libfmt.a",
	    "/initramfs/format.o", 0 };
	char *linkv[] = { cc, "/initramfs/fmtlibprobe.o", "/initramfs/libfmt.a",
	    "--no-default-config", cfg, "-o", "/initramfs/bin/fmtprobe", 0 };
	char *r1[] = { "/initramfs/bin/fmtprobe", 0 };

	if(run("compile-format", c1) != 0 ||
	   run("compile-probe", c2) != 0 ||
	   run("archive", a1) != 0 ||
	   run("link", linkv) != 0 ||
	   run("run", r1) != 0)
		return 1;
	printf("FMTLIB-DONE\n");
	fflush(stdout);
	return 0;
}
