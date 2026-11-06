/* Host tests for memwatch ring and stack watermark. */
#include <stdio.h>
#include <string.h>

#include "memwatch.h"

static int failures;

static void
check(int cond, const char *name)
{
	if (cond)
		printf("pass %s\n", name);
	else {
		printf("FAIL %s\n", name);
		failures++;
	}
}

static MemwatchSample
mk(uint32_t free_heap, uint32_t arena_used, uint32_t stack_used)
{
	MemwatchSample s;
	memset(&s, 0, sizeof(s));
	s.free_heap = free_heap;
	s.arena_used = arena_used;
	s.stack_used = stack_used;
	return s;
}

int
main(void)
{
	MemwatchSample out;
	uint32_t i;
	uint8_t stack[16];

	memwatch_reset();
	check(memwatch_count() == 0, "empty-count");
	check(memwatch_get(0, &out) == 0, "empty-get");

	{
		MemwatchSample s = mk(5000, 10, 100);
		memwatch_push(&s);
		s = mk(3000, 900, 250);
		memwatch_push(&s);
		s = mk(4500, 40, 120);
		memwatch_push(&s);
	}
	check(memwatch_count() == 3, "count-3");
	check(memwatch_min_free() == 3000, "min-free");
	check(memwatch_max_arena_used() == 900, "max-arena");
	check(memwatch_max_stack_used() == 250, "max-stack");
	check(memwatch_get(1, &out) == 1 && out.free_heap == 3000 &&
		      out.seq == 1,
	      "order");

	/* Wraparound keeps the newest MEMWATCH_CAP, in order. */
	memwatch_reset();
	for (i = 0; i < MEMWATCH_CAP + 5; i++) {
		MemwatchSample s = mk(1000 + i, i, i);
		memwatch_push(&s);
	}
	check(memwatch_count() == MEMWATCH_CAP, "wrap-count");
	check(memwatch_total() == MEMWATCH_CAP + 5, "wrap-total");
	check(memwatch_get(0, &out) == 1 && out.seq == 5, "wrap-oldest");
	check(memwatch_get(MEMWATCH_CAP - 1, &out) == 1 &&
		      out.seq == MEMWATCH_CAP + 4,
	      "wrap-newest");
	check(memwatch_get(MEMWATCH_CAP, &out) == 0, "wrap-oob");

	/* Stack watermark: stacks grow down from the top. */
	memset(stack, 0xA5, sizeof(stack));
	check(memwatch_stack_used(stack, sizeof(stack), 0xA5) == 0,
	      "stack-unused");
	memset(stack + sizeof(stack) - 5, 0x11, 5);
	check(memwatch_stack_used(stack, sizeof(stack), 0xA5) == 5,
	      "stack-top5");
	memset(stack, 0x11, sizeof(stack));
	check(memwatch_stack_used(stack, sizeof(stack), 0xA5) ==
		      sizeof(stack),
	      "stack-full");
	check(memwatch_stack_used(NULL, 16, 0xA5) == 0, "stack-null");

	if (failures == 0)
		printf("test_memwatch: all tests passed\n");
	return failures != 0;
}
