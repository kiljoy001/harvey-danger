#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define PG 4096
#define BIGPG (2 * 1024 * 1024)

static volatile unsigned char *shared;

static void
childstatus(const char *tag, pid_t pid)
{
	int st;

	st = 0;
	if(waitpid(pid, &st, 0) < 0){
		printf("%s -1\n", tag);
		return;
	}
	printf("%s %d %d\n", tag, WIFSIGNALED(st),
	    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

static void
try_access(void *p, int writeit, const char *tag)
{
	pid_t pid;

	pid = fork();
	if(pid == 0){
		if(writeit)
			*(volatile unsigned char *)p = 0x5a;
		else
			(void)*(volatile unsigned char *)p;
		_exit(0);
	}
	if(pid < 0){
		printf("%s fork-fail %d\n", tag, errno);
		return;
	}
	childstatus(tag, pid);
}

static void *
threadfn(void *arg)
{
	(void)arg;
	usleep(200000);
	if(shared != nil && shared[0] == 'T' && shared[1] == 'H')
		printf("MMAP-THREAD-SAW 1\n");
	else
		printf("MMAP-THREAD-SAW 0\n");
	return 0;
}

static void
test_anon(void)
{
	unsigned char *p;
	int n;

	p = mmap(nil, PG, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	printf("MMAP-ANON %d\n", p != MAP_FAILED);
	if(p == MAP_FAILED)
		return;
	p[0] = 'A';
	p[PG-1] = 'Z';
	printf("MMAP-RW %d\n", p[0] == 'A' && p[PG-1] == 'Z');

	/* read-only: a write must not complete, a read must still work */
	n = mprotect(p, PG, PROT_READ);
	printf("MPROT-READ-CALL %d\n", n == 0);
	printf("MPROT-READ-BYTE %d\n", p[0] == 'A');
	try_access(p, 1, "MPROT-WRITE");

	/* PROT_NONE: neither read nor write may complete */
	n = mprotect(p, PG, PROT_NONE);
	printf("MPROT-NONE-CALL %d\n", n == 0);
	try_access(p, 0, "MPROT-NONEREAD");

	/* and back to writable */
	n = mprotect(p, PG, PROT_READ|PROT_WRITE);
	printf("MPROT-RW-CALL %d\n", n == 0);
	p[1] = 'B';
	printf("MPROT-RW-BYTE %d\n", p[1] == 'B');

	munmap(p, PG);
	try_access(p, 0, "MUNMAP-READ");
}

static void
test_fixed(void)
{
	unsigned char *want, *p;

	want = (unsigned char *)0x1000000000ULL;	/* 64 GiB, 2 MiB aligned */
	p = mmap(want, BIGPG, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0);
	printf("MMAP-FIXED %d\n", p == want);
	if(p == MAP_FAILED)
		return;
	p[0] = 'F';
	printf("MMAP-FIXEDBYTE %d\n", p[0] == 'F');
	munmap(p, BIGPG);
}

static void
test_thread(void)
{
	pthread_t t;
	unsigned char *p;

	if(pthread_create(&t, 0, threadfn, 0) != 0){
		printf("MMAP-THREAD-CREATE 0\n");
		return;
	}
	/* Mapping made after the thread exists must be visible to it. */
	p = mmap(nil, PG, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if(p == MAP_FAILED){
		printf("MMAP-THREAD-MAP 0\n");
		pthread_join(t, 0);
		return;
	}
	p[0] = 'T';
	p[1] = 'H';
	shared = p;
	pthread_join(t, 0);
	munmap(p, PG);
}

int
main(void)
{
	setvbuf(stdout, nil, _IONBF, 0);
	printf("MMAP-START\n");
	test_anon();
	test_fixed();
	test_thread();
	printf("MMAP-DONE\n");
	return 0;
}
