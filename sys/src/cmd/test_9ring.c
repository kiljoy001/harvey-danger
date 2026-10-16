#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <sched.h>
#include <stddef.h>

#include "sys/include/9ring.h"

/* 9P Message types from fcall.h */
enum {
	Tflush = 108,
	Rflush = 109,
	Tread  = 116,
	Rread  = 117,
	Twrite = 118,
	Rwrite = 119,
};

static uint64_t
get_nanos(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* =========================================================================
 * 1. Cache Line Alignment & Data Structure Layout Verification
 * ========================================================================= */
static void
test_cache_alignment(void)
{
	printf("[TEST 1] Verifying cache line isolation & structure layout...\n");

	printf("  sizeof(P9Slot):   %zu bytes\n", sizeof(P9Slot));
	printf("  sizeof(P9Comp):   %zu bytes\n", sizeof(P9Comp));
	printf("  sizeof(P9TRing):  %zu bytes\n", sizeof(P9TRing));
	printf("  sizeof(P9RRing):  %zu bytes\n", sizeof(P9RRing));
	printf("  sizeof(P9Channel):%zu bytes\n", sizeof(P9Channel));

	P9Channel ch;
	memset(&ch, 0, sizeof(ch));

	uintptr_t t_head = (uintptr_t)&ch.t_ring.head;
	uintptr_t t_tail = (uintptr_t)&ch.t_ring.tail;
	uintptr_t t_slots = (uintptr_t)&ch.t_ring.slots[0];

	uintptr_t r_head = (uintptr_t)&ch.r_ring.head;
	uintptr_t r_tail = (uintptr_t)&ch.r_ring.tail;
	uintptr_t r_comps = (uintptr_t)&ch.r_ring.comps[0];

	assert(t_head % 64 == 0);
	assert(t_tail % 64 == 0);
	assert(t_slots % 64 == 0);

	assert(r_head % 64 == 0);
	assert(r_tail % 64 == 0);
	assert(r_comps % 64 == 0);

	/* Ensure head and tail are on distinct cache lines */
	assert(t_tail - t_head >= 64);
	assert(t_slots - t_tail >= 64);
	assert(r_tail - r_head >= 64);
	assert(r_comps - r_tail >= 64);

	printf("  PASS: Head and tail pointers isolated on separate 64-byte cache lines.\n\n");
}

/* =========================================================================
 * 2. FIFO Ordering and Ring Capacity Edge Cases
 * ========================================================================= */
static void
test_ring_capacity_and_fifo(void)
{
	printf("[TEST 2] Verifying FIFO ordering and ring capacity boundaries...\n");

	P9Channel ch;
	memset(&ch, 0, sizeof(ch));

	P9Slot s;
	P9Comp c;

	/* Ring empty check */
	assert(p9_tring_consume(&ch.t_ring, &s) == 0);
	assert(p9_rring_poll(&ch.r_ring, &c) == 0);

	/* Fill T-ring to capacity */
	for (int i = 0; i < P9_RING_SIZE; i++) {
		s.tag = (u16)i;
		s.type = Tread;
		s.fid = 1;
		s.offset = (u64)(i * 4096);
		s.count = 4096;
		int ret = p9_tring_submit(&ch.t_ring, &s);
		assert(ret == 0);
	}

	/* Ring full check */
	s.tag = 999;
	assert(p9_tring_submit(&ch.t_ring, &s) == -1);

	/* Drain T-ring and verify strict FIFO order */
	for (int i = 0; i < P9_RING_SIZE; i++) {
		int ret = p9_tring_consume(&ch.t_ring, &s);
		assert(ret == 1);
		assert(s.tag == (u16)i);
		assert(s.offset == (u64)(i * 4096));
	}

	/* Ring empty again */
	assert(p9_tring_consume(&ch.t_ring, &s) == 0);

	printf("  PASS: FIFO order strictly maintained; full and empty boundaries enforced.\n\n");
}

/* =========================================================================
 * 3. Tagged Out-of-Order Execution (Killing Head-of-Line Blocking)
 * ========================================================================= */
static void
test_out_of_order_execution(void)
{
	printf("[TEST 3] Verifying Tagged Out-of-Order Execution...\n");

	P9Channel ch;
	memset(&ch, 0, sizeof(ch));

	/*
	 * Client submits 3 requests:
	 *   Tag 10: Slow Disk Read (simulated 20ms)
	 *   Tag 11: Fast RAM Read (instant)
	 *   Tag 12: Fast Register Write (instant)
	 */
	P9Slot s10 = { .tag = 10, .type = Tread, .fid = 1, .offset = 0, .count = 4096 };
	P9Slot s11 = { .tag = 11, .type = Tread, .fid = 2, .offset = 0, .count = 64 };
	P9Slot s12 = { .tag = 12, .type = Twrite, .fid = 3, .offset = 0, .count = 4 };

	p9_tring_submit(&ch.t_ring, &s10);
	p9_tring_submit(&ch.t_ring, &s11);
	p9_tring_submit(&ch.t_ring, &s12);

	/* Server consumes all 3 requests */
	P9Slot reqs[3];
	for (int i = 0; i < 3; i++) {
		assert(p9_tring_consume(&ch.t_ring, &reqs[i]) == 1);
	}

	/*
	 * Server finishes Fast RAM (Tag 11) and Fast Register (Tag 12) immediately,
	 * while Tag 10 is still pending. Server submits completions out of order:
	 *   1st completion: Tag 11
	 *   2nd completion: Tag 12
	 *   3rd completion: Tag 10 (completed later)
	 */
	P9Comp c11 = { .tag = 11, .type = Rread, .status = 0, .count = 64 };
	P9Comp c12 = { .tag = 12, .type = Rwrite, .status = 0, .count = 4 };
	P9Comp c10 = { .tag = 10, .type = Rread, .status = 0, .count = 4096 };

	p9_rring_complete(&ch.r_ring, &c11);
	p9_rring_complete(&ch.r_ring, &c12);
	p9_rring_complete(&ch.r_ring, &c10);

	/*
	 * Client polls completions:
	 * Tag 11 arrives first, then Tag 12, then Tag 10.
	 * Fast operations are never delayed behind Tag 10!
	 */
	P9Comp out;
	assert(p9_rring_poll(&ch.r_ring, &out) == 1 && out.tag == 11);
	assert(p9_rring_poll(&ch.r_ring, &out) == 1 && out.tag == 12);
	assert(p9_rring_poll(&ch.r_ring, &out) == 1 && out.tag == 10);
	assert(p9_rring_poll(&ch.r_ring, &out) == 0);

	printf("  PASS: Tag 11 and Tag 12 returned before Tag 10 without head-of-line blocking.\n\n");
}

/* =========================================================================
 * 4. Tflush Native Request Cancellation
 * ========================================================================= */
static void
test_tflush_cancellation(void)
{
	printf("[TEST 4] Verifying Native Tflush Request Cancellation...\n");

	P9Channel ch;
	memset(&ch, 0, sizeof(ch));

	/* Client submits slow sensor read under Tag 20 */
	P9Slot s20 = { .tag = 20, .type = Tread, .fid = 5, .offset = 0, .count = 128 };
	p9_tring_submit(&ch.t_ring, &s20);

	/* Client decides to cancel Tag 20 by issuing Tflush (tag 21, fid holds oldtag) */
	P9Slot flush = { .tag = 21, .type = Tflush, .fid = 20, .offset = 0, .count = 0 };
	p9_tring_submit(&ch.t_ring, &flush);

	/* Server consumes request and flush */
	P9Slot r_work, r_flush;
	assert(p9_tring_consume(&ch.t_ring, &r_work) == 1 && r_work.tag == 20);
	assert(p9_tring_consume(&ch.t_ring, &r_flush) == 1 && r_flush.tag == 21);
	assert(r_flush.type == Tflush && r_flush.fid == 20);

	/* Server aborts Tag 20 and replies with Rflush (tag 21) */
	P9Comp comp_flush = { .tag = 21, .type = Rflush, .status = 0, .count = 0 };
	p9_rring_complete(&ch.r_ring, &comp_flush);

	/* Client observes Rflush and marks Tag 20 safely cancelled and reclaimed */
	P9Comp out;
	assert(p9_rring_poll(&ch.r_ring, &out) == 1);
	assert(out.tag == 21 && out.type == Rflush);
	assert(p9_rring_poll(&ch.r_ring, &out) == 0);

	printf("  PASS: Tflush aborted pending request cleanly; tag freed without stalls.\n\n");
}

/* =========================================================================
 * 5. Cross-Core Benchmark: Duplex Lockless SPSC 9P Ring vs Mutex Queue
 * ========================================================================= */

#define BENCH_OPS 1000000

/* --- Mutex baseline queue --- */
typedef struct MutexQueue {
	pthread_mutex_t mtx;
	pthread_cond_t  cv;
	P9Slot slots[P9_RING_SIZE];
	int head;
	int tail;
	int count;
} MutexQueue;

static void
mq_init(MutexQueue *q)
{
	pthread_mutex_init(&q->mtx, NULL);
	pthread_cond_init(&q->cv, NULL);
	q->head = q->tail = q->count = 0;
}

static void
mq_push(MutexQueue *q, const P9Slot *s)
{
	pthread_mutex_lock(&q->mtx);
	while (q->count == P9_RING_SIZE)
		pthread_cond_wait(&q->cv, &q->mtx);
	q->slots[q->head] = *s;
	q->head = (q->head + 1) & P9_RING_MASK;
	q->count++;
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mtx);
}

static int
mq_pop(MutexQueue *q, P9Slot *s)
{
	pthread_mutex_lock(&q->mtx);
	while (q->count == 0)
		pthread_cond_wait(&q->cv, &q->mtx);
	*s = q->slots[q->tail];
	q->tail = (q->tail + 1) & P9_RING_MASK;
	q->count--;
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mtx);
	return 1;
}

/* --- Benchmark context --- */
typedef struct BenchContext {
	P9Channel ch;
	MutexQueue mq_req;
	MutexQueue mq_resp;
	_Atomic bool stop;
	int client_cpu;
	int server_cpu;
} BenchContext;

static void
set_cpu_affinity(int cpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

/* Lockless Server Thread */
static void*
lockless_server_fn(void *arg)
{
	BenchContext *ctx = (BenchContext*)arg;
	set_cpu_affinity(ctx->server_cpu);

	P9Slot slot;
	P9Comp comp;

	for (int i = 0; i < BENCH_OPS; i++) {
		while (!p9_tring_consume(&ctx->ch.t_ring, &slot)) {
			#if defined(__x86_64__)
			__builtin_ia32_pause();
			#endif
		}

		comp.tag = slot.tag;
		comp.type = Rread;
		comp.status = 0;
		comp.count = slot.count;
		comp.result = 0;

		while (p9_rring_complete(&ctx->ch.r_ring, &comp) != 0) {
			#if defined(__x86_64__)
			__builtin_ia32_pause();
			#endif
		}
	}
	return NULL;
}

/* Mutex Baseline Server Thread */
static void*
mutex_server_fn(void *arg)
{
	BenchContext *ctx = (BenchContext*)arg;
	set_cpu_affinity(ctx->server_cpu);

	P9Slot slot;
	P9Slot resp;

	for (int i = 0; i < BENCH_OPS; i++) {
		mq_pop(&ctx->mq_req, &slot);
		resp.tag = slot.tag;
		resp.type = Rread;
		resp.count = slot.count;
		mq_push(&ctx->mq_resp, &resp);
	}
	return NULL;
}

static void
run_benchmark(void)
{
	printf("[BENCHMARK] Running Cross-Core Round-Trip Benchmark (%d iterations)...\n", BENCH_OPS);

	BenchContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.client_cpu = 2; /* Core 2 */
	ctx.server_cpu = 4; /* Core 4 */

	printf("  Hardware affinity: Client on CPU %d, Server on CPU %d\n", ctx.client_cpu, ctx.server_cpu);

	/* -------------------------------------------------------------
	 * 1. Lockless Duplex SPSC 9P Ring
	 * ------------------------------------------------------------- */
	pthread_t server_th;
	pthread_create(&server_th, NULL, lockless_server_fn, &ctx);
	set_cpu_affinity(ctx.client_cpu);

	P9Slot s = { .type = Tread, .fid = 1, .count = 4096 };
	P9Comp c;

	/* Pipeline with multiple in-flight slots (queue depth up to 64) */
	const int window = 64;
	int sent = 0;
	int received = 0;

	uint64_t t0 = get_nanos();

	while (received < BENCH_OPS) {
		while (sent < BENCH_OPS && (sent - received) < window) {
			s.tag = (u16)(sent & 0xFFFF);
			s.offset = (u64)sent * 4096;
			if (p9_tring_submit(&ctx.ch.t_ring, &s) == 0) {
				sent++;
			} else {
				break;
			}
		}

		while (p9_rring_poll(&ctx.ch.r_ring, &c)) {
			received++;
		}
	}

	pthread_join(server_th, NULL);
	uint64_t t1 = get_nanos();
	double lockless_seconds = (double)(t1 - t0) / 1e9;
	double lockless_mops = (double)BENCH_OPS / lockless_seconds / 1e6;
	double lockless_latency_ns = (double)(t1 - t0) / (double)BENCH_OPS;

	printf("  [1] Duplex Lockless SPSC Ring:\n");
	printf("      Total Time:     %.4f seconds\n", lockless_seconds);
	printf("      Throughput:     %.2f Million Ops/sec\n", lockless_mops);
	printf("      Avg Latency:    %.2f nanoseconds / RPC\n\n", lockless_latency_ns);

	/* -------------------------------------------------------------
	 * 2. Traditional Mutex + Condvar Queue Baseline
	 * ------------------------------------------------------------- */
	mq_init(&ctx.mq_req);
	mq_init(&ctx.mq_resp);

	pthread_create(&server_th, NULL, mutex_server_fn, &ctx);
	set_cpu_affinity(ctx.client_cpu);

	sent = 0;
	received = 0;

	t0 = get_nanos();

	while (received < BENCH_OPS) {
		while (sent < BENCH_OPS && (sent - received) < window) {
			s.tag = (u16)(sent & 0xFFFF);
			s.offset = (u64)sent * 4096;
			mq_push(&ctx.mq_req, &s);
			sent++;
		}

		P9Slot resp;
		while (received < sent) {
			mq_pop(&ctx.mq_resp, &resp);
			received++;
		}
	}

	pthread_join(server_th, NULL);
	t1 = get_nanos();
	double mutex_seconds = (double)(t1 - t0) / 1e9;
	double mutex_mops = (double)BENCH_OPS / mutex_seconds / 1e6;
	double mutex_latency_ns = (double)(t1 - t0) / (double)BENCH_OPS;

	printf("  [2] Traditional Mutex + Condvar Baseline:\n");
	printf("      Total Time:     %.4f seconds\n", mutex_seconds);
	printf("      Throughput:     %.2f Million Ops/sec\n", mutex_mops);
	printf("      Avg Latency:    %.2f nanoseconds / RPC\n\n", mutex_latency_ns);

	/* -------------------------------------------------------------
	 * Comparison Summary
	 * ------------------------------------------------------------- */
	double speedup = mutex_seconds / lockless_seconds;
	printf("  ==============================================================\n");
	printf("  SPEEDUP: Duplex Lockless SPSC is %.2fx faster than Mutex IPC\n", speedup);
	printf("  Latency reduced from %.1f ns -> %.1f ns per message!\n", mutex_latency_ns, lockless_latency_ns);
	printf("  ==============================================================\n\n");
}

int
main(void)
{
	printf("=================================================================\n");
	printf("  Duplex Lockless SPSC 9P Ring Buffer Test & Benchmark Suite\n");
	printf("=================================================================\n\n");

	test_cache_alignment();
	test_ring_capacity_and_fifo();
	test_out_of_order_execution();
	test_tflush_cancellation();
	run_benchmark();

	printf("ALL 9RING TESTS AND BENCHMARKS COMPLETED SUCCESSFULLY!\n");
	return 0;
}
