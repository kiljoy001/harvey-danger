#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "9ring.h"
#include "virtio9p_core.h"

static int failures;

#define CHECK(c) check((c), #c, __LINE__)

static void
check(int condition, const char *expression, int line)
{
	if(condition)
		return;
	fprintf(stderr, "kernel-transport:%d: check failed: %s\n", line,
		expression);
	failures++;
}

static void
test_layout(void)
{
	P9Channel channel;
	uintptr_t thead, ttail, tslot, rhead, rtail, rcomp;

	memset(&channel, 0, sizeof(channel));
	thead = (uintptr_t)&channel.t_ring.head;
	ttail = (uintptr_t)&channel.t_ring.tail;
	tslot = (uintptr_t)&channel.t_ring.slots[0];
	rhead = (uintptr_t)&channel.r_ring.head;
	rtail = (uintptr_t)&channel.r_ring.tail;
	rcomp = (uintptr_t)&channel.r_ring.comps[0];
	CHECK(sizeof(P9Slot) == 32);
	CHECK(sizeof(P9Comp) == 16);
	CHECK(thead % P9_CACHELINE == 0);
	CHECK(ttail % P9_CACHELINE == 0);
	CHECK(tslot % P9_CACHELINE == 0);
	CHECK(rhead % P9_CACHELINE == 0);
	CHECK(rtail % P9_CACHELINE == 0);
	CHECK(rcomp % P9_CACHELINE == 0);
	CHECK(ttail - thead >= P9_CACHELINE);
	CHECK(tslot - ttail >= P9_CACHELINE);
	CHECK(rtail - rhead >= P9_CACHELINE);
	CHECK(rcomp - rtail >= P9_CACHELINE);
}

static void
test_ring_boundaries(void)
{
	P9Channel channel;
	P9Slot slot, out;
	P9Comp comp, done;
	int i;

	memset(&channel, 0, sizeof(channel));
	memset(&slot, 0, sizeof(slot));
	CHECK(p9_tring_consume(&channel.t_ring, &out) == 0);
	for(i = 0; i < P9_RING_SIZE; i++){
		slot.tag = (u16)i;
		slot.offset = (u64)i * 4096;
		CHECK(p9_tring_submit(&channel.t_ring, &slot) == 0);
	}
	CHECK(p9_tring_submit(&channel.t_ring, &slot) < 0);
	for(i = 0; i < P9_RING_SIZE; i++){
		CHECK(p9_tring_consume(&channel.t_ring, &out) == 1);
		CHECK(out.tag == (u16)i);
		CHECK(out.offset == (u64)i * 4096);
	}
	CHECK(p9_tring_consume(&channel.t_ring, &out) == 0);

	memset(&comp, 0, sizeof(comp));
	for(i = 0; i < P9_RING_SIZE; i++){
		comp.tag = (u16)(P9_RING_SIZE - i);
		comp.result = i;
		CHECK(p9_rring_complete(&channel.r_ring, &comp) == 0);
	}
	CHECK(p9_rring_complete(&channel.r_ring, &comp) < 0);
	for(i = 0; i < P9_RING_SIZE; i++){
		CHECK(p9_rring_poll(&channel.r_ring, &done) == 1);
		CHECK(done.tag == (u16)(P9_RING_SIZE - i));
		CHECK(done.result == (u64)i);
	}
	CHECK(p9_rring_poll(&channel.r_ring, &done) == 0);
}

static void
test_ring_wrap(void)
{
	P9TRing ring;
	P9Slot slot, out;
	uint32_t start;
	int i;

	memset(&ring, 0, sizeof(ring));
	memset(&slot, 0, sizeof(slot));
	start = UINT32_MAX - 127;
	ring.head = start;
	ring.tail = start;
	for(i = 0; i < P9_RING_SIZE; i++){
		slot.offset = (u64)i;
		CHECK(p9_tring_submit(&ring, &slot) == 0);
	}
	CHECK(p9_tring_submit(&ring, &slot) < 0);
	for(i = 0; i < P9_RING_SIZE; i++){
		CHECK(p9_tring_consume(&ring, &out) == 1);
		CHECK(out.offset == (u64)i);
	}
	CHECK(ring.head == ring.tail);
}

static void
putlength(u8 *message, u32 length)
{
	message[0] = length;
	message[1] = length >> 8;
	message[2] = length >> 16;
	message[3] = length >> 24;
}

static void
test_virtio_validation(void)
{
	u8 message[128];

	memset(message, 0, sizeof(message));
	putlength(message, sizeof(message));
	CHECK(v9pmessagevalid(message, sizeof(message), sizeof(message)));
	CHECK(!v9pmessagevalid(message, sizeof(message) - 1, sizeof(message)));
	CHECK(!v9pmessagevalid(message, 6, sizeof(message)));
	CHECK(!v9pmessagevalid(message, sizeof(message), sizeof(message) - 1));
	CHECK(!v9pmessagevalid(NULL, sizeof(message), sizeof(message)));

	CHECK(v9pqueuesize(0, 64) == 0);
	CHECK(v9pqueuesize(1, 64) == 0);
	CHECK(v9pqueuesize(2, 64) == 2);
	CHECK(v9pqueuesize(3, 64) == 2);
	CHECK(v9pqueuesize(63, 64) == 32);
	CHECK(v9pqueuesize(64, 64) == 64);
	CHECK(v9pqueuesize(1024, 64) == 64);

	CHECK(v9pindexvalid(3, 4));
	CHECK(!v9pindexvalid(4, 4));
	CHECK(!v9pindexvalid(1, 3));
	CHECK(v9pchain2valid(1, V9pdescnext, 2, 0, 4));
	CHECK(!v9pchain2valid(4, V9pdescnext, 2, 0, 4));
	CHECK(!v9pchain2valid(1, 0, 2, 0, 4));
	CHECK(!v9pchain2valid(1, V9pdescnext, 4, 0, 4));
	CHECK(!v9pchain2valid(1, V9pdescnext, 1, 0, 4));
	CHECK(!v9pchain2valid(1, V9pdescnext, 2, V9pdescnext, 4));
}

static uint64_t
random64(uint64_t *state)
{
	uint64_t x;

	x = *state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

static u32
expectedqueuesize(u32 offered, u32 limit)
{
	u32 size, capped;

	capped = offered < limit ? offered : limit;
	size = 1;
	while(size <= capped / 2)
		size *= 2;
	return size >= 2 ? size : 0;
}

static void
property_ring_model(void)
{
	P9TRing ring;
	P9Slot slot, out;
	u64 model[P9_RING_SIZE];
	uint64_t random, serial;
	int seed, step, front, count, result;

	for(seed = 1; seed <= 128; seed++){
		memset(&ring, 0, sizeof(ring));
		memset(&slot, 0, sizeof(slot));
		random = (uint64_t)seed * 0x9e3779b97f4a7c15ULL;
		serial = 0;
		front = 0;
		count = 0;
		for(step = 0; step < 4096; step++){
			if(random64(&random) & 1){
				slot.offset = serial++;
				result = p9_tring_submit(&ring, &slot);
				CHECK((result == 0) == (count < P9_RING_SIZE));
				if(result == 0){
					model[(front + count) & P9_RING_MASK] = slot.offset;
					count++;
				}
			}else{
				result = p9_tring_consume(&ring, &out);
				CHECK((result == 1) == (count > 0));
				if(result == 1){
					CHECK(out.offset == model[front]);
					front = (front + 1) & P9_RING_MASK;
					count--;
				}
			}
			CHECK((u32)(ring.head - ring.tail) == (u32)count);
		}
	}
}

static void
property_virtio_validation(void)
{
	u8 message[256];
	uint64_t random;
	u32 offered, limit, length;
	int seed, step;

	for(seed = 1; seed <= 128; seed++){
		random = (uint64_t)seed * 0xd1b54a32d192ed03ULL;
		for(step = 0; step < 4096; step++){
			offered = random64(&random);
			limit = random64(&random) % 1024;
			CHECK(v9pqueuesize(offered, limit) ==
				expectedqueuesize(offered, limit));
			length = 7 + random64(&random) % (sizeof(message) - 7);
			memset(message, 0, sizeof(message));
			putlength(message, length);
			CHECK(v9pmessagevalid(message, length, sizeof(message)));
			message[random64(&random) % 4] ^= 1;
			CHECK(!v9pmessagevalid(message, length, sizeof(message)));
		}
	}
}

static void
rununit(void)
{
	test_layout();
	test_ring_boundaries();
	test_ring_wrap();
	test_virtio_validation();
}

int
main(int argc, char **argv)
{
	int unit, property;

	unit = argc == 1 || strcmp(argv[1], "--unit") == 0;
	property = argc == 1 || strcmp(argv[1], "--property") == 0;
	if(!unit && !property){
		fprintf(stderr, "usage: %s [--unit|--property]\n", argv[0]);
		return 2;
	}
	if(unit)
		rununit();
	if(property){
		property_ring_model();
		property_virtio_validation();
	}
	if(failures != 0)
		return 1;
	printf("KERNEL-TRANSPORT-%s PASS\n", unit && property ? "ALL" :
		unit ? "UNIT" : "PROPERTY");
	return 0;
}
