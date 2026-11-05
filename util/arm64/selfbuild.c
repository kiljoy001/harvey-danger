/*
 * Guest driver for util/arm64/test-selfbuild.py.
 *
 * The kernel starts this as /boot/init.  It compiles llvm-as.cpp with the
 * SDK's own clang++ (harvey-aarch64 target), links the object with the SDK's
 * ld.lld against the prebuilt ARM64 LLVM archives, runs the resulting
 * llvm-as on a small .ll file and compares the bitcode byte for byte with
 * the host build of the same revision.
 *
 * The runner links libc.a first (for bind/mkdir), so waitpid() would resolve
 * to the Plan 9 libc version that takes no arguments; use APEX wait4.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

extern int wait4(int, int *, int, void *);
extern unsigned long long nsec(void);

static int
run(const char *label, char *const argv[])
{
	pid_t pid;
	int st;
	unsigned long long t0, t1;

	fflush(stdout);
	fflush(stderr);
	t0 = nsec();
	pid = fork();
	if(pid == 0){
		execv(argv[0], argv);
		fprintf(stderr, "SB %s exec: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0){
		printf("SB %s fork: %s\n", label, strerror(errno));
		fflush(stdout);
		return -1;
	}
	if(wait4(pid, &st, 0, 0) < 0){
		printf("SB %s wait: %s\n", label, strerror(errno));
		fflush(stdout);
		return -1;
	}
	if(WIFSIGNALED(st)){
		printf("SB %s SIGNAL %d\n", label, WTERMSIG(st));
		fflush(stdout);
		return -1;
	}
	t1 = nsec();
	printf("SB %s EXIT %d TIME %llu ms\n", label, WEXITSTATUS(st),
	    (t1 - t0) / 1000000ULL);
	fflush(stdout);
	return WEXITSTATUS(st);
}

static void
compare(const char *guest, const char *host)
{
	unsigned char a[4096], b[4096];
	long off = 0;
	int fa, fb, na, nb, i;

	fa = open(guest, O_RDONLY);
	fb = open(host, O_RDONLY);
	if(fa < 0 || fb < 0){
		printf("SB BC-DIFF cannot open reference\n");
		fflush(stdout);
		return;
	}
	for(;;){
		na = read(fa, a, sizeof a);
		nb = read(fb, b, sizeof b);
		if(na < 0)
			na = 0;
		if(nb < 0)
			nb = 0;
		if(na != nb){
			printf("SB BC-DIFF short at %ld (%d vs %d)\n",
			    off, na, nb);
			fflush(stdout);
			break;
		}
		for(i = 0; i < na; i++)
			if(a[i] != b[i]){
				printf("SB BC-DIFF at %ld\n", off + i);
				fflush(stdout);
				close(fa);
				close(fb);
				return;
			}
		off += na;
		if(na == 0){
			printf("SB BC-MATCH %ld bytes\n", off);
			fflush(stdout);
			break;
		}
	}
	close(fa);
	close(fb);
}

int
main(void)
{
	char *cc = "/initramfs/sdk/bin/clang++";
	int r;

	{
		char *a[] = { cc, "--no-default-config",
		    "--config=/initramfs/etc/compile.cfg",
		    "-c", "/initramfs/sdk/l/src/llvm-as.cpp",
		    "-o", "/initramfs/llvm-as.o", 0 };
		r = run("compile", a);
	}
	if(r == 0){
		char *a[] = { cc, "--no-default-config",
		    "--config=/initramfs/etc/link.cfg",
		    "-o", "/initramfs/bin/llvm-as", 0 };
		r = run("link", a);
	}
	if(r == 0){
		char *a[] = { "/initramfs/bin/llvm-as",
		    "/initramfs/sdk/l/test.ll",
		    "-o", "/initramfs/guest.bc", 0 };
		r = run("llvm-as", a);
	}
	if(r == 0)
		compare("/initramfs/guest.bc", "/initramfs/sdk/l/host.bc");
	printf("SELFBUILD-DONE\n");
	fflush(stdout);
	return 0;
}
