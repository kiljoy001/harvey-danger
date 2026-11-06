/* p9 - portable Plan 9 primitives; see p9.h for the split. */
#include "p9.h"

#include <string.h>

/* ---- QLock (FIFO ticket) ---- */
void
p9_qlock_init(P9QLock *q)
{
	p9_os_lock_init(&q->lock);
	p9_os_wait_init(&q->wait);
	q->next_ticket = 0;
	q->serving = 0;
	q->locked = 0;
}

void
p9_qlock(P9QLock *q)
{
	uint32_t ticket;
	p9_os_lock(&q->lock);
	ticket = q->next_ticket++;
	while (q->locked || ticket != q->serving)
		p9_os_wait(&q->wait, &q->lock);
	q->locked = 1;
	p9_os_unlock(&q->lock);
}

int
p9_canqlock(P9QLock *q)
{
	int ok = 0;
	p9_os_lock(&q->lock);
	if (!q->locked && q->serving == q->next_ticket) {
		q->next_ticket++;
		q->locked = 1;
		ok = 1;
	}
	p9_os_unlock(&q->lock);
	return ok;
}

void
p9_qunlock(P9QLock *q)
{
	p9_os_lock(&q->lock);
	q->locked = 0;
	q->serving++;
	p9_os_wake(&q->wait, 1);
	p9_os_unlock(&q->lock);
}

/* ---- Rendez ---- */
void
p9_rendez_init(P9Rendez *r)
{
	p9_os_wait_init(&r->wait);
}

void
p9_sleep(P9Rendez *r, P9OsLock *lock)
{
	p9_os_wait(&r->wait, lock);
}

void
p9_wakeup(P9Rendez *r)
{
	/* Waking needs the wait's internal lock protected wake;
	 * the POSIX hook serialises with its own mutex by waking
	 * through a zero-length critical section is not possible
	 * here, so wake is issued directly: pthread cond signal is
	 * safe without holding the lock. Target hook likewise. */
	p9_os_wake(&r->wait, 0);
}

/* ---- scheduler gate ---- */
static P9OsLock sched_lock;
static P9Wait sched_wait;
static int sched_ready;
static int sched_holds;
static P9Proc *sched_owner;
int g_p9_sched_holds; /* debug mirror for panic dumps */

static void
sched_init_once(void)
{
	if (!sched_ready) {
		p9_os_lock_init(&sched_lock);
		p9_os_wait_init(&sched_wait);
		sched_ready = 1;
	}
}

void
p9_sched_suspend_all(void)
{
	P9Proc *me;
	sched_init_once();
	me = p9_current();
	p9_os_lock(&sched_lock);
	/* FreeRTOS suspend-all leaves the caller running; on this
	 * cooperative substrate that means the suspend is owned:
	 * p9_checkpoint blocks everyone except the owner. A second
	 * proc suspending while held nests the count but does not
	 * steal ownership. */
	if (sched_holds == 0)
		sched_owner = me;
	sched_holds++;
	g_p9_sched_holds = sched_holds;
	p9_os_unlock(&sched_lock);
}

void
p9_sched_resume_all(void)
{
	sched_init_once();
	p9_os_lock(&sched_lock);
	if (sched_holds > 0)
		sched_holds--;
	if (sched_holds == 0)
		sched_owner = NULL;
	g_p9_sched_holds = sched_holds;
	if (sched_holds == 0)
		p9_os_wake(&sched_wait, 1);
	p9_os_unlock(&sched_lock);
}

void
p9_checkpoint(void)
{
	sched_init_once();
	p9_os_lock(&sched_lock);
	while (sched_holds > 0 && p9_current() != sched_owner)
		p9_os_wait(&sched_wait, &sched_lock);
	p9_os_unlock(&sched_lock);
}

/* ---- Proc ---- */
static P9OsLock procs_lock;
static int procs_ready;
static P9Proc *procs_head;
static P9Proc proc_pool[16];
static int proc_pool_used;

typedef struct {
	P9Proc *proc;
	void (*fn)(void *);
	void *arg;
	volatile int copied;
} ProcBoot;

static ProcBoot proc_boots[16];
static int proc_boots_used;

static void
proc_trampoline(void *arg)
{
	ProcBoot *slot = (ProcBoot *)arg;
	ProcBoot boot = *slot;
	slot->copied = 1;
	boot.fn(boot.arg);
	boot.proc->exited = 1;
	p9_os_thread_exit();
}

P9Proc *
p9_proc_create(const char *name, void (*fn)(void *), void *arg)
{
	P9Proc *p;
	if (!procs_ready) {
		p9_os_lock_init(&procs_lock);
		procs_ready = 1;
	}
	p9_checkpoint();
	p9_os_lock(&procs_lock);
	if (proc_pool_used >= 16) {
		p9_os_unlock(&procs_lock);
		return NULL;
	}
	p = &proc_pool[proc_pool_used++];
	memset(p, 0, sizeof(*p));
	strncpy(p->name, name, sizeof(p->name) - 1);
	p->next = procs_head;
	procs_head = p;
	p9_os_unlock(&procs_lock);

	if (proc_boots_used >= 16)
		return NULL;
	{
		ProcBoot *slot = &proc_boots[proc_boots_used++];
		slot->proc = p;
		slot->fn = fn;
		slot->arg = arg;
		slot->copied = 0;
		p->os = p9_os_thread_create(proc_trampoline, slot, name);
		if (p->os == NULL)
			return NULL;
		while (!slot->copied)
			p9_os_delay_ms(1);
	}
	return p;
}

void
p9_proc_exit(void)
{
	p9_os_thread_exit();
}

P9Proc *
p9_current(void)
{
	void *cur = p9_os_current();
	P9Proc *p;
	if (!procs_ready)
		return NULL;
	for (p = procs_head; p != NULL; p = p->next)
		if (p->os == cur)
			return p;
	return NULL;
}

void
p9_yield(void)
{
	p9_os_yield();
}

void
p9_delay_ms(uint32_t ms)
{
	/* Delay in slices so a scheduler hold is honoured. */
	while (ms > 0) {
		uint32_t slice = ms > 20 ? 20 : ms;
		p9_checkpoint();
		p9_os_delay_ms(slice);
		ms -= slice;
	}
}

uint32_t
p9_ms_to_ticks(uint32_t ms)
{
	return ms / P9_TICK_MS;
}

void
p9_delay_ticks(uint32_t ticks)
{
	if (ticks == P9_BLOCKING) {
		for (;;)
			p9_os_delay_ms(1000);
	}
	p9_delay_ms(ticks * P9_TICK_MS);
}

/* ---- bounded message queue ---- */
void
p9_msgq_init(P9MsgQ *q, void *storage, uint32_t cap, uint32_t item_size)
{
	p9_os_lock_init(&q->lock);
	p9_os_wait_init(&q->not_empty);
	p9_os_wait_init(&q->not_full);
	q->items = (uint8_t *)storage;
	q->item_size = item_size;
	q->cap = cap;
	q->head = 0;
	q->count = 0;
}

__attribute__((weak)) void
p9_qput_bad(void *q, const void *item, int pos, uint32_t items,
	    uint32_t cap, uint32_t sz)
{
	(void)q; (void)item; (void)pos; (void)items; (void)cap; (void)sz;
}

static void
q_put(P9MsgQ *q, const void *item, int pos)
{
	uint32_t at;
	if (q->items == NULL || q->cap == 0) {
		p9_qput_bad(q, item, pos, (uint32_t)(uintptr_t)q->items,
			    q->cap, q->item_size);
		return;
	}
	/* item may be NULL: semaphore gives carry no payload
	 * (FreeRTOS queues with uxItemSize == 0 skip the copy). */
	if (pos == 2 && q->count == q->cap) {
		/* overwrite: replace newest (tail) item */
		at = (q->head + q->count - 1) % q->cap;
		if (item != NULL)
			memcpy(q->items + at * q->item_size, item,
			       q->item_size);
		return;
	}
	if (pos == 0) {
		q->head = (q->head + q->cap - 1) % q->cap;
		at = q->head;
	} else {
		at = (q->head + q->count) % q->cap;
	}
	if (item != NULL)
		memcpy(q->items + at * q->item_size, item, q->item_size);
	q->count++;
}

/* Timed waits are approximated by a 1 ms poll: tick timeouts
 * in this layer are for OSI compatibility, and deadlines are
 * computed from p9_os_now_ms. Portable and substrate-light. */
static int
wait_until(P9MsgQ *q, P9Wait *w, int sending, uint32_t timeout_ticks)
{
	uint64_t deadline = 0;
	if (timeout_ticks != P9_BLOCKING)
		deadline = p9_os_now_ms() + (uint64_t)timeout_ticks * P9_TICK_MS;
	for (;;) {
		int ready = sending ? (q->count < q->cap) : (q->count > 0);
		uint64_t now;
		if (ready)
			return 1;
		if (timeout_ticks == 0)
			return 0;
		now = p9_os_now_ms();
		if (timeout_ticks != P9_BLOCKING && now >= deadline)
			return 0;
		p9_os_unlock(&q->lock);
		p9_os_delay_ms(1);
		p9_os_lock(&q->lock);
		(void)w;
	}
}

int
p9_msgq_send(P9MsgQ *q, const void *item, uint32_t timeout_ticks, int pos)
{
	p9_checkpoint();
	p9_os_lock(&q->lock);
	if (q->count >= q->cap && pos != 2) {
		if (!wait_until(q, &q->not_full, 1, timeout_ticks)) {
			p9_os_unlock(&q->lock);
			return 0;
		}
	}
	q_put(q, item, pos);
	p9_os_wake(&q->not_empty, 1);
	p9_os_unlock(&q->lock);
	return 1;
}

int
p9_msgq_send_isr(P9MsgQ *q, const void *item, int pos)
{
	if (!p9_os_trylock(&q->lock))
		return 0;
	if (q->count >= q->cap && pos != 2) {
		p9_os_unlock(&q->lock);
		return 0;
	}
	q_put(q, item, pos);
	p9_os_unlock(&q->lock);
	return 1;
}

int
p9_msgq_recv_isr(P9MsgQ *q, void *item)
{
	if (!p9_os_trylock(&q->lock))
		return 0;
	if (q->count == 0) {
		p9_os_unlock(&q->lock);
		return 0;
	}
	if (item != NULL)
		memcpy(item, q->items + q->head * q->item_size,
		       q->item_size);
	q->head = (q->head + 1) % q->cap;
	q->count--;
	p9_os_unlock(&q->lock);
	return 1;
}

int
p9_msgq_recv(P9MsgQ *q, void *item, uint32_t timeout_ticks)
{
	p9_checkpoint();
	p9_os_lock(&q->lock);
	if (q->count == 0) {
		if (!wait_until(q, &q->not_empty, 0, timeout_ticks)) {
			p9_os_unlock(&q->lock);
			return 0;
		}
	}
	if (item != NULL)
		memcpy(item, q->items + q->head * q->item_size,
		       q->item_size);
	q->head = (q->head + 1) % q->cap;
	q->count--;
	p9_os_wake(&q->not_full, 1);
	p9_os_unlock(&q->lock);
	return 1;
}

uint32_t
p9_msgq_num(const P9MsgQ *q)
{
	return q->count;
}
