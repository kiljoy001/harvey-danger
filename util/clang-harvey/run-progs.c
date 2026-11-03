/*
 * Build and run the differential-execution programs inside the guest.
 *
 * Runs diffexec.build through hbuild first, then executes every program named
 * in progs.list, writing each one's stdout and stderr beside it (*.out,
 * *.err) and reporting its exit status.  Everything is relative to
 * /n/host/build/diffexec so the same list serves the host run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static int
spawn(char *const av[])
{
	int pid = fork();
	if (pid == 0) {
		execv(av[0], av);
		perror(av[0]);
		_exit(127);
	}
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int
runone(const char *path)
{
	char out[512], err[512];
	snprintf(out, sizeof out, "%s.out", path);
	snprintf(err, sizeof err, "%s.err", path);

	int pid = fork();
	if (pid == 0) {
		int fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		int fe = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fo < 0 || fe < 0) {
			perror("open");
			_exit(126);
		}
		dup2(fo, 1);
		dup2(fe, 2);
		close(fo);
		close(fe);
		char *av[] = { (char *)path, 0 };
		execv(path, av);
		perror(path);
		_exit(127);
	}
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int
main(void)
{
	if (chdir("/n/host/build/diffexec") < 0) {
		perror("chdir");
		return 1;
	}
	setenv("TMPDIR", "/n/host/build/diffexec/tmp", 1);

	{
		char *build[] = { "/n/host/build/llvm-guest/hbuild", "-j", "4",
		                  "-f", "/n/host/build/llvm-guest/diffexec.build", 0 };
		fprintf(stderr, "== hbuild diffexec.build\n");
		int st = spawn(build);
		fprintf(stderr, "== build status %d\n", st);
		if (st != 0)
			return 1;
	}

	FILE *f = fopen("progs.list", "r");
	if (f == nil) {
		perror("progs.list");
		return 1;
	}
	char line[512];
	while (fgets(line, sizeof line, f) != nil) {
		char *nl = strchr(line, '\n');
		if (nl != nil)
			*nl = 0;
		if (line[0] == 0)
			continue;
		int st = runone(line);
		fprintf(stderr, "== run %s status %d\n", line, st);
	}
	fclose(f);
	return 0;
}
