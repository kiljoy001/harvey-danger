/* Host tests for p9 primitives and the OSI adapter surface. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../p9.h"
#include "../osi_wifi.h"

/* ---- msg queue via procs ---- */
static P9MsgQ q1;
static uint8_t q1storage[8 * sizeof(int)];
static volatile int consumer_sum;
static volatile int consumer_done;

static void
consumer(void *arg)
{
	int i, v;
	(void)arg;
	for (i = 0; i < 20; i++) {
		assert(p9_msgq_recv(&q1, &v, P9_BLOCKING) == 1);
		assert(v == i);
		consumer_sum += v;
	}
	consumer_done = 1;
}

static void
test_msgq_procs(void)
{
	int i;
	p9_msgq_init(&q1, q1storage, 8, sizeof(int));
	assert(p9_proc_create("consumer", consumer, NULL) != NULL);
	for (i = 0; i < 20; i++)
		assert(p9_msgq_send(&q1, &i, P9_BLOCKING, 1) == 1);
	while (!consumer_done)
		p9_delay_ms(5);
	assert(consumer_sum == 190);
	puts("pass msgq-procs");
}

/* ---- qlock order ---- */
static P9QLock ql;
static int order[4];
static volatile int ordern;

static void
locker(void *arg)
{
	int id = (int)(intptr_t)arg;
	p9_qlock(&ql);
	order[ordern++] = id;
	p9_delay_ms(15);
	p9_qunlock(&ql);
}

static void
test_qlock_fifo(void)
{
	int i;
	p9_qlock_init(&ql);
	for (i = 0; i < 4; i++) {
		void *a = (void *)(intptr_t)i;
		assert(p9_proc_create("locker", locker, a) != NULL);
		p9_delay_ms(20); /* stagger arrivals deterministically */
	}
	while (ordern < 4)
		p9_delay_ms(5);
	for (i = 0; i < 4; i++)
		assert(order[i] == i);
	assert(p9_canqlock(&ql));
	p9_qunlock(&ql);
	puts("pass qlock-fifo");
}

/* ---- OSI queues ---- */
static void
test_osi_queues(void)
{
	void *q = __wifi_queue_create(2, sizeof(int));
	int v, out;
	assert(q != NULL);
	v = 1; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK) == 1);
	v = 2; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK) == 1);
	v = 9; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK) == 0);
	v = 9; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_OVERWRITE) == 1);
	assert(__wifi_queue_recv(q, &out, 0) == 1 && out == 1);
	assert(__wifi_queue_recv(q, &out, 0) == 1 && out == 9);
	v = 3; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK) == 1);
	v = 0; assert(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_FRONT) == 1);
	assert(__wifi_queue_recv(q, &out, 0) == 1 && out == 0);
	assert(__wifi_queue_recv(q, &out, 0) == 1 && out == 3);
	assert(__wifi_queue_msg_num(q) == 0);
	__wifi_queue_delete(q);
	puts("pass osi-queues");
}

/* ---- OSI semaphore + timer ---- */
static volatile int timer_fired;

static void
timer_cb(void *arg)
{
	(void)arg;
	timer_fired = 1;
}

static void
test_osi_sem_timer(void)
{
	void *s = __wifi_semphr_create(2, 1);
	void *tm;
	int waited = 0;
	assert(s != NULL);
	assert(__wifi_semphr_take(s, 0) == 1);
	assert(__wifi_semphr_take(s, 0) == 0);
	assert(__wifi_semphr_give(s) == 1);
	assert(__wifi_semphr_take(s, 5) == 1);
	__wifi_semphr_delete(s);

	tm = __wifi_timer_create("t1", 5 /* ticks = 50 ms */, false, NULL, timer_cb);
	assert(tm != NULL);
	while (!timer_fired && waited < 500) {
		p9_delay_ms(10);
		waited += 10;
	}
	assert(timer_fired);
	puts("pass osi-sem-timer");
}

/* ---- OSI task + queue echo ---- */
static void *echo_q;
static volatile int echo_got;

static void
echo_proc(void *arg)
{
	int v;
	(void)arg;
	if (__wifi_queue_recv(echo_q, &v, OSI_FUNCS_TIME_BLOCKING) == 1)
		echo_got = v;
}

static void
test_osi_task_echo(void)
{
	int v = 42, waited = 0;
	echo_q = __wifi_queue_create(4, sizeof(int));
	assert(echo_q != NULL);
	assert(__wifi_task_create((void *)echo_proc, "echo", 1024, NULL, 5) != NULL);
	assert(__wifi_queue_send(echo_q, &v, OSI_FUNCS_TIME_BLOCKING,
				 OSI_QUEUE_SEND_BACK) == 1);
	while (echo_got == 0 && waited < 1000) {
		p9_delay_ms(10);
		waited += 10;
	}
	assert(echo_got == 42);
	puts("pass osi-task-echo");
}

/* ---- OSI blob contract (from RTOS blob call-site RE) ---- */
static void
test_osi_blob_contract(void)
{
	/* configMAX_PRIORITIES is 15 in the RTOS SDK config. */
	assert(__wifi_task_get_max_priority() == 15);

	/* pp_attach creates its queue as (75 entries, 8 bytes). */
	{
		void *q = __wifi_queue_create(75, 8);
		uint64_t v, out = 0;
		int i, hptw = -1;

		assert(q != NULL);
		for (i = 0; i < 75; i++) {
			v = (uint64_t)i;
			assert(__wifi_queue_send(q, &v, 0,
						 OSI_QUEUE_SEND_BACK) == 1);
		}
		assert(__wifi_queue_msg_num(q) == 75);
		assert(__wifi_queue_recv(q, &out, 0) == 1 && out == 0);
		v = 99;
		assert(__wifi_queue_send_from_isr(q, &v, &hptw) == 1);
		/* ISR sends never ask the caller to yield out of an
		 * interrupt frame on this substrate. */
		assert(hptw == 0);
		assert(__wifi_queue_msg_num(q) == 75);
		__wifi_queue_delete(q);
	}
	puts("pass osi-blob-contract");
}

int
main(void)
{
	test_msgq_procs();
	test_qlock_fifo();
	test_osi_queues();
	test_osi_sem_timer();
	test_osi_task_echo();
	test_osi_blob_contract();
	puts("test_kernel: all tests passed");
	return 0;
}
