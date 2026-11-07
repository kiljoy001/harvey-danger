/*
 * Guest driver: compile and run a C++ probe with the SDK clang++.
 *
 * CXX_SRC, CXX_STD, CXX_INC, CXX_DEF and CXX_OUT override the defaults, so
 * the same driver runs the self-contained cxxprobe.cpp and a header-only
 * library probe such as fmt.
 */
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
		fprintf(stderr, "CXX-GUEST %s exec: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0){
		printf("CXX-GUEST %s fork: %s\n", label, strerror(errno));
		return -1;
	}
	if(wait4(pid, &st, 0, 0) < 0){
		printf("CXX-GUEST %s wait: %s\n", label, strerror(errno));
		return -1;
	}
	if(WIFSIGNALED(st)){
		printf("CXX-GUEST %s SIGNAL %d\n", label, WTERMSIG(st));
		return -1;
	}
	if(WEXITSTATUS(st) != 0){
		printf("CXX-GUEST %s EXIT %d\n", label, WEXITSTATUS(st));
		return -1;
	}
	return 0;
}

int
main(void)
{
	char *cc = "/initramfs/sdk/bin/clang++";
	char *src = getenv("CXX_SRC");
	char *std = getenv("CXX_STD");
	char *inc = getenv("CXX_INC");
	char *def = getenv("CXX_DEF");
	char *extra = getenv("CXX_EXTRA");
	char *out = getenv("CXX_OUT");
	char *compile[16];
	char *probe[2];
	int n = 0;

	if(src == 0)
		src = "/initramfs/t/cxxprobe.cpp";
	if(std == 0)
		std = "-std=c++23";
	if(out == 0)
		out = "/initramfs/bin/cxxprobe";

	compile[n++] = cc;
	compile[n++] = "--no-default-config";
	compile[n++] = "--config=/initramfs/sdk/bin/clang++.cfg";
	compile[n++] = std;
	if(inc != 0)
		compile[n++] = inc;
	if(def != 0)
		compile[n++] = def;
	if(extra != 0)
		compile[n++] = extra;
	compile[n++] = src;
	compile[n++] = "-o";
	compile[n++] = out;
	compile[n] = 0;

	if(run("compile", compile) != 0)
		return 1;
	probe[0] = out;
	probe[1] = 0;
	if(run("probe", probe) != 0)
		return 1;
	printf("CXX-GUEST-DONE\n");
	fflush(stdout);
	return 0;
}
