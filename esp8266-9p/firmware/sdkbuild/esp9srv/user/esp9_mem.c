/* esp9_mem - see esp9_mem.h. Sampler appends only; no printing
 * from timer context. Rendering is on 9P read. */
#include "esp9_mem.h"

#if defined(ICACHE_FLASH)
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
/* ets_* ROM functions have no prototypes in this SDK's headers. */
extern int ets_sprintf(char *buf, const char *fmt, ...);
extern unsigned int ets_strlen(const char *s);
extern void ets_timer_disarm(ETSTimer *ptimer);
extern void ets_timer_setfn(ETSTimer *ptimer, ETSTimerFunc *fn, void *arg);
extern void ets_timer_arm_new(ETSTimer *ptimer, uint32_t ms, bool repeat,
			      int ms_flag);
#define ESP9_SPRINTF os_sprintf
#define ESP9_STRLEN os_strlen
#else
#include <stdio.h>
#define ESP9_SPRINTF sprintf
#define ESP9_STRLEN strlen
#endif

#include <string.h>

#include "memwatch.h"

static pp_arena *mw_arena;
static volatile uint32_t mw_ev;
static char membuf[8192];

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

	p += ESP9_SPRINTF(p,
			  "# memwatch samples %u total %u minfree %u "
			  "maxarena %u maxstack %u\n"
			  "# seq tick ev free arena arenahigh q stack\n",
			  n, memwatch_total(), memwatch_min_free(),
			  memwatch_max_arena_used(),
			  memwatch_max_stack_used());
	for (i = 0; i < n; i++) {
		MemwatchSample s;
		int w;

		if (!memwatch_get(i, &s))
			break;
		w = ESP9_SPRINTF(p, "%u %u %u %u %u %u %u %u\n", s.seq,
				 s.tick_ms, s.event, s.free_heap,
				 s.arena_used, s.arena_high, s.q_count,
				 s.stack_used);
		if (w < 0 || p + w >= end)
			break;
		p += w;
	}
	*p = '\0';
	*len = (uint32_t)ESP9_STRLEN(membuf);
	return membuf;
}

#if defined(ICACHE_FLASH)
static ETSTimer mw_timer;

static void ICACHE_FLASH_ATTR
mw_tick(void *arg)
{
	(void)arg;
	esp9_mem_sample_at(system_get_time() / 1000,
			   system_get_free_heap_size());
}

void ICACHE_FLASH_ATTR
esp9_mem_start(pp_arena *arena)
{
	esp9_mem_init(arena);
	os_timer_disarm(&mw_timer);
	os_timer_setfn(&mw_timer, mw_tick, NULL);
	os_timer_arm(&mw_timer, 100, 1);
}
#else
void
esp9_mem_start(pp_arena *arena)
{
	esp9_mem_init(arena);
}
#endif
