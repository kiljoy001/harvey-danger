/* freertos_shim - see freertos_shim.h.
 *
 * Known cooperative approximations (documented, not hidden):
 * - Suspending another task only takes effect at its next shim
 *   call; suspending self spins in yield until resumed.
 * - Deleting another task marks it suspended forever; only
 *   self-delete truly exits (the substrate has no cross-proc
 *   kill).
 * - Timers fire from one daemon proc, as in FreeRTOS.
 * - ISR variants behave like task variants plus hptw=1 on
 *   success; on this substrate a yield is always safe.
 */
#include "freertos_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p9.h"

#define MAX_TASKS 32

/* Raw UART0 FIFO marker for early bring-up (no stdio/locks). */
static void shim_marker(char c)
{
#ifdef __xtensa__
	volatile uint32_t *fifo = (volatile uint32_t *)0x60000000u;
	volatile uint32_t *stat = (volatile uint32_t *)0x6000001Cu;
	while (((*stat >> 16) & 0xFFu) >= 126u) {
	}
	*fifo = (uint32_t)(uint8_t)c;
#else
	(void)c; /* host tests: no UART to mark on */
#endif
}
#define TICK_MS 10u

typedef struct ShimTask {
	/* Offset 0 must be the saved-SP slot: the SDK vector
	 * layer (_xt_int_enter/_xt_int_exit in os_cpu_a.S) reads
	 * and writes pxCurrentTCB[0] as exactly that, as it does
	 * for a real FreeRTOS TCB. Keeping anything else here
	 * corrupts it on the first interrupt. */
	void *top_sp;
	P9Proc *proc;
	char name[16];
	void *tls[8];
	void *tls_cb[8];
	uint32_t notify_val;
	int notify_pending;
	int suspended;
	int used;
	struct ShimTask *tcb_dummy;
} ShimTask;

static ShimTask tasks[MAX_TASKS];
void *pxCurrentTCB;

static ShimTask *
task_of(P9Proc *p)
{
	int i;
	for (i = 0; i < MAX_TASKS; i++)
		if (tasks[i].used && tasks[i].proc == p)
			return &tasks[i];
	return NULL;
}

static ShimTask *
cur_task(void)
{
	P9Proc *p = p9_current();
	return p != NULL ? task_of(p) : NULL;
}

static void
suspend_check(void)
{
	ShimTask *t = cur_task();
	while (t != NULL && t->suspended)
		p9_yield();
}

typedef struct {
	void (*fn)(void *);
	void *arg;
	ShimTask *task;
} TaskBoot;

/* Interrupt-context detection for ISR-safe degradation of
 * task-version queue calls (blob code, e.g. pp_post, calls
 * the task send from the WDEV FIQ). NMIIrqIsOn is set by
 * wDev_ProcessFiq itself; xPortInIsrContext covers level-1
 * handlers dispatched by the shim vector layer. */
extern uint8_t NMIIrqIsOn __attribute__((weak));
extern int xPortInIsrContext(void) __attribute__((weak));
static int
shim_in_isr(void)
{
	if (&NMIIrqIsOn != NULL && NMIIrqIsOn != 0)
		return 1;
	if (xPortInIsrContext != NULL && xPortInIsrContext())
		return 1;
	return 0;
}

static void
task_trampoline(void *arg)
{
	TaskBoot *b = (TaskBoot *)arg;
	void (*fn)(void *) = b->fn;
	void *farg = b->arg;

	shim_marker('T');
	b->task->proc = p9_current();
	pxCurrentTCB = (void *)b->task;
	free(b);
	fn(farg);
	/* A FreeRTOS task must delete itself; do it for it. */
	p9_os_thread_exit();
}

BaseType_t
xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack_depth,
	    void *arg, UBaseType_t prio, TaskHandle_t *out)
{
	int i;
	TaskBoot *b;
	ShimTask *t = NULL;

	(void)stack_depth;
	(void)prio;
	for (i = 0; i < MAX_TASKS; i++)
		if (!tasks[i].used) {
			t = &tasks[i];
			break;
		}
	if (t == NULL)
		return shimFAIL;
	memset(t, 0, sizeof(*t));
	t->used = 1;
	t->tcb_dummy = t;
	if (name != NULL)
		strncpy(t->name, name, sizeof(t->name) - 1);
	b = (TaskBoot *)malloc(sizeof(*b));
	if (b == NULL)
		return shimFAIL;
	b->fn = fn;
	b->arg = arg;
	b->task = t;
	shim_marker('C');
	if (p9_proc_create(t->name[0] ? t->name : "shim", task_trampoline,
			   b) == NULL) {
		free(b);
		t->used = 0;
		return shimFAIL;
	}
	if (out != NULL)
		*out = (TaskHandle_t)t;
	return shimPASS;
}

void
vTaskDelete(TaskHandle_t h)
{
	ShimTask *t = (ShimTask *)h;

	if (t == NULL || t == cur_task()) {
		p9_os_thread_exit();
		return;
	}
	t->suspended = 1; /* no cross-proc kill on this substrate */
}

void
vTaskDelay(TickType_t ticks)
{
	suspend_check();
	p9_delay_ticks(ticks);
}

void
vTaskDelayUntil(TickType_t *last, TickType_t inc)
{
	TickType_t now = xTaskGetTickCount();
	TickType_t target = *last + inc;

	if (target > now)
		p9_delay_ticks(target - now);
	*last = target;
}

TaskHandle_t
xTaskGetCurrentTaskHandle(void)
{
	return (TaskHandle_t)cur_task();
}

TickType_t
xTaskGetTickCount(void)
{
	return (TickType_t)(p9_os_now_ms() / TICK_MS);
}

TickType_t
xTaskGetTickCountFromISR(void)
{
	return xTaskGetTickCount();
}

BaseType_t
xTaskGetSchedulerState(void)
{
	return 2; /* taskSCHEDULER_RUNNING */
}

void
vTaskStartScheduler(void)
{
	shim_marker('S');
	/* The substrate scheduler is pump-driven: run it here,
	 * where FreeRTOS would take over. Never returns in
	 * practice (procs keep being created and run). */
	for (;;)
		p9_delay_ticks(1000);
}

void
vTaskSuspend(TaskHandle_t h)
{
	ShimTask *t = h != NULL ? (ShimTask *)h : cur_task();
	if (t != NULL) {
		t->suspended = 1;
		if (t == cur_task())
			suspend_check();
	}
}

void
vTaskResume(TaskHandle_t h)
{
	ShimTask *t = (ShimTask *)h;
	if (t != NULL)
		t->suspended = 0;
}

uint32_t shim_debug_suspended_mask(void)
{
	uint32_t m = 0;
	int i;
	for (i = 0; i < MAX_TASKS; i++)
		if (tasks[i].used && tasks[i].suspended)
			m |= 1u << i;
	return m;
}

void vTaskSuspendAll(void) { p9_sched_suspend_all(); }
BaseType_t xTaskResumeAll(void)
{
	p9_sched_resume_all();
	return shimPASS;
}

UBaseType_t
uxTaskPriorityGet(TaskHandle_t h)
{
	(void)h;
	return 1;
}

void
vTaskPrioritySet(TaskHandle_t h, UBaseType_t prio)
{
	(void)h;
	(void)prio;
}

const char *
pcTaskGetName(TaskHandle_t h)
{
	ShimTask *t = h != NULL ? (ShimTask *)h : cur_task();
	return t != NULL ? t->name : "?";
}

void *
pvTaskGetThreadLocalStoragePointer(TaskHandle_t h, BaseType_t idx)
{
	ShimTask *t = h != NULL ? (ShimTask *)h : cur_task();
	if (t == NULL || idx < 0 || idx >= 8)
		return NULL;
	return t->tls[idx];
}

void
vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t h,
						BaseType_t idx, void *ptr,
						void *cb)
{
	ShimTask *t = h != NULL ? (ShimTask *)h : cur_task();
	if (t == NULL || idx < 0 || idx >= 8)
		return;
	t->tls[idx] = ptr;
	t->tls_cb[idx] = cb;
}

/* The SDK newlib __errno() is
 *   (int *)pvTaskGetThreadLocalStorageBufferPointer(NULL,
 *   ERRNO_TLS_INDEX)
 * i.e. it stores errno in the task's TLS pointer slot itself,
 * so this must return the ADDRESS of the slot, never NULL
 * (lwIP's sock_set_errno writes through it on any socket
 * error, e.g. every client disconnect). */
void **
pvTaskGetThreadLocalStorageBufferPointer(TaskHandle_t h, BaseType_t idx)
{
	static void *tls_notask[8];
	ShimTask *t = h != NULL ? (ShimTask *)h : cur_task();
	if (idx < 0 || idx >= 8)
		return NULL;
	if (t == NULL)
		return &tls_notask[idx];
	return &t->tls[idx];
}

BaseType_t
xTaskGenericNotify(TaskHandle_t h, uint32_t value, BaseType_t action,
		   uint32_t *prev)
{
	ShimTask *t = (ShimTask *)h;
	if (t == NULL)
		return shimFAIL;
	if (prev != NULL)
		*prev = t->notify_val;
	switch (action) {
	case 1: /* eSetBits */
		t->notify_val |= value;
		break;
	case 2: /* eIncrement */
		t->notify_val++;
		break;
	case 3: /* eSetValueWithOverwrite */
		t->notify_val = value;
		break;
	case 4: /* eSetValueWithoutOverwrite */
		if (t->notify_pending)
			return shimFAIL;
		t->notify_val = value;
		break;
	default: /* eNoAction */
		break;
	}
	t->notify_pending = 1;
	return shimPASS;
}

uint32_t
ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks)
{
	ShimTask *t = cur_task();
	uint32_t start = xTaskGetTickCount();
	uint32_t v;

	if (t == NULL)
		return 0;
	for (;;) {
		if (t->notify_pending) {
			v = t->notify_val;
			t->notify_val = clear_on_exit ? 0 : v - 1;
			if (t->notify_val == 0)
				t->notify_pending = 0;
			return v;
		}
		if (ticks != shimBLOCK_FOREVER &&
		    xTaskGetTickCount() - start >= ticks)
			return 0;
		p9_yield();
	}
}

/* ---- queues / semaphores ---- */
enum { QK_QUEUE = 0, QK_MUTEX = 1, QK_COUNTING = 2, QK_BINARY = 3,
       QK_RECURSIVE = 4 };

typedef struct {
	int kind;
	P9MsgQ q;
	void *storage;
	P9QLock lock;
	ShimTask *holder;
	uint32_t recurse;
} ShimQueue;

/* RX progress counter for the B20/B21 stall hunt: posts to
 * the blob pp queue (created 75 x 8) are how received frames
 * reach the pp task. Exported so the app can print it as
 * liveness telemetry: if it freezes while the scheduler
 * lives, the RX path died below the pp queue. */
static void *pp_queue_id;
uint32_t g_pp_posts;

QueueHandle_t
xQueueGenericCreate(UBaseType_t len, UBaseType_t item_size,
		    uint8_t queue_type)
{
	ShimQueue *q = (ShimQueue *)malloc(sizeof(*q));
	uint32_t i;

	if (q == NULL)
		return NULL;
	memset(q, 0, sizeof(*q));
	q->kind = queue_type;
	if (queue_type == QK_MUTEX || queue_type == QK_RECURSIVE) {
		p9_qlock_init(&q->lock);
		return (QueueHandle_t)q;
	}
	if (item_size == 0)
		item_size = 1; /* counting/binary semaphore tokens */
	if (len == 0)
		len = 1;
	q->storage = malloc((size_t)len * item_size);
	if (q->storage == NULL) {
		free(q);
		return NULL;
	}
	p9_msgq_init(&q->q, q->storage, len, item_size);
	if (len == 75 && item_size == 8)
		pp_queue_id = (void *)q;
	if (queue_type == QK_BINARY) {
		/* binary semaphores start empty in FreeRTOS */
	}
	(void)i;
	return (QueueHandle_t)q;
}

uint32_t g_shim_qsend_ok, g_shim_qsend_fail, g_shim_qrecv_ok,
	g_shim_qrecv_to, g_shim_qsend_isr_ok, g_shim_qsend_isr_fail;

BaseType_t
xQueueGenericSend(QueueHandle_t h, const void *item, TickType_t ticks,
		  BaseType_t position)
{
	ShimQueue *q = (ShimQueue *)h;
	BaseType_t r;
	if (q == NULL)
		return shimFAIL;
	if (q->kind == QK_MUTEX || q->kind == QK_RECURSIVE)
		return xQueueGiveMutexRecursive(h);
	if (shim_in_isr()) {
		/* Task-version send called from an interrupt (pp_post
		 * does this from the WDEV FIQ): never checkpoint,
		 * lock-spin or yield there. */
		r = p9_msgq_send_isr(&q->q, item, (int)position) == 1
			       ? shimPASS
			       : shimFAIL;
		if (r == shimPASS)
			g_shim_qsend_ok++;
		else
			g_shim_qsend_fail++;
		return r;
	}
	suspend_check();
	r = p9_msgq_send(&q->q, item, ticks, (int)position) == 1
		       ? shimPASS
		       : shimFAIL;
	if (r == shimPASS)
		g_shim_qsend_ok++;
	else
		g_shim_qsend_fail++;
	return r;
}

BaseType_t
xQueueGenericSendFromISR(QueueHandle_t h, const void *item,
			 BaseType_t *hptw)
{
	ShimQueue *q = (ShimQueue *)h;
	BaseType_t r;

	/* ISR context: never checkpoint, lock-spin or yield (see
	 * osi_wifi.c). Report no wakeup; the cooperative
	 * scheduler pumps the receiver at 1 ms cadence. */
	if (hptw != NULL)
		*hptw = 0;
	if (q == NULL)
		return shimFAIL;
	if (q->kind == QK_MUTEX || q->kind == QK_RECURSIVE)
		return xQueueGiveMutexRecursive(h);
	r = p9_msgq_send_isr(&q->q, item, 1) == 1 ? shimPASS : shimFAIL;
	if ((void *)q == pp_queue_id && item != NULL &&
	    *(const uint32_t *)item == 5)
		g_pp_posts++;
	if (r == shimPASS)
		g_shim_qsend_isr_ok++;
	else
		g_shim_qsend_isr_fail++;
	return r;
}

BaseType_t
xQueueReceive(QueueHandle_t h, void *item, TickType_t ticks)
{
	ShimQueue *q = (ShimQueue *)h;
	if (q == NULL)
		return shimFAIL;
	if (q->kind == QK_MUTEX || q->kind == QK_RECURSIVE)
		return xQueueTakeMutexRecursive(h, ticks);
	suspend_check();
	{
		BaseType_t r = p9_msgq_recv(&q->q, item, ticks) == 1
				       ? shimPASS
				       : shimFAIL;
		if (r == shimPASS)
			g_shim_qrecv_ok++;
		else
			g_shim_qrecv_to++;
		return r;
	}
}

BaseType_t
xQueueReceiveFromISR(QueueHandle_t h, void *item, BaseType_t *hptw)
{
	ShimQueue *q = (ShimQueue *)h;

	if (hptw != NULL)
		*hptw = 0;
	if (q == NULL || q->kind == QK_MUTEX || q->kind == QK_RECURSIVE)
		return shimFAIL;
	return p9_msgq_recv_isr(&q->q, item) == 1 ? shimPASS : shimFAIL;
}

BaseType_t
xQueueSemaphoreTake(QueueHandle_t h, TickType_t ticks)
{
	uint8_t tok;
	return xQueueReceive(h, &tok, ticks);
}

QueueHandle_t
xQueueCreateCountingSemaphore(UBaseType_t max, UBaseType_t initial)
{
	ShimQueue *q =
		(ShimQueue *)xQueueGenericCreate(max, 1, QK_COUNTING);
	uint8_t tok = 1;
	UBaseType_t i;

	if (q == NULL)
		return NULL;
	for (i = 0; i < initial; i++)
		p9_msgq_send(&q->q, &tok, 0, 1);
	return (QueueHandle_t)q;
}

QueueHandle_t xQueueCreateMutex(uint8_t type)
{
	return xQueueGenericCreate(1, 1, type == 0 ? QK_MUTEX : type);
}

BaseType_t
xQueueTakeMutexRecursive(QueueHandle_t h, TickType_t ticks)
{
	ShimQueue *q = (ShimQueue *)h;
	ShimTask *me = cur_task();
	(void)ticks;

	if (q == NULL)
		return shimFAIL;
	if (q->holder == me && me != NULL) {
		q->recurse++;
		return shimPASS;
	}
	/* P9QLock has no timeout; ticks are advisory here. */
	p9_qlock(&q->lock);
	q->holder = me;
	q->recurse = 1;
	return shimPASS;
}

BaseType_t
xQueueGiveMutexRecursive(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;

	if (q == NULL)
		return shimFAIL;
	if (q->recurse > 0)
		q->recurse--;
	if (q->recurse == 0) {
		q->holder = NULL;
		p9_qunlock(&q->lock);
	}
	return shimPASS;
}

TaskHandle_t
xQueueGetMutexHolder(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;
	return q != NULL ? (TaskHandle_t)q->holder : NULL;
}

BaseType_t
xQueueGiveFromISR(QueueHandle_t h, BaseType_t *hptw)
{
	uint8_t tok = 1;
	return xQueueGenericSendFromISR(h, &tok, hptw);
}

UBaseType_t
uxQueueMessagesWaiting(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;
	return q != NULL ? p9_msgq_num(&q->q) : 0;
}

UBaseType_t
uxQueueSpacesAvailable(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;
	return q != NULL ? q->q.cap - p9_msgq_num(&q->q) : 0;
}

BaseType_t
xQueueIsQueueFullFromISR(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;
	return q != NULL && p9_msgq_num(&q->q) >= q->q.cap ? 1 : 0;
}

void
vQueueDelete(QueueHandle_t h)
{
	ShimQueue *q = (ShimQueue *)h;
	if (q == NULL)
		return;
	free(q->storage);
	free(q);
}

/* ---- event groups ---- */
typedef struct {
	uint32_t bits;
} ShimEventGroup;

EventGroupHandle_t
xEventGroupCreate(void)
{
	ShimEventGroup *g = (ShimEventGroup *)malloc(sizeof(*g));
	if (g != NULL)
		g->bits = 0;
	return (EventGroupHandle_t)g;
}

EventBits_t
xEventGroupSetBits(EventGroupHandle_t h, EventBits_t bits)
{
	ShimEventGroup *g = (ShimEventGroup *)h;
	if (g == NULL)
		return 0;
	g->bits |= bits;
	return g->bits;
}

EventBits_t
xEventGroupClearBits(EventGroupHandle_t h, EventBits_t bits)
{
	ShimEventGroup *g = (ShimEventGroup *)h;
	EventBits_t before;
	if (g == NULL)
		return 0;
	before = g->bits;
	g->bits &= ~bits;
	return before;
}

EventBits_t
xEventGroupWaitBits(EventGroupHandle_t h, EventBits_t want,
		    BaseType_t clear_on_exit, BaseType_t wait_all,
		    TickType_t ticks)
{
	ShimEventGroup *g = (ShimEventGroup *)h;
	uint32_t start = xTaskGetTickCount();

	if (g == NULL)
		return 0;
	suspend_check();
	for (;;) {
		uint32_t b = g->bits;
		int ok = wait_all ? ((b & want) == want) : ((b & want) != 0);
		if (ok) {
			if (clear_on_exit)
				g->bits &= ~want;
			return b;
		}
		if (ticks != shimBLOCK_FOREVER &&
		    xTaskGetTickCount() - start >= ticks)
			return b;
		p9_yield();
	}
}

void vEventGroupDelete(EventGroupHandle_t h) { free(h); }

void
vEventGroupSetBitsCallback(EventGroupHandle_t h, EventBits_t bits)
{
	xEventGroupSetBits(h, bits);
}

/* ---- timers ---- */
typedef struct ShimTimer {
	char name[16];
	uint32_t period;
	int autoreload;
	int active;
	uint32_t deadline;
	void *id;
	void (*cb)(TimerHandle_t);
	struct ShimTimer *next;
} ShimTimer;

static ShimTimer *timers;
static int timer_daemon_started;
static ShimTask *timer_daemon_task;

static void
timer_daemon(void *arg)
{
	(void)arg;
	timer_daemon_task = cur_task();
	for (;;) {
		ShimTimer *t;
		uint32_t now = xTaskGetTickCount();

		for (t = timers; t != NULL; t = t->next) {
			if (t->active && (int32_t)(now - t->deadline) >= 0) {
				if (t->autoreload)
					t->deadline = now + t->period;
				else
					t->active = 0;
				t->cb((TimerHandle_t)t);
				now = xTaskGetTickCount();
			}
		}
		p9_delay_ticks(1);
	}
}

static void
timer_daemon_ensure(void)
{
	if (!timer_daemon_started) {
		timer_daemon_started = 1;
		xTaskCreate(timer_daemon, "TmrSvc", 1024, NULL, 2, NULL);
	}
}

TimerHandle_t
xTimerCreate(const char *name, TickType_t period, UBaseType_t auto_reload,
	     void *id, void (*cb)(TimerHandle_t))
{
	ShimTimer *t = (ShimTimer *)malloc(sizeof(*t));

	if (t == NULL)
		return NULL;
	memset(t, 0, sizeof(*t));
	if (name != NULL)
		strncpy(t->name, name, sizeof(t->name) - 1);
	t->period = period;
	t->autoreload = (int)auto_reload;
	t->id = id;
	t->cb = cb;
	t->next = timers;
	timers = t;
	timer_daemon_ensure();
	return (TimerHandle_t)t;
}

/* timer commands: 0 start, 1 reset, 2 stop, 3 change-period,
 * 4 delete(unsupported), 6 start-from-isr, 7 reset-from-isr */
BaseType_t
xTimerGenericCommand(TimerHandle_t h, BaseType_t cmd, TickType_t value,
		     BaseType_t *hptw, TickType_t ticks)
{
	ShimTimer *t = (ShimTimer *)h;
	(void)ticks;
	if (hptw != NULL)
		*hptw = 0;
	if (t == NULL)
		return shimFAIL;
	switch (cmd) {
	case 0:
	case 1:
	case 6:
	case 7:
		t->deadline = xTaskGetTickCount() + t->period;
		t->active = 1;
		break;
	case 2:
		t->active = 0;
		break;
	case 3:
		t->period = value;
		break;
	default:
		break;
	}
	return shimPASS;
}

void *
pvTimerGetTimerID(TimerHandle_t h)
{
	ShimTimer *t = (ShimTimer *)h;
	return t != NULL ? t->id : NULL;
}

TaskHandle_t
xTimerGetTimerDaemonTaskHandle(void)
{
	return (TaskHandle_t)timer_daemon_task;
}

BaseType_t
xTimerPendFunctionCallFromISR(void (*fn)(void *, uint32_t), void *arg1,
			      uint32_t arg2, BaseType_t *hptw)
{
	if (hptw != NULL)
		*hptw = 0;
	if (fn == NULL)
		return shimFAIL;
	fn(arg1, arg2);
	return shimPASS;
}

/* ---- stream buffers over a byte queue ---- */
typedef struct {
	P9MsgQ q;
	void *storage;
} ShimStream;

StreamBufferHandle_t
xStreamBufferGenericCreate(size_t size, size_t trigger)
{
	ShimStream *b = (ShimStream *)malloc(sizeof(*b));
	(void)trigger;
	if (b == NULL)
		return NULL;
	if (size == 0)
		size = 1;
	b->storage = malloc(size);
	if (b->storage == NULL) {
		free(b);
		return NULL;
	}
	p9_msgq_init(&b->q, b->storage, (uint32_t)size, 1);
	return (StreamBufferHandle_t)b;
}

size_t
xStreamBufferSend(StreamBufferHandle_t h, const void *data, size_t len,
		  TickType_t ticks)
{
	ShimStream *b = (ShimStream *)h;
	const uint8_t *p = (const uint8_t *)data;
	size_t i;

	if (b == NULL)
		return 0;
	for (i = 0; i < len; i++)
		if (p9_msgq_send(&b->q, p + i, ticks, 1) != 1)
			break;
	return i;
}

size_t
xStreamBufferReceive(StreamBufferHandle_t h, void *data, size_t len,
		     TickType_t ticks)
{
	ShimStream *b = (ShimStream *)h;
	uint8_t *p = (uint8_t *)data;
	size_t i;

	if (b == NULL)
		return 0;
	for (i = 0; i < len; i++) {
		if (p9_msgq_recv(&b->q, p + i, i == 0 ? ticks : 0) != 1)
			break;
	}
	return i;
}

size_t
xStreamBufferSendFromISR(StreamBufferHandle_t h, const void *data,
			 size_t len, BaseType_t *hptw)
{
	size_t n = xStreamBufferSend(h, data, len, 0);
	if (hptw != NULL)
		*hptw = n > 0 ? 1 : 0;
	return n;
}

size_t
xStreamBufferReceiveFromISR(StreamBufferHandle_t h, void *data, size_t len,
			    BaseType_t *hptw)
{
	size_t n = xStreamBufferReceive(h, data, len, 0);
	if (hptw != NULL)
		*hptw = n > 0 ? 1 : 0;
	return n;
}

size_t
xStreamBufferBytesAvailable(StreamBufferHandle_t h)
{
	ShimStream *b = (ShimStream *)h;
	return b != NULL ? p9_msgq_num(&b->q) : 0;
}

size_t
xStreamBufferSpacesAvailable(StreamBufferHandle_t h)
{
	ShimStream *b = (ShimStream *)h;
	return b != NULL ? b->q.cap - p9_msgq_num(&b->q) : 0;
}

void
vStreamBufferDelete(StreamBufferHandle_t h)
{
	ShimStream *b = (ShimStream *)h;
	if (b == NULL)
		return;
	free(b->storage);
	free(b);
}

BaseType_t
xTaskNotifyWait(uint32_t clear_in, uint32_t clear_out, uint32_t *value,
		TickType_t ticks)
{
	ShimTask *t = cur_task();
	uint32_t start = xTaskGetTickCount();

	if (t == NULL)
		return shimFAIL;
	t->notify_val &= ~clear_in;
	for (;;) {
		if (t->notify_pending) {
			if (value != NULL)
				*value = t->notify_val;
			t->notify_val &= ~clear_out;
			if (t->notify_val == 0)
				t->notify_pending = 0;
			return shimPASS;
		}
		if (ticks != shimBLOCK_FOREVER &&
		    xTaskGetTickCount() - start >= ticks)
			return shimFAIL;
		p9_yield();
	}
}
