#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

static int p[2];

static void *
writer(void *arg)
{
	char *b;
	(void)arg;
	b = malloc(1 << 20);
	memset(b, 'W', 1 << 20);
	write(p[1], b, 1 << 20);
	free(b);
	return 0;
}

static int
pollout(int fd)
{
	struct pollfd pf;

	pf.fd = fd;
	pf.events = POLLOUT;
	pf.revents = 0;
	if(poll(&pf, 1, 200) != 1)
		return 0;
	return (pf.revents & POLLOUT) != 0;
}

static void
test_blocked(void)
{
	pthread_t t;
	char buf[4096];
	size_t total;
	int n, m;

	pipe(p);
	if(pthread_create(&t, 0, writer, 0) != 0){
		printf("WPOLL create-fail\n");
		return;
	}
	usleep(100000);

	/* First POLLOUT enables the APEX write-behind queue, so the answer
	 * is "room in the queue", not "the pipe is full". */
	n = pollout(p[1]);
	printf("WPOLL-BUF %d %d\n", n, n);

	total = 0;
	while(total < (1 << 20)){
		m = read(p[0], buf, sizeof buf);
		if(m <= 0)
			break;
		total += m;
	}
	pthread_join(t, 0);
	printf("WPOLL-IDLE %d %d\n", pollout(p[1]), pollout(p[1]));
	close(p[0]);
	close(p[1]);
}

static void
test_round(void)
{
	char buf[4];
	int n, m;

	pipe(p);
	if(!pollout(p[1]))
		printf("WQ-ENABLE 0\n");
	n = write(p[1], "OK", 2);
	m = read(p[0], buf, 2);
	printf("WQ-ROUND %d %d %d\n", n, m,
	    m == 2 && buf[0] == 'O' && buf[1] == 'K');
	close(p[0]);
	close(p[1]);
}

static void
test_close(void)
{
	char buf[4];
	int n, m;

	pipe(p);
	if(!pollout(p[1]))
		printf("WQ-ENABLE 0\n");
	n = write(p[1], "CL", 2);
	close(p[1]);			/* must flush the queue */
	m = read(p[0], buf, 2);
	printf("WQ-CLOSE %d %d %d\n", n, m,
	    m == 2 && buf[0] == 'C' && buf[1] == 'L');
	close(p[0]);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	test_blocked();
	test_round();
	test_close();
	printf("WPOLL-DONE\n");
	return 0;
}
