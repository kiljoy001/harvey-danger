/*
 * Build a piece of LLVM on Harvey, with the native compiler.
 *
 * The harness writes ccargs.txt next to this program: one argument per line,
 * the full argv for compiling llvm/lib/Support/CommandLine.cpp.  This program
 * runs that compile, compiles a small driver, links both against the
 * prebuilt libLLVMSupport.a, and runs the result.
 *
 * Everything below happens in the guest:
 *
 *     clang++ -c CommandLine.cpp -o CommandLine.o
 *     clang++ -c cl.cpp -o cl.o
 *     clang++ cl.o CommandLine.o libLLVMSupport.a -o cl
 *     ./cl --count=7 --name=harvey
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define GUEST "/n/host/build/llvm-guest"
#define CC "/n/host/build/clang-harvey/clang++"
#define LLVM_INCLUDE "-I/n/host/build/llvm-build/include"
#define UTF "-I/n/host/build/llvm-src/llvm/include"

static int
run(char *const av[])
{
	int pid, st;

	pid = fork();
	if(pid == 0){
		execv(av[0], av);
		perror(av[0]);
		_exit(127);
	}
	if(pid < 0)
		return -1;
	st = 0;
	waitpid(pid, &st, 0);
	return st;
}

/*
 * Read ccargs.txt into buf and split it into argv on newlines.  The file is
 * small (a compiler command line), so a static buffer is enough.
 */
static int
load_ccargs(char *buf, size_t size, char **av, int max)
{
	int fd, n, k, argc;
	char *p;

	fd = open(GUEST "/ccargs.txt", O_RDONLY);
	if(fd < 0)
		return 0;
	n = 0;
	while(n < (int)size - 1){
		k = read(fd, buf + n, size - 1 - n);
		if(k <= 0)
			break;
		n += k;
	}
	close(fd);
	buf[n] = 0;

	argc = 0;
	for(p = buf; *p != 0 && argc < max - 1; ){
		av[argc++] = p;
		while(*p != 0 && *p != '\n')
			p++;
		while(*p == '\n')
			*p++ = 0;
	}
	av[argc] = 0;
	return argc;
}

int
main(void)
{
	static char buf[8192];
	char *ccargs[128];
	char *a2[16], *a3[16], *a4[8];
	int st;

	if(chdir(GUEST) < 0){
		perror("chdir");
		return 1;
	}
	setenv("TMPDIR", GUEST "/tmp", 1);

	if(load_ccargs(buf, sizeof buf, ccargs, 128) < 2){
		fprintf(stderr, "selfhost: cannot read " GUEST "/ccargs.txt\n");
		return 1;
	}
	fprintf(stderr, "== compile llvm/lib/Support/CommandLine.cpp on Harvey\n");
	st = run(ccargs);
	fprintf(stderr, "== CommandLine.cpp status %d\n", st);
	if(st)
		return 1;

	fprintf(stderr, "== compile cl.cpp on Harvey\n");
	a2[0] = CC; a2[1] = "-std=c++17"; a2[2] = "-fno-exceptions";
	a2[3] = "-fno-rtti"; a2[4] = LLVM_INCLUDE; a2[5] = UTF;
	a2[6] = "-c"; a2[7] = GUEST "/cl.cpp"; a2[8] = "-o";
	a2[9] = GUEST "/cl.o"; a2[10] = 0;
	st = run(a2);
	fprintf(stderr, "== cl.cpp status %d\n", st);
	if(st)
		return 1;

	fprintf(stderr, "== link with the prebuilt LLVMSupport\n");
	a3[0] = CC; a3[1] = "-o"; a3[2] = GUEST "/cl";
	a3[3] = GUEST "/cl.o"; a3[4] = GUEST "/CommandLine.o";
	a3[5] = "/n/host/build/llvm-build/lib/libLLVMSupport.a";
	a3[6] = "/n/host/build/llvm-build/lib/libLLVMDemangle.a";
	a3[7] = "-fno-exceptions"; a3[8] = "-fno-rtti"; a3[9] = 0;
	st = run(a3);
	fprintf(stderr, "== link status %d\n", st);
	if(st)
		return 1;

	a4[0] = GUEST "/cl"; a4[1] = "--count=7"; a4[2] = "--name=harvey";
	a4[3] = 0;
	execv(a4[0], a4);
	perror("execv cl");
	return 1;
}
