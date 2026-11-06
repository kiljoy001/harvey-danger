/* rtos_mem - see rtos_mem.h. Sampler task appends only. */
#include "rtos_mem.h"

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "memwatch.h"

static pp_arena *mw_arena;
static volatile uint32_t mw_ev;
static char membuf[6144];

void
esp9_mem_init(pp_arena *arena)
{
	mw_arena = arena;
	mw_ev = 0;
	memwatch_reset();
}

void
esp9_mem_event(uint32_t ev)
{
	mw_ev = ev;
}

void
esp9_mem_sample_at(uint32_t tick_ms, uint32_t free_heap)
{
	MemwatchSample s;

	memset(&s, 0, sizeof(s));
	s.tick_ms = tick_ms;
	s.event = mw_ev;
	s.free_heap = free_heap;
	if (mw_arena != NULL) {
		s.arena_used = mw_arena->used;
		s.arena_high = mw_arena->high;
	}
	memwatch_push(&s);
}

const char *
esp9_mem_read(uint32_t *len)
{
	uint32_t i, n = memwatch_count();
	char *p = membuf;
	char *end = membuf + sizeof(membuf);

	p += sprintf(p,
		     "# memwatch samples %u total %u minfree %u "
		     "maxarena %u maxstack %u\n"
		     "# seq tick ev free arena arenahigh q stack\n",
		     n, memwatch_total(), memwatch_min_free(),
		     memwatch_max_arena_used(), memwatch_max_stack_used());
	for (i = 0; i < n; i++) {
		MemwatchSample s;
		int w;

		if (!memwatch_get(i, &s))
			break;
		w = sprintf(p, "%u %u %u %u %u %u %u %u\n", s.seq, s.tick_ms,
			    s.event, s.free_heap, s.arena_used, s.arena_high,
			    s.q_count, s.stack_used);
		if (w < 0 || p + w >= end)
			break;
		p += w;
	}
	*p = '\0';
	*len = (uint32_t)strlen(membuf);
	return membuf;
}

static void
mem_task(void *arg)
{
	(void)arg;
	for (;;) {
		esp9_mem_sample_at((uint32_t)(esp_timer_get_time() / 1000),
				   esp_get_free_heap_size());
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void
esp9_mem_start(pp_arena *arena)
{
	esp9_mem_init(arena);
	xTaskCreate(mem_task, "memwatch", 2048, NULL, 3, NULL);
}
