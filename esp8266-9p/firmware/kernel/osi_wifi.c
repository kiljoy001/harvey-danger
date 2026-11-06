/* osi_wifi - see osi_wifi.h. Static pools only; no heap. */
#include "osi_wifi.h"

#include <string.h>

#include "p9.h"

#define MAX_QUEUES 8
#define MAX_SEMPHRS 8
#define MAX_TIMERS 8

/* ---- tasks ---- */
void *
__wifi_task_create(void *task_func, const char *name, uint32_t stack_depth,
		   void *param, uint32_t prio)
{
	(void)stack_depth;
	(void)prio;
	return p9_proc_create(name != NULL ? name : "wifi",
			      (void (*)(void *))task_func, param);
}

void
__wifi_task_delete(void *task)
{
	/* Cooperative substrate: a task deletes itself. Deleting
	 * another proc needs the Phase C scheduler; mark exited
	 * so hosts can observe the request. */
	if (task == p9_current() || task == NULL)
		p9_proc_exit();
	if (task != NULL)
		((P9Proc *)task)->exited = 1;
}

void __wifi_task_yield_from_isr(void)
{
	/* No yield from ISR context on this substrate: a longjmp
	 * out of an interrupt frame would corrupt both the frame
	 * and the interrupted proc's resume anchor. The posted
	 * work is picked up by the cooperative scheduler on the
	 * next yield from any proc (procs poll at 1 ms cadence). */
}
void __wifi_task_delay(uint32_t tick) { p9_delay_ticks(tick); }
/* Matches configMAX_PRIORITIES (15) in the RTOS SDK's
 * FreeRTOSConfig.h; pp_attach creates its task at max-2 = 13. */
uint32_t __wifi_task_get_max_priority(void) { return 15; }
uint32_t __wifi_task_ms_to_ticks(uint32_t ms) { return p9_ms_to_ticks(ms); }
void __wifi_task_suspend_all(void) { p9_sched_suspend_all(); }
void __wifi_task_resume_all(void) { p9_sched_resume_all(); }

void *
__wifi_task_top_sp(void)
{
	/* The RTOS version returns pxCurrentTCB[0] (saved SP).
	 * Callers use it for stack diagnostics only; the current
	 * proc struct is the closest honest equivalent. */
	return p9_current();
}

/* ---- queues ---- */
typedef struct {
	int used;
	P9MsgQ q;
	uint8_t storage[32 * 32]; /* up to 32 items of 32 bytes */
} OsiQueue;

static OsiQueue queues[MAX_QUEUES];
static P9OsLock pools_lock;
static int pools_ready;

static void
pools_init_once(void)
{
	if (!pools_ready) {
		p9_os_lock_init(&pools_lock);
		pools_ready = 1;
	}
}

void *
__wifi_queue_create(uint32_t queue_len, uint32_t item_size)
{
	int i;
	pools_init_once();
	/* Pool storage is 1024 B per queue. The blob contract
	 * needs pp_attach's (75, 8) = 600 B queue, which the old
	 * per-dimension 32 cap wrongly rejected. */
	if (queue_len == 0 || item_size == 0 || queue_len > 255 ||
	    queue_len * item_size > 32 * 32)
		return NULL;
	p9_os_lock(&pools_lock);
	for (i = 0; i < MAX_QUEUES; i++) {
		if (!queues[i].used) {
			queues[i].used = 1;
			p9_os_unlock(&pools_lock);
			p9_msgq_init(&queues[i].q, queues[i].storage,
				     queue_len, item_size);
			return &queues[i].q;
		}
	}
	p9_os_unlock(&pools_lock);
	return NULL;
}

void
__wifi_queue_delete(void *queue)
{
	int i;
	pools_init_once();
	p9_os_lock(&pools_lock);
	for (i = 0; i < MAX_QUEUES; i++)
		if (&queues[i].q == queue)
			queues[i].used = 0;
	p9_os_unlock(&pools_lock);
}

int
__wifi_queue_send(void *queue, void *item, uint32_t block_time_tick, uint32_t pos)
{
	if (queue == NULL)
		return 0;
	return p9_msgq_send((P9MsgQ *)queue, item, block_time_tick, (int)pos);
}

int
__wifi_queue_send_from_isr(void *queue, void *item, int *hptw)
{
	/* ISR context never blocks or yields on this substrate:
	 * try-send only, and report no wakeup (hptw=0) so the
	 * caller does not try to yield from the interrupt frame.
	 * The receiving proc polls at 1 ms cadence. */
	{
		int ok = queue != NULL
				 ? p9_msgq_send_isr((P9MsgQ *)queue, item,
						    OSI_QUEUE_SEND_BACK)
				 : 0;
		if (hptw != NULL)
			*hptw = 0;
		return ok;
	}
}

int
__wifi_queue_recv(void *queue, void *item, uint32_t block_time_tick)
{
	if (queue == NULL)
		return 0;
	return p9_msgq_recv((P9MsgQ *)queue, item, block_time_tick);
}

uint32_t
__wifi_queue_msg_num(void *queue)
{
	if (queue == NULL)
		return 0;
	return p9_msgq_num((P9MsgQ *)queue);
}

/* ---- counting semaphores ---- */
typedef struct {
	int used;
	P9OsLock lock;
	P9Wait wait;
	uint32_t count, max;
} OsiSemphr;

static OsiSemphr semphrs[MAX_SEMPHRS];

void *
__wifi_semphr_create(uint32_t max, uint32_t init)
{
	int i;
	pools_init_once();
	p9_os_lock(&pools_lock);
	for (i = 0; i < MAX_SEMPHRS; i++) {
		if (!semphrs[i].used) {
			OsiSemphr *s = &semphrs[i];
			s->used = 1;
			s->count = init;
			s->max = max;
			p9_os_unlock(&pools_lock);
			p9_os_lock_init(&s->lock);
			p9_os_wait_init(&s->wait);
			return s;
		}
	}
	p9_os_unlock(&pools_lock);
	return NULL;
}

void
__wifi_semphr_delete(void *semphr)
{
	OsiSemphr *s = (OsiSemphr *)semphr;
	if (s != NULL)
		s->used = 0;
}

int32_t
__wifi_semphr_take(void *semphr, uint32_t block_time_tick)
{
	OsiSemphr *s = (OsiSemphr *)semphr;
	uint64_t deadline = 0;
	if (s == NULL)
		return 0;
	if (block_time_tick != OSI_FUNCS_TIME_BLOCKING)
		deadline = p9_os_now_ms() + (uint64_t)block_time_tick * P9_TICK_MS;
	p9_os_lock(&s->lock);
	for (;;) {
		if (s->count > 0) {
			s->count--;
			p9_os_unlock(&s->lock);
			return 1;
		}
		if (block_time_tick == 0 ||
		    (block_time_tick != OSI_FUNCS_TIME_BLOCKING &&
		     p9_os_now_ms() >= deadline)) {
			p9_os_unlock(&s->lock);
			return 0;
		}
		/* Poll at 1 ms: take deadlines are OSI compat, the
		 * rendez-based sleep is p9_sleep for procs that
		 * structure their own waits. */
		p9_os_unlock(&s->lock);
		p9_os_delay_ms(1);
		p9_os_lock(&s->lock);
	}
}

int32_t
__wifi_semphr_give(void *semphr)
{
	OsiSemphr *s = (OsiSemphr *)semphr;
	int ok = 0;
	if (s == NULL)
		return 0;
	p9_os_lock(&s->lock);
	if (s->count < s->max) {
		s->count++;
		ok = 1;
	}
	p9_os_unlock(&s->lock);
	if (ok)
		p9_os_wake(&s->wait, 1);
	return ok;
}

/* ---- timers (one service proc, lazy) ---- */
typedef struct {
	int used, armed;
	uint64_t due_ms;
	uint32_t period_ticks;
	int auto_load;
	void *arg;
	void (*cb)(void *timer);
} OsiTimer;

static OsiTimer timers[MAX_TIMERS];
static int timer_service_started;

static void
timer_service(void *arg)
{
	(void)arg;
	for (;;) {
		int i;
		uint64_t now = p9_os_now_ms();
		for (i = 0; i < MAX_TIMERS; i++) {
			OsiTimer *tm = &timers[i];
			if (tm->used && tm->armed && now >= tm->due_ms) {
				if (tm->auto_load)
					tm->due_ms = now + (uint64_t)tm->period_ticks * P9_TICK_MS;
				else
					tm->armed = 0;
				if (tm->cb != NULL)
					tm->cb(tm->arg);
			}
		}
		p9_os_delay_ms(5);
	}
}

static void
timer_service_ensure(void)
{
	if (!timer_service_started) {
		timer_service_started = 1;
		p9_proc_create("ositimers", timer_service, NULL);
	}
}

void *
__wifi_timer_create(const char *name, uint32_t period_ticks, bool auto_load,
		    void *arg, void (*cb)(void *timer))
{
	int i;
	(void)name;
	pools_init_once();
	timer_service_ensure();
	p9_os_lock(&pools_lock);
	for (i = 0; i < MAX_TIMERS; i++) {
		if (!timers[i].used) {
			OsiTimer *tm = &timers[i];
			memset(tm, 0, sizeof(*tm));
			tm->used = 1;
			tm->period_ticks = period_ticks;
			tm->auto_load = auto_load;
			tm->arg = arg;
			tm->cb = cb;
			tm->armed = 1;
			tm->due_ms = p9_os_now_ms() + (uint64_t)period_ticks * P9_TICK_MS;
			p9_os_unlock(&pools_lock);
			return tm;
		}
	}
	p9_os_unlock(&pools_lock);
	return NULL;
}

int
__wifi_timer_reset(void *timer, uint32_t ticks)
{
	OsiTimer *tm = (OsiTimer *)timer;
	(void)ticks;
	if (tm == NULL)
		return 0;
	tm->due_ms = p9_os_now_ms() + (uint64_t)tm->period_ticks * P9_TICK_MS;
	tm->armed = 1;
	return 1;
}

int
__wifi_timer_stop(void *timer, uint32_t ticks)
{
	OsiTimer *tm = (OsiTimer *)timer;
	(void)ticks;
	if (tm == NULL)
		return 0;
	tm->armed = 0;
	return 1;
}
