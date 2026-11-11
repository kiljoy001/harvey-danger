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

int
main(void)
{
	pthread_t t;
	struct pollfd pf;
	char buf[4096];
	size_t total;
	int n, m;

	setvbuf(stdout, NULL, _IONBF, 0);
	pipe(p);
	if(pthread_create(&t, 0, writer, 0) != 0){
		printf("WPOLL create-fail\n");
		return 1;
	}
	usleep(100000);

	pf.fd = p[1];
	pf.events = POLLOUT;
	pf.revents = 0;
	n = poll(&pf, 1, 100);
	printf("WPOLL-BUSY %d %d\n", n, pf.revents == 0);

	total = 0;
	while(total < (1 << 20)){
		m = read(p[0], buf, sizeof buf);
		if(m <= 0)
			break;
		total += m;
	}
	pthread_join(t, 0);

	pf.revents = 0;
	n = poll(&pf, 1, 100);
	printf("WPOLL-IDLE %d %d\n", n, (pf.revents & POLLOUT) != 0);
	close(p[0]);
	close(p[1]);
	return 0;
}
