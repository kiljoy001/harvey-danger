/*
 * hbuild: a staged, parallel build runner for Harvey.
 *
 * The manifest is a list of commands, one per line, with a line "---"
 * between stages.  Arguments are separated by spaces; nothing is quoted and
 * no shell syntax is understood, which matches what the generator emits.
 * Each stage runs up to -j children at once; the next stage starts when the
 * current one has finished.
 *
 * This is deliberately not a dependency graph.  It is enough for the shape
 * LLVM's static libraries and tools have: every compile, then every
 * archive, then the link.  Running several children at once overlaps the
 * 9P reads and process starts even when the guest has one vCPU.
 *
 *     hbuild [-j N] [-f manifest] [-q]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAXJOBS 64

struct cmd {
	char **argv;
};

struct stage {
	struct cmd *cmds;
	int n, cap;
};

static struct stage *stages;
static int nstages, stages_cap;

static void
oom(void)
{
	fprintf(stderr, "hbuild: out of memory\n");
	exit(1);
}

static void *
xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);
	if (q == nil)
		oom();
	return q;
}

static struct stage *
newstage(void)
{
	if (nstages == stages_cap) {
		stages_cap = stages_cap ? stages_cap * 2 : 8;
		stages = xrealloc(stages, stages_cap * sizeof *stages);
	}
	memset(&stages[nstages], 0, sizeof stages[nstages]);
	return &stages[nstages++];
}

static void
addcmd(struct stage *st, char **argv)
{
	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 32;
		st->cmds = xrealloc(st->cmds, st->cap * sizeof *st->cmds);
	}
	st->cmds[st->n++].argv = argv;
}

static char *
readfile(const char *path)
{
	int fd;
	ssize_t n;
	size_t len = 0, cap = 1 << 16;
	char *buf = xrealloc(nil, cap);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "hbuild: %s: %s\n", path, strerror(errno));
		return nil;
	}
	while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
		len += n;
		if (len + 1 == cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
	}
	close(fd);
	buf[len] = 0;
	return buf;
}

/*
 * Split one line in place on spaces and tabs.  The generator writes plain
 * paths, so there is no quoting to honour.
 */
static char **
split(char *s)
{
	char **av;
	int n = 0, cap = 16;

	av = xrealloc(nil, cap * sizeof *av);
	for (;;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == 0)
			break;
		if (n + 2 > cap) {
			cap *= 2;
			av = xrealloc(av, cap * sizeof *av);
		}
		av[n++] = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
		if (*s)
			*s++ = 0;
	}
	av[n] = 0;
	return av;
}

static int
parse(const char *path)
{
	char *buf, *p, *line, *nl;
	struct stage *st;

	buf = readfile(path);
	if (buf == nil)
		return -1;
	st = newstage();

	for (p = buf; *p != 0; p = nl ? nl + 1 : p + strlen(p)) {
		line = p;
		nl = strchr(p, '\n');
		if (nl) {
			*nl = 0;
			if (nl > line && nl[-1] == '\r')
				nl[-1] = 0;
		}
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == 0 || *line == '#')
			continue;
		if (strcmp(line, "---") == 0) {
			st = newstage();
			continue;
		}
		addcmd(st, split(line));
	}
	/* The file always ends with a newline; drop a trailing empty stage. */
	if (nstages > 0 && stages[nstages - 1].n == 0)
		nstages--;
	return 0;
}

static int
runstage(struct stage *st, int jobs, int quiet)
{
	struct { pid_t pid; int idx; } run[MAXJOBS];
	int i = 0, running = 0, failed = 0, done = 0;

	memset(run, 0, sizeof run);
	while (i < st->n || running > 0) {
		while (i < st->n && running < jobs) {
			pid_t pid = fork();
			if (pid == 0) {
				execv(st->cmds[i].argv[0], st->cmds[i].argv);
				fprintf(stderr, "hbuild: %s: %s\n",
				    st->cmds[i].argv[0], strerror(errno));
				_exit(127);
			}
			if (pid < 0) {
				fprintf(stderr, "hbuild: fork: %s\n", strerror(errno));
				failed++;
				i++;
				continue;
			}
			run[running].pid = pid;
			run[running].idx = i++;
			running++;
		}
		if (running == 0)
			break;
		{
			int status;
			pid_t pid = waitpid(-1, &status, 0);
			int k;

			if (pid < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "hbuild: wait: %s\n", strerror(errno));
				return failed + 1;
			}
			for (k = 0; k < running; k++)
				if (run[k].pid == pid)
					break;
			if (k == running)
				continue;
			done++;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				failed++;
				fprintf(stderr, "hbuild: FAILED: %s\n",
				    st->cmds[run[k].idx].argv[0]);
			}
			run[k] = run[--running];
		}
	}
	if (!quiet && st->n > 0)
		fprintf(stderr, "hbuild: stage done: %d/%d (%d failed)\n",
		    done, st->n, failed);
	return failed;
}

int
main(int argc, char **argv)
{
	const char *manifest = "build.txt";
	int jobs = 4, i, s, failed = 0, quiet = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
			manifest = argv[++i];
		else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
			jobs = atoi(argv[++i]);
		else if (strcmp(argv[i], "-q") == 0)
			quiet = 1;
		else {
			fprintf(stderr,
			    "usage: hbuild [-j N] [-f manifest] [-q]\n");
			return 2;
		}
	}
	if (jobs < 1)
		jobs = 1;
	if (jobs > MAXJOBS)
		jobs = MAXJOBS;

	if (parse(manifest) < 0)
		return 1;

	for (s = 0; s < nstages; s++) {
		int n = runstage(&stages[s], jobs, quiet);
		failed += n;
		if (n != 0 && !quiet)
			fprintf(stderr, "hbuild: stopping after stage %d\n", s + 1);
		if (n != 0)
			break;
	}
	return failed ? 1 : 0;
}
