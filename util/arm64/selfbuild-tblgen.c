/*
 * Guest driver for the llvm-tblgen self-build on Harvey/ARM64.
 *
 * Reads a manifest of TableGen source files, compiles each one with the
 * SDK's clang++, links the objects with the SDK's ld.lld against the
 * prebuilt ARM64 LLVM archives, then runs the result on ValueTypes.td and
 * compares the generated fragment with the host build of the same revision.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

extern int wait4(int, int *, int, void *);
extern unsigned long long nsec(void);

#define MAXTU 200

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
		fprintf(stderr, "TGEN %s exec: %s\n", label, strerror(errno));
		_exit(127);
	}
	if(pid < 0){
		printf("TGEN %s fork: %s\n", label, strerror(errno));
		fflush(stdout);
		return -1;
	}
	if(wait4(pid, &st, 0, 0) < 0){
		printf("TGEN %s wait: %s\n", label, strerror(errno));
		fflush(stdout);
		return -1;
	}
	if(WIFSIGNALED(st)){
		printf("TGEN %s SIGNAL %d\n", label, WTERMSIG(st));
		fflush(stdout);
		return -1;
	}
	return WEXITSTATUS(st);
}

/*
 * Run one compile in the background.  The caller reaps it with wait4();
 * this lets several clang++ processes run on different CPUs.
 */
static pid_t
spawn(char *const argv[])
{
	pid_t pid;

	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if(pid == 0){
		execv(argv[0], argv);
		fprintf(stderr, "TGEN compile exec: %s\n", strerror(errno));
		_exit(127);
	}
	if(pid < 0){
		printf("TGEN compile fork: %s\n", strerror(errno));
		fflush(stdout);
	}
	return pid;
}

static int
reap(pid_t pid)
{
	int st;

	if(wait4(pid, &st, 0, 0) < 0){
		printf("TGEN compile wait: %s\n", strerror(errno));
		fflush(stdout);
		return -1;
	}
	if(WIFSIGNALED(st)){
		printf("TGEN compile SIGNAL %d\n", WTERMSIG(st));
		fflush(stdout);
		return -1;
	}
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
		printf("TGEN VT-DIFF cannot open reference\n");
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
			printf("TGEN VT-DIFF short at %ld (%d vs %d)\n",
			    off, na, nb);
			fflush(stdout);
			break;
		}
		for(i = 0; i < na; i++)
			if(a[i] != b[i]){
				printf("TGEN VT-DIFF at %ld\n", off + i);
				fflush(stdout);
				close(fa);
				close(fb);
				return;
			}
		off += na;
		if(na == 0){
			printf("TGEN VT-MATCH %ld bytes\n", off);
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
	static char paths[MAXTU][512], obj[MAXTU][64];
	static int ok[MAXTU];
	static pid_t pids[MAXTU];
	char *cc = "/initramfs/sdk/bin/clang++";
	char line[512], *objs[MAXTU];
	char *v[4 + MAXTU + 8];
	int n, nobj, i, r = 0, limit, jobs, running, done, started;
	char *lim, *jb;
	unsigned long long t0, t1;
	FILE *f;

	mkdir("/initramfs/tgobjs", 0777);
	f = fopen("/initramfs/sdk/l/tg.list", "r");
	if(f == 0){
		printf("TGEN no manifest: %s\n", strerror(errno));
		printf("TGEN-DONE\n");
		fflush(stdout);
		return 0;
	}
	lim = getenv("TGEN_LIMIT");
	limit = lim ? atoi(lim) : -1;
	jb = getenv("TGEN_JOBS");
	jobs = jb ? atoi(jb) : 1;
	if(jobs < 1)
		jobs = 1;
	if(jobs > 8)
		jobs = 8;
	n = 0;
	while(fgets(line, sizeof line, f) != 0){
		int len = strlen(line);

		while(len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		if(len == 0)
			continue;
		if(limit >= 0 && n >= limit)
			break;
		if(n >= MAXTU)
			break;
		snprintf(paths[n], sizeof paths[n], "%s", line);
		snprintf(obj[n], sizeof obj[n], "/initramfs/tgobjs/%d.o", n);
		n++;
	}
	fclose(f);

	t0 = nsec();
	nobj = 0;
	running = done = started = 0;
	while(done < n){
		while(r == 0 && running < jobs && started < n){
			char *a[] = { cc, "--no-default-config",
			    "--config=/initramfs/etc/tg-compile.cfg",
			    "-c", paths[started], "-o", obj[started], 0 };
			pid_t pid = spawn(a);

			if(pid < 0){
				r = -1;
				break;
			}
			pids[started] = pid;
			started++;
			running++;
		}
		if(running == 0)
			break;
		/* Reap the oldest compile; equal-sized TUs finish roughly in order. */
		i = done;
		done++;
		running--;
		if(reap(pids[i]) == 0)
			ok[i] = 1;
		else{
			printf("TGEN FAIL %s\n", paths[i]);
			fflush(stdout);
			r = -1;
		}
		if((done % 5) == 0 || done == n){
			printf("TGEN %d/%d\n", done, n);
			fflush(stdout);
		}
	}
	while(done < started){
		if(reap(pids[done]) == 0)
			ok[done] = 1;
		done++;
	}
	for(i = 0; i < n; i++)
		if(ok[i])
			objs[nobj++] = obj[i];
	t1 = nsec();
	printf("TGEN compiled %d files in %llu ms\n", nobj,
	    (t1 - t0) / 1000000ULL);
	fflush(stdout);
	if(r != 0 || limit >= 0 || nobj == 0)
		goto done;

	v[0] = cc;
	i = 1;
	/* Objects first: the config's archive list is expanded where --config
	 * appears, and static archives must follow the objects that use them. */
	for(n = 0; n < nobj; n++)
		v[i++] = objs[n];
	v[i++] = "--no-default-config";
	v[i++] = "--config=/initramfs/etc/tg-link.cfg";
	v[i++] = "-o";
	v[i++] = "/initramfs/bin/llvm-tblgen";
	v[i] = 0;
	r = run("link", v);
	if(r != 0)
		goto done;
	{
		char *a[] = { "/initramfs/bin/llvm-tblgen", "-gen-vt",
		    "-I", "/initramfs/sdk/l/include",
		    "/initramfs/sdk/l/include/llvm/CodeGen/ValueTypes.td",
		    "-o", "/initramfs/genvt.inc", 0 };
		r = run("tblgen", a);
	}
	if(r == 0)
		compare("/initramfs/genvt.inc", "/initramfs/sdk/l/host-vt.inc");
done:
	if(limit >= 0)
		printf("TGEN-LIMIT-DONE\n");
	else
		printf("TGEN-DONE\n");
	fflush(stdout);
	return 0;
}
