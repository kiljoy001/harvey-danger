#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "9ring.h"

enum {
	Operations = 100000,
};

typedef struct ThreadTest ThreadTest;
struct ThreadTest {
	P9Channel channel;
	atomic_int failed;
};

static void *
tsubmit(void *argument)
{
	ThreadTest *test;
	P9Slot slot;
	int i;

	test = argument;
	memset(&slot, 0, sizeof(slot));
	for(i = 0; i < Operations; i++){
		slot.tag = (u16)i;
		slot.offset = (u64)i;
		while(p9_tring_submit(&test->channel.t_ring, &slot) < 0)
			sched_yield();
	}
	return NULL;
}

static void *
tconsume(void *argument)
{
	ThreadTest *test;
	P9Slot slot;
	int i;

	test = argument;
	for(i = 0; i < Operations; i++){
		while(p9_tring_consume(&test->channel.t_ring, &slot) == 0)
			sched_yield();
		if(slot.tag != (u16)i || slot.offset != (u64)i)
			atomic_store(&test->failed, 1);
	}
	return NULL;
}

static void *
rcomplete(void *argument)
{
	ThreadTest *test;
	P9Comp comp;
	int i;

	test = argument;
	memset(&comp, 0, sizeof(comp));
	for(i = 0; i < Operations; i++){
		comp.tag = (u16)i;
		comp.result = (u64)i;
		while(p9_rring_complete(&test->channel.r_ring, &comp) < 0)
			sched_yield();
	}
	return NULL;
}

static void *
rpoll(void *argument)
{
	ThreadTest *test;
	P9Comp comp;
	int i;

	test = argument;
	for(i = 0; i < Operations; i++){
		while(p9_rring_poll(&test->channel.r_ring, &comp) == 0)
			sched_yield();
		if(comp.tag != (u16)i || comp.result != (u64)i)
			atomic_store(&test->failed, 1);
	}
	return NULL;
}

int
main(void)
{
	ThreadTest test;
	pthread_t threads[4];
	int i;

	memset(&test, 0, sizeof(test));
	if(pthread_create(&threads[0], NULL, tsubmit, &test) != 0 ||
	   pthread_create(&threads[1], NULL, tconsume, &test) != 0 ||
	   pthread_create(&threads[2], NULL, rcomplete, &test) != 0 ||
	   pthread_create(&threads[3], NULL, rpoll, &test) != 0)
		return 1;
	for(i = 0; i < 4; i++)
		if(pthread_join(threads[i], NULL) != 0)
			return 1;
	if(atomic_load(&test.failed) != 0)
		return 1;
	printf("KERNEL-TRANSPORT-THREAD PASS\n");
	return 0;
}
