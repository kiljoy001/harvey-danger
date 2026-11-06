/* memwatch - in-action memory capture for the plan9ized ESP8266.
 *
 * A fixed ring of samples. The sampler never allocates and never
 * prints: it only appends one record. Dumping happens later, after
 * the action under test has finished. Portable on purpose; the
 * caller fills each field from whatever substrate it runs on
 * (system heap counters, a pp_arena, a P9MsgQ, proc stacks).
 */
#ifndef MEMWATCH_H
#define MEMWATCH_H

#include <stdint.h>

#define MEMWATCH_CAP 64

typedef struct {
	uint32_t seq;        /* monotonically increasing */
	uint32_t tick_ms;
	uint32_t event;      /* caller-defined phase marker */
	uint32_t free_heap;  /* substrate heap free right now */
	uint32_t arena_used;
	uint32_t arena_high;
	uint32_t q_count;
	uint32_t stack_used; /* worst proc stack high-water seen */
} MemwatchSample;

void memwatch_reset(void);
void memwatch_push(const MemwatchSample *s);
/* Number of samples currently readable (<= MEMWATCH_CAP). */
uint32_t memwatch_count(void);
/* Total pushes since reset, including overwritten ones. */
uint32_t memwatch_total(void);
/* Read the i-th oldest readable sample. Returns 1 on success. */
int memwatch_get(uint32_t i, MemwatchSample *out);
/* Smallest free_heap in the ring, or 0 if empty. */
uint32_t memwatch_min_free(void);
uint32_t memwatch_max_arena_used(void);
uint32_t memwatch_max_stack_used(void);

/* Stack high-water from low-end pattern painting.
 * Stacks grow down, so the untouched pattern run at the low end
 * is the unused part. Returns bytes used. */
uint32_t memwatch_stack_used(const uint8_t *base, uint32_t cap,
			     uint8_t pattern);

#endif
