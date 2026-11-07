/* Guest driver: compile and run the C++ runtime probe with the SDK clang++. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
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
	char *compile[] = {
		cc, "--no-default-config",
		"--config=/initramfs/sdk/bin/clang++.cfg",
		"-std=c++20", "/initramfs/t/cxxprobe.cpp",
		"-o", "/initramfs/bin/cxxprobe", 0,
	};
	char *probe[] = { "/initramfs/bin/cxxprobe", 0 };

	if(run("compile", compile) != 0)
		return 1;
	if(run("probe", probe) != 0)
		return 1;
	printf("CXX-GUEST-DONE\n");
	fflush(stdout);
	return 0;
}
