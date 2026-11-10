#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <poll.h>
#include <sys/wait.h>

static void
test_timeout(void)
{
	fd_set r;
	struct timeval tv = {0, 200000};
	int p[2], n;

	printf("SEL-TIMEOUT-ENTER\n");
	pipe(p);
	FD_ZERO(&r);
	FD_SET(p[0], &r);
	n = select(p[0]+1, &r, 0, 0, &tv);
	printf("SEL-TIMEOUT %d %d\n", n, n == 0 && !FD_ISSET(p[0], &r));
	close(p[0]);
	close(p[1]);
}

static void
test_read(void)
{
	int p[2], n, m;
	pid_t pid;
	char c = 0;
	fd_set r;
	struct timeval tv = {1, 0};

	pipe(p);
	pid = fork();
	if(pid == 0){
		usleep(50000);
		write(p[1], "Z", 1);
		_exit(0);
	}
	close(p[1]);
	FD_ZERO(&r);
	FD_SET(p[0], &r);
	n = select(p[0]+1, &r, 0, 0, &tv);
	printf("SEL-READY %d %d\n", n, n == 1 && FD_ISSET(p[0], &r));
	m = read(p[0], &c, 1);
	printf("SEL-BYTE %d %c\n", m, c);
	waitpid(pid, 0, 0);
	close(p[0]);
}

static void
test_poll(void)
{
	int p[2], n, m;
	char c = 0;
	struct pollfd pf;

	pipe(p);
	pf.fd = p[0];
	pf.events = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 100);
	printf("POLL-TIMEOUT %d %d\n", n, n == 0);

	/* Timeout first, then the same (still open) pipe becomes readable. */
	usleep(30000);
	write(p[1], "Q", 1);
	pf.revents = 0;
	n = poll(&pf, 1, 1000);
	printf("POLL-READY %d %d\n", n, n == 1 && (pf.revents & POLLIN));
	m = read(p[0], &c, 1);
	printf("POLL-BYTE %d %c %02x\n", m, c, (unsigned char)c);
	close(p[0]);
	close(p[1]);
}

static void
test_poll_child(void)
{
	int p[2], n, m;
	pid_t pid;
	char c = 0;
	struct pollfd pf;

	pipe(p);
	pid = fork();
	if(pid == 0){
		usleep(30000);
		write(p[1], "W", 1);
		_exit(0);
	}
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 1000);
	printf("POLL-CHILD %d %d\n", n, n == 1 && (pf.revents & POLLIN));
	m = read(p[0], &c, 1);
	printf("POLL-CHILDBYTE %d %c\n", m, c);
	waitpid(pid, 0, 0);
	close(p[0]);
}

static void
test_eof(void)
{
	int p[2], n, m;
	char c;
	struct pollfd pf;

	pipe(p);
	close(p[1]);
	pf.fd = p[0];
	pf.events = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 1000);
	printf("POLL-EOF %d %d %d\n", n, n == 1, (pf.revents & POLLHUP) != 0);
	m = read(p[0], &c, 1);
	printf("POLL-EOFRD %d\n", m);
	close(p[0]);
}

static void
test_file(void)
{
	int fd, n, m;
	char c;
	struct pollfd pf;

	fd = open("/selprobe.tmp", O_RDWR|O_CREAT|O_TRUNC, 0666);
	write(fd, "F", 1);
	lseek(fd, 0, 0);
	pf.fd = fd;
	pf.events = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 100);
	printf("POLL-FILE %d %d\n", n, n == 1 && (pf.revents & POLLIN));
	m = read(fd, &c, 1);
	printf("POLL-FILEBYTE %d %c\n", m, c);
	close(fd);
	unlink("/selprobe.tmp");
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("SELPROBE-START\n");
	test_timeout();
	test_read();
	test_poll();
	test_poll_child();
	test_eof();
	test_file();
	printf("SELPROBE-DONE\n");
	return 0;
}
