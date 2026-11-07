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
#include <stdlib.h>
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

static long
readall(const char *path, char *buf, long max)
{
	int fd, n;
	long off = 0;

	fd = open(path, O_RDONLY);
	if(fd < 0)
		return -1;
	while(off < max - 1 && (n = read(fd, buf + off, max - 1 - off)) > 0)
		off += n;
	close(fd);
	buf[off] = 0;
	return off;
}

static long
copy_skipping_moduleid(const char *in, long n, char *out, long max)
{
	long i = 0, o = 0;

	while(i < n && o < max - 1){
		long j = i;
		while(j < n && in[j] != '\n')
			j++;
		if(!(j - i >= 11 && strncmp(in + i, "; ModuleID =", 11) == 0)){
			long k;
			for(k = i; k < j && o < max - 1; k++)
				out[o++] = in[k];
			if(j < n && o < max - 1)
				out[o++] = '\n';
		}
		i = j + 1;
	}
	out[o] = 0;
	return o;
}

static void
compare(const char *guest, const char *host)
{
	static char ga[65536], ha[65536], g[65536], h[65536];
	long gn, hn, g2, h2;

	gn = readall(guest, ga, sizeof ga);
	hn = readall(host, ha, sizeof ha);
	if(gn < 0 || hn < 0){
		printf("SB MATCH cannot open reference\n");
		fflush(stdout);
		return;
	}
	g2 = copy_skipping_moduleid(ga, gn, g, sizeof g);
	h2 = copy_skipping_moduleid(ha, hn, h, sizeof h);
	if(g2 != h2 || memcmp(g, h, g2) != 0){
		printf("SB DIFF %ld vs %ld bytes\n", g2, h2);
		fflush(stdout);
		return;
	}
	printf("SB MATCH %ld bytes\n", g2);
	fflush(stdout);
}

static void
dump(const char *path)
{
	char b[512];
	int fd, n;

	fd = open(path, O_RDONLY);
	printf("SB DUMP %s:\n", path);
	if(fd >= 0){
		while((n = read(fd, b, sizeof b)) > 0)
			write(1, b, n);
		close(fd);
	}
	printf("\nSB DUMP-END\n");
	fflush(stdout);
}

int
main(void)
{
	char *cc = "/initramfs/sdk/bin/clang++";
	char *tool = getenv("SB_TOOL");
	char src[128], obj[128], bin[128], guest[128], host[128];
	int r, isdis;

	if(tool == 0)
		tool = "llvm-as";
	isdis = strcmp(tool, "llvm-dis") == 0;
	snprintf(src, sizeof src, "/initramfs/sdk/l/src/%s.cpp", tool);
	snprintf(obj, sizeof obj, "/initramfs/%s.o", tool);
	snprintf(bin, sizeof bin, "/initramfs/bin/%s", tool);
	snprintf(guest, sizeof guest, isdis ? "/initramfs/guest.ll"
	    : "/initramfs/guest.bc");
	snprintf(host, sizeof host, isdis ? "/initramfs/sdk/l/host.ll"
	    : "/initramfs/sdk/l/host.bc");

	{
		char *a[] = { cc, "--no-default-config",
		    "--config=/initramfs/etc/compile.cfg",
		    "-c", src, "-o", obj, 0 };
		r = run("compile", a);
	}
	if(r == 0){
		char *a[] = { cc, "--no-default-config",
		    "--config=/initramfs/etc/link.cfg",
		    "-o", bin, 0 };
		r = run("link", a);
	}
	if(r == 0){
		char *a[] = { bin,
		    isdis ? "/initramfs/sdk/l/test.bc" : "/initramfs/sdk/l/test.ll",
		    "-o", guest, 0 };
		r = run(tool, a);
	}
	if(r == 0)
		compare(guest, host);
	printf("SELFBUILD-DONE\n");
	fflush(stdout);
	return 0;
}
