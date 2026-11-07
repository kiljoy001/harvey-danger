/* Guest driver: exercise the shipped clang, llvm-ar and llvm-ranlib. */
#include <stdio.h>
#include <errno.h>
#include <string.h>
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
	pid = fork();
	if(pid == 0){
		execv(argv[0], argv);
		fprintf(stderr, "ART %s: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0)
		return -1;
	if(wait4(pid, &st, 0, 0) < 0)
		return -1;
	if(!WIFEXITED(st) || WEXITSTATUS(st) != 0){
		printf("ART %s FAIL %d\n", label,
		    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return -1;
	}
	printf("ART %s OK\n", label);
	return 0;
}

int
main(void)
{
	char *cc[] = { "/initramfs/sdk/bin/clang", "--no-default-config",
	    "--config=/initramfs/sdk/bin/clang.cfg", "-c",
	    "/initramfs/t/x.c", "-o", "/initramfs/x.o", 0 };
	char *ac[] = { "/initramfs/sdk/bin/llvm-ar", "rcs",
	    "/initramfs/libx.a", "/initramfs/x.o", 0 };
	char *at[] = { "/initramfs/sdk/bin/llvm-ar", "t",
	    "/initramfs/libx.a", 0 };
	char *rl[] = { "/initramfs/sdk/bin/llvm-ranlib",
	    "/initramfs/libx.a", 0 };

	if(run("cc", cc) != 0 ||
	   run("ar-create", ac) != 0 ||
	   run("ar-list", at) != 0 ||
	   run("ranlib", rl) != 0)
		return 1;
	printf("ART-DONE\n");
	fflush(stdout);
	return 0;
}
