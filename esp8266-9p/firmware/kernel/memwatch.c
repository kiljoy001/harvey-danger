/* memwatch - see memwatch.h. No heap, no printing. */
#include "memwatch.h"

#include <string.h>

static MemwatchSample ring[MEMWATCH_CAP];
static uint32_t head;  /* index of oldest sample when full */
static uint32_t count; /* samples currently in the ring */
static uint32_t total; /* pushes since reset */

void
memwatch_reset(void)
{
	memset(ring, 0, sizeof(ring));
	head = 0;
	count = 0;
	total = 0;
}

void
memwatch_push(const MemwatchSample *s)
{
	MemwatchSample *dst;

	if (count < MEMWATCH_CAP) {
		dst = &ring[(head + count) % MEMWATCH_CAP];
		count++;
	} else {
		dst = &ring[head];
		head = (head + 1) % MEMWATCH_CAP;
	}
	*dst = *s;
	dst->seq = total;
	total++;
}

uint32_t
memwatch_count(void)
{
	return count;
}

uint32_t
memwatch_total(void)
{
	return total;
}

int
memwatch_get(uint32_t i, MemwatchSample *out)
{
	if (i >= count || out == NULL)
		return 0;
	*out = ring[(head + i) % MEMWATCH_CAP];
	return 1;
}

uint32_t
memwatch_min_free(void)
{
	uint32_t i, best = 0;
	int seen = 0;

	for (i = 0; i < count; i++) {
		uint32_t v = ring[(head + i) % MEMWATCH_CAP].free_heap;
		if (!seen || v < best)
			best = v;
		seen = 1;
	}
	return best;
}

uint32_t
memwatch_max_arena_used(void)
{
	uint32_t i, best = 0;

	for (i = 0; i < count; i++) {
		uint32_t v = ring[(head + i) % MEMWATCH_CAP].arena_used;
		if (v > best)
			best = v;
	}
	return best;
}

uint32_t
memwatch_max_stack_used(void)
{
	uint32_t i, best = 0;

	for (i = 0; i < count; i++) {
		uint32_t v = ring[(head + i) % MEMWATCH_CAP].stack_used;
		if (v > best)
			best = v;
	}
	return best;
}

uint32_t
memwatch_stack_used(const uint8_t *base, uint32_t cap, uint8_t pattern)
{
	uint32_t i = 0;

	if (base == NULL)
		return 0;
	while (i < cap && base[i] == pattern)
		i++;
	return cap - i;
}
