/* p9_os_xtensa - Xtensa substrate for the p9 primitives.
 *
 * Phase C v1 (doc/PLAN9-OS.md): cooperative procs on the
 * ESP8266 behind the SDK's own init, replacing the POSIX test
 * substrate hook-for-hook.
 *
 * How procs work here:
 * - Each proc is a setjmp/longjmp coroutine on its own 4 KB
 *   DRAM stack (3072 B since B17; memwatch showed shallow
 *   high-water, and DRAM is the blobtest constraint). A proc's first entry uses p9_xt_enter (naked
 *   asm: set SP, jump to the C trampoline) - the trampoline
 *   immediately setjmp()s its resume anchor and longjmps back
 *   to the scheduler, so every later resume is a plain
 *   longjmp into that anchor. Windowed-ABI setjmp from the
 *   toolchain libc (jmp_buf = regs[12] + save[4] + retaddr)
 *   does the register-window work; setjmp/longjmp are called
 *   through function pointers so the compiler cannot expand
 *   them as builtins with different rules.
 * - The scheduler context is whoever pumps: the SDK main
 *   context is proc 0 by convention. p9_os_delay_ms and yield
 *   in scheduler context run ready procs round-robin.
 * - Locks are cooperative spin flags. They are never held
 *   across a yield except through p9_os_wait, which unlocks
 *   first (mirroring the rendez discipline in p9.c). NOT
 *   ISR-safe yet: interrupt locking for os_lock is the next
 *   hardening step and is flagged in the B11 report.
 * - p9_os_wait is yield-on-wait: wake is a no-op because every
 *   waiter rechecks its condition after each yield, exactly
 *   as the Plan 9 sleep loop is written. This is cooperative
 *   semantics, not interrupt-driven scheduling.
 */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "p9.h"
#include "memwatch.h"
#include "p9_os_xtensa.h"

extern uint32_t system_get_time(void); /* microseconds */
extern void ets_delay_us(uint32_t us);
extern void esp_task_wdt_reset(void) __attribute__((weak)); /* task_wdt.c */
extern int os_printf_plus(const char *fmt, ...);

static int (*real_setjmp)(jmp_buf) = setjmp;
static void (*real_longjmp)(jmp_buf, int) = longjmp;

/* 8 since B25: blob pp/tcpip/event procs plus srv9p and the
 * UART cons proc did not fit in 6 (srv9p silently never
 * started, found by the missing 9p-listening milestone).
 * Slots are cheap; the stack pool below is the real budget. */
#define MAX_XPROCS 8
/* Proc stacks come from a static pool, sized by task name:
 * the RTOS pp task alone is created with a 6144 B stack
 * (_g_esp_wifi_ppt_task_stk_size); the tcpip task and the
 * 9P server get 4096; everything else 3072. Uniform 6144 B
 * stacks cost more DRAM than this chip has once the server
 * is linked. An overflowing stack grows down into the next
 * pool allocation, so sizes carry margin over the measured
 * high-waters (~2.4 KB worst in bring-up). */
/* 34816 + 1024 for il9p at 3072 (B29). Larger enlargements
 * were bisected to break blob Wi-Fi init (TxNodeNum fault):
 * every pool byte is a byte off the init heap, and B19
 * already showed init starves silently. Keep this small. */
#define XSTACK_POOL_BYTES 35840u

typedef struct XProc XProc;
struct XProc {
	int used;
	int started;
	int done;
	void (*fn)(void *);
	void *arg;
	const char *name;
	uint32_t saved_intlevel; /* PS.INTLEVEL at last yield */
	jmp_buf env;
	XProc *next;
	uint32_t *stack;	/* into stack_pool */
	uint32_t stack_bytes;
};

static XProc xprocs[MAX_XPROCS];
static XProc *xhead;
static XProc *xcur; /* NULL in scheduler context */

/* Scheduler trace for bring-up: last slot run + run counts,
 * read by panicHandler in shim_xtensa.c. */
int g_p9_last_slot = -1;
uint32_t g_p9_run_counts[MAX_XPROCS];
uint32_t g_p9_isr_yield_violations;

/* True when called on an interrupt/NMI stack or inside a
 * dispatched level-1 handler. Yielding there would longjmp
 * out of a live interrupt frame and strand it. */
extern int xPortInIsrContext(void) __attribute__((weak));
static int
in_isr_context(void)
{
	uint32_t sp;
	__asm__ volatile("mov %0, a1" : "=r"(sp));
	if (xPortInIsrContext != NULL && xPortInIsrContext())
		return 1;
	/* A running proc whose SP is inside its own stack is in
	 * proc context by construction: the NMI switches SP to
	 * _chip_nmi_stk, so an NMI frame can never alias a proc
	 * stack. This check must come first (B37): the hardcoded
	 * NMI window below was measured against an old link
	 * layout, and as BSS grew the proc stack pool drifted
	 * into it -- sys_evt's stack reached the window, every
	 * p9_os_delay_ms in it read as ISR context and refused
	 * to yield, and boot wedged in esp_event_loop_create. */
	if (xcur != NULL) {
		uint32_t lo = (uint32_t)(uintptr_t)xcur->stack;

		if (sp >= lo && sp < lo + xcur->stack_bytes)
			return 0;
	}
	/* NMI (WDEV FIQ) context runs on _chip_nmi_stk; no proc or
	 * scheduler stack ever lives there. The level-1 interrupt
	 * stack range is NOT used: the boot/startup stack overlaps
	 * it, which would false-positive during scheduler pumping. */
	if (sp >= 0x3ffef950u && sp < 0x3ffefd50u)
		return 1;
	return 0;
}
uint32_t g_p9_last_now_ms;
uint32_t g_p9_sched_delay_entries;
uint32_t g_p9_last_ccount;
const char *g_p9_slot_names[MAX_XPROCS];
uint8_t g_p9_slot_done[MAX_XPROCS];

static jmp_buf sched_env;
static int rr_hint;

/* File-scope assembly (Xtensa GCC has no naked attribute):
 * p9_xt_enter(pc=a2, arg=a3, sp=a4): set SP, arg into a2, jump. */
extern void p9_xt_enter(void *pc, void *arg, void *sp);
__asm__(
	".globl p9_xt_enter\n"
	".type p9_xt_enter, @function\n"
	"p9_xt_enter:\n"
	"\tmov a15, a2\n"
	"\tmov a1, a4\n"
	"\tmov a2, a3\n"
	"\tjx a15\n");

/* Startup leaves PS.INTLEVEL=2 (rsil in call_start_cpu),
 * which masks every level-1 interrupt (WDEV, timers) forever
 * on this substrate: unlike the real port, no task-entry frame
 * ever lowers it. But forcing INTLEVEL=0 on every resume is
 * also wrong: blob code holds interrupt locks (and flash-cache
 * -off windows, e.g. PHY calibration) across calls that yield,
 * and an FRC/WDEV dispatch into flash code during such a
 * window faults. So INTLEVEL is a per-proc quantity here:
 * run_one saves it when a proc yields and restores it before
 * resuming that proc; the scheduler context itself runs at 0. */
static void
irq_level1_on(void)
{
	uint32_t ps;
	__asm__ volatile("rsr %0, ps" : "=r"(ps));
	ps &= ~0xFu;
	__asm__ volatile("wsr %0, ps\n\trsync" ::"r"(ps));
}

static void
irq_set_intlevel(uint32_t level)
{
	uint32_t ps;
	__asm__ volatile("rsr %0, ps" : "=r"(ps));
	ps = (ps & ~0xFu) | (level & 0xFu);
	__asm__ volatile("wsr %0, ps\n\trsync" ::"r"(ps));
}

static uint32_t
irq_intlevel(void)
{
	uint32_t ps;
	__asm__ volatile("rsr %0, ps" : "=r"(ps));
	return ps & 0xFu;
}

static void
xt_trampoline(void *arg)
{
	XProc *xp = (XProc *)arg;
	if (real_setjmp(xp->env) == 0) {
		xp->started = 1;
		real_longjmp(sched_env, 1);
	}
	irq_level1_on();
	xp->fn(xp->arg);
	xp->done = 1;
	g_p9_slot_done[xp - xprocs] = 1;
	real_longjmp(sched_env, 1);
	for (;;) {
	}
}

/* Run one ready proc until it yields or exits. Called only in
 * scheduler context. Returns 1 if a proc ran. */
extern void *p9_crit_owner_os(void) __attribute__((weak));
extern int p9_crit_nesting(void) __attribute__((weak));
extern void p9_isr_dispatch_pending(void) __attribute__((weak));

/* Only a full lock (PS.INTLEVEL 15, the vPortEnterCritical
 * path) gates other procs and ISR dispatch: those sections
 * wrap flash-cache-off windows (PHY calibration), where no
 * other flash code may run. A lower level (ROM ets_intr_lock
 * raises to 3) only masks interrupts; on the real port other
 * tasks still run under it. Gating on any level > 0
 * deadlocked (B28): tiT yielded inside an ets lock waiting
 * for mailbox work that only the gated-off soft/timer
 * dispatch could deliver, and froze the RX chain with
 * pend=0x480 forever. */
#define XT_FULL_INTLEVEL 15

static int
proc_holds_full_lock(XProc *xp)
{
	return xp->used && !xp->done &&
		xp->saved_intlevel >= XT_FULL_INTLEVEL;
}

int
p9_intlevel_held(void)
{
	int i;
	for (i = 0; i < MAX_XPROCS; i++)
		if (proc_holds_full_lock(&xprocs[i]))
			return 1;
	return 0;
}

__attribute__((weak)) void
p9_stack_blown(int slot)
{
	(void)slot;
	for (;;) {
	}
}

static int
run_one(void)
{
	int tries;
	void *critos = NULL;
	XProc *holder = NULL;
	int hi;
	/* Stack guard: stacks grow down from the top of each
	 * pool allocation; the lowest word must still hold the
	 * fill pattern. A blown stack corrupts the neighbouring
	 * proc's stack and wedges the machine silently, so name
	 * it while we still can. */
	for (hi = 0; hi < MAX_XPROCS; hi++)
		if (xprocs[hi].used && !xprocs[hi].done &&
		    xprocs[hi].stack[0] != 0xA5A5A5A5u)
			p9_stack_blown(hi);
	if (p9_crit_nesting != NULL && p9_crit_owner_os != NULL &&
	    p9_crit_nesting() > 0)
		critos = p9_crit_owner_os();
	/* A proc that yielded with a full interrupt lock
	 * (INTLEVEL 15, our critical sections) may be inside a
	 * global flash-cache-off window: no other proc may
	 * execute until it resumes and unlocks. Lower levels
	 * (ROM ets_intr_lock = 3) do not gate: see
	 * proc_holds_full_lock. */
	for (hi = 0; hi < MAX_XPROCS; hi++)
		if (proc_holds_full_lock(&xprocs[hi])) {
			holder = &xprocs[hi];
			break;
		}
	for (tries = 0; tries < MAX_XPROCS; tries++) {
		XProc *xp;
		rr_hint = (rr_hint + 1) % MAX_XPROCS;
		xp = &xprocs[rr_hint];
		if (!xp->used || xp->done || xp == xcur)
			continue;
		if (critos != NULL && (void *)xp != critos)
			continue; /* critical section (maybe cache-off) held */
		if (holder != NULL && xp != holder)
			continue; /* interrupts locked / cache-off held */
		xcur = xp;
		g_p9_last_slot = rr_hint;
		g_p9_run_counts[rr_hint]++;
		if (real_setjmp(sched_env) == 0) {
			if (!xp->started) {
				void *top = xp->stack + xp->stack_bytes / 4;
				p9_xt_enter((void *)xt_trampoline, xp, top);
			} else {
				irq_set_intlevel(xp->saved_intlevel);
				real_longjmp(xp->env, 1);
			}
		}
		xp->saved_intlevel = irq_intlevel();
		irq_level1_on(); /* scheduler context runs unmasked */
		xcur = NULL;
		return 1;
	}
	return 0;
}

/* ---- time and delay ---- */
extern uint32_t g_esp_ticks_per_us; /* SDK system_api.c */
extern uint64_t g_esp_os_us __attribute__((weak)); /* shim_xtensa.c */

uint64_t
p9_os_now_ms(void)
{
	uint64_t now;
	uint32_t cc;
	__asm__ volatile("rsr %0, ccount" : "=r"(cc));
	g_p9_last_ccount = cc;
	/* The real port resets CCOUNT every tick and accumulates
	 * the elapsed microseconds into g_esp_os_us, so SDK time
	 * (g_esp_os_us + ccount / ticks_per_us) never wraps. This
	 * substrate has no tick reset: without maintenance the
	 * sum wraps to zero every 2^32 cycles (~26.8 s at
	 * 160 MHz, ~53.7 s at 80 MHz) and every deadline in the
	 * system wedges. This function is called at least once
	 * per millisecond from the delay/wait paths, so count
	 * wraps here and keep g_esp_os_us in step. */
	{
		static uint32_t last_cc;
		static uint32_t wraps;
		static int init;
		if (!init) {
			init = 1;
			last_cc = cc;
		} else {
			if (cc < last_cc)
				wraps++;
			last_cc = cc;
			if (&g_esp_os_us != NULL &&
			    g_esp_ticks_per_us != 0)
				g_esp_os_us = ((uint64_t)wraps << 32) /
					g_esp_ticks_per_us;
		}
	}
	now = system_get_time() / 1000;
	g_p9_last_now_ms = (uint32_t)now;
	return now;
}

/* Raw-UART heartbeat helpers: no locks, no heap, safe when
 * every proc is blocked (B21). */
static void
hb_putc(char c)
{
	volatile uint32_t *fifo = (volatile uint32_t *)0x60000000u;
	volatile uint32_t *stat = (volatile uint32_t *)0x6000001Cu;

	while (((*stat >> 16) & 0xFFu) >= 126u) {
	}
	*fifo = (uint32_t)(uint8_t)c;
}

static void
hb_str(const char *m)
{
	while (*m)
		hb_putc(*m++);
}

static void
hb_hex32(uint32_t v)
{
	int i;

	for (i = 28; i >= 0; i -= 4) {
		uint32_t d = (v >> i) & 0xFu;
		hb_putc(d < 10 ? '0' + (char)d : 'a' + (char)(d - 10));
	}
}

void
p9_os_delay_ms(uint32_t ms)
{
	uint64_t end;
	if (in_isr_context()) {
		g_p9_isr_yield_violations++;
		return;
	}
	end = p9_os_now_ms() + ms;
	if (xcur != NULL) {
		/* In a proc: yield the whole delay away in slices. */
		while (p9_os_now_ms() < end) {
			if (real_setjmp(xcur->env) == 0)
				real_longjmp(sched_env, 1);
			/* INTLEVEL restored per-proc by run_one. */
		}
		return;
	}
	/* Scheduler context: pump procs while waiting. This
	 * context is the idle-task equivalent on this substrate,
	 * so it also feeds the task watchdog, as the real idle
	 * hook (esp_internal_idle_hook) does. */
	g_p9_sched_delay_entries++;
	while (p9_os_now_ms() < end) {
		if (esp_task_wdt_reset != NULL)
			esp_task_wdt_reset();
		if (!run_one())
			ets_delay_us(1000);
		irq_level1_on();
		if (p9_isr_dispatch_pending != NULL)
			p9_isr_dispatch_pending();
		/* Liveness heartbeat (B21): raw UART, no locks,
		 * no heap - the only output guaranteed to work
		 * when every proc is blocked. If HB continues
		 * during a stall, the scheduler lives and the
		 * rx/last/count fields say where things stand. */
		{
			static uint64_t last_hb;
			uint64_t now = p9_os_now_ms();
			if (now - last_hb >= 10000) {
				/* RX chain counters (B28): posts =
				 * pp-queue RX deliveries; pd = PendSV
				 * posts from the FIQ ring; dr = soft
				 * drain calls; s7 = soft-interrupt
				 * vector hits; pend = deferred bits
				 * awaiting dispatch. Comparing them
				 * after a stall names the broken
				 * link. */
				extern uint32_t g_pp_posts
					__attribute__((weak));
				extern uint32_t g_pendsv_posts
					__attribute__((weak));
				extern uint32_t g_soft_drains
					__attribute__((weak));
				extern uint32_t g_isr_counts[32]
					__attribute__((weak));
				extern uint32_t g_isr_pending
					__attribute__((weak));
				int i;

				last_hb = now;
				hb_str("HB rx=");
				hb_hex32(&g_pp_posts != NULL ?
					 g_pp_posts : 0);
				hb_str(" pd=");
				hb_hex32(&g_pendsv_posts != NULL ?
					 g_pendsv_posts : 0);
				hb_str(" dr=");
				hb_hex32(&g_soft_drains != NULL ?
					 g_soft_drains : 0);
				hb_str(" s7=");
				hb_hex32(&g_isr_counts != NULL ?
					 g_isr_counts[7] : 0);
				hb_str(" pend=");
				hb_hex32(&g_isr_pending != NULL ?
					 g_isr_pending : 0);
				hb_str(" il=");
				for (i = 0; i < MAX_XPROCS; i++) {
					uint32_t lv = xprocs[i].used ?
						xprocs[i].saved_intlevel : 0;
					hb_putc(!xprocs[i].used ? '.' :
						lv < 10 ? '0' + (char)lv :
						'a' + (char)(lv - 10));
				}
				hb_str(" last=");
				hb_putc(g_p9_last_slot < 0 ? '-' :
					'0' + (char)g_p9_last_slot);
				hb_str(" runs=");
				for (i = 0; i < MAX_XPROCS; i++)
					hb_putc('0' +
						(char)(g_p9_run_counts[i] % 10u));
				hb_putc('\n');
			}
		}
	}
}

void
p9_os_yield(void)
{
	if (in_isr_context()) {
		g_p9_isr_yield_violations++;
		return;
	}
	if (xcur != NULL) {
		if (real_setjmp(xcur->env) == 0)
			real_longjmp(sched_env, 1);
	} else {
		run_one();
		if (p9_isr_dispatch_pending != NULL)
			p9_isr_dispatch_pending();
	}
}

/* ---- threads (procs) ---- */
static uint32_t stack_pool[XSTACK_POOL_BYTES / 4];
static uint32_t stack_pool_used; /* bytes */

static uint32_t
stack_size_for(const char *name)
{
	if (name != NULL) {
		if (strcmp(name, "ppT") == 0)
			return 8192; /* + level-1 handler frames on its stack */
		if (strcmp(name, "tiT") == 0 || strcmp(name, "uiT") == 0 ||
		    strcmp(name, "srv9p") == 0)
			return 4096;
		if (strcmp(name, "sys_evt") == 0)
			return 6144; /* runs esp_wifi_connect + scan_done */
	}
	return 3072;
}

void *
p9_os_thread_create(void (*fn)(void *), void *arg, const char *name)
{
	int i;
	for (i = 0; i < MAX_XPROCS; i++) {
		XProc *xp = &xprocs[i];
		uint32_t bytes;
		if (xp->used && !xp->done)
			continue;
		bytes = stack_size_for(name);
		if (stack_pool_used + bytes > XSTACK_POOL_BYTES) {
			os_printf_plus("p9_os: FATAL stack pool exhausted "
				       "(used %u + %u > %u) creating %s\n",
				       stack_pool_used, bytes,
				       XSTACK_POOL_BYTES,
				       name != NULL ? name : "?");
			return NULL;
		}
		memset(xp, 0, sizeof(*xp));
		xp->stack = stack_pool + stack_pool_used / 4;
		xp->stack_bytes = bytes;
		stack_pool_used += bytes;
		memset(xp->stack, P9_XT_STACK_PATTERN, bytes);
		xp->used = 1;
		xp->name = name;
		g_p9_slot_names[i] = name;
		g_p9_slot_done[i] = 0;
		xp->fn = fn;
		xp->arg = arg;
		xp->next = xhead;
		xhead = xp;
		return xp;
	}
	os_printf_plus("p9_os: FATAL out of procs (%d slots) creating %s\n",
		       MAX_XPROCS, name != NULL ? name : "?");
	return NULL;
}

void
p9_os_thread_exit(void)
{
	if (xcur != NULL) {
		xcur->done = 1;
		real_longjmp(sched_env, 1);
	}
	for (;;)
		ets_delay_us(100000);
}

void *
p9_os_current(void)
{
	return xcur;
}

/* ---- locks: cooperative spin flags ---- */
#define MAX_OSLOCKS 32
static volatile uint8_t oslock_flags[MAX_OSLOCKS];
static int oslock_next;

void
p9_os_lock_init(P9OsLock *l)
{
	int idx = oslock_next;
	if (idx >= MAX_OSLOCKS) {
		l->impl = NULL;
		return;
	}
	oslock_next++;
	oslock_flags[idx] = 0;
	l->impl = (void *)(uintptr_t)(idx + 1);
}

void
p9_os_lock(P9OsLock *l)
{
	uint32_t idx;
	if (l == NULL || l->impl == NULL)
		return;
	idx = (uint32_t)(uintptr_t)l->impl - 1;
	while (oslock_flags[idx]) {
		if (xcur != NULL)
			p9_os_yield();
		else
			ets_delay_us(50);
	}
	oslock_flags[idx] = 1;
}

void
p9_os_unlock(P9OsLock *l)
{
	uint32_t idx;
	if (l == NULL || l->impl == NULL)
		return;
	idx = (uint32_t)(uintptr_t)l->impl - 1;
	oslock_flags[idx] = 0;
}

int
p9_os_trylock(P9OsLock *l)
{
	uint32_t idx;
	if (l == NULL || l->impl == NULL)
		return 0;
	idx = (uint32_t)(uintptr_t)l->impl - 1;
	if (oslock_flags[idx])
		return 0;
	oslock_flags[idx] = 1;
	return 1;
}

/* ---- wait/wake: yield-on-wait (see file header) ---- */
void p9_os_wait_init(P9Wait *w) { (void)w; }

void
p9_os_wait(P9Wait *w, P9OsLock *l)
{
	(void)w;
	p9_os_unlock(l);
	p9_os_yield();
	if (xcur == NULL)
		ets_delay_us(200);
	p9_os_lock(l);
}

void p9_os_wake(P9Wait *w, int all)
{
	(void)w;
	(void)all;
}

/* ---- debug: stack high-water and register snapshot ---- */
uint32_t
p9_os_stack_slots(void)
{
	return MAX_XPROCS;
}

uint32_t
p9_os_proc_slots(void)
{
	return MAX_XPROCS;
}

uint32_t
p9_os_proc_used(void)
{
	uint32_t n = 0;
	int i;

	for (i = 0; i < MAX_XPROCS; i++)
		if (xprocs[i].used && !xprocs[i].done)
			n++;
	return n;
}

const char *
p9_os_proc_name(uint32_t slot)
{
	if (slot >= MAX_XPROCS || !xprocs[slot].used)
		return NULL;
	return xprocs[slot].name;
}

uint32_t
p9_os_stack_highwater(uint32_t slot)
{
	if (slot >= MAX_XPROCS || !xprocs[slot].used)
		return 0;
	return memwatch_stack_used((const uint8_t *)xprocs[slot].stack,
				   xprocs[slot].stack_bytes,
				   P9_XT_STACK_PATTERN);
}

void
p9_os_capture_regs(P9XtRegs *r)
{
	uint32_t sp;

	if (r == NULL)
		return;
	memset(r, 0, sizeof(*r));
	__asm__ volatile("mov %0, a1" : "=r"(sp));
	r->sp = sp;
	r->pc = (uint32_t)__builtin_return_address(0);
	r->a0 = r->pc;
	__asm__ volatile("rsr %0, ps" : "=r"(r->ps));
	__asm__ volatile("rsr %0, exccause" : "=r"(r->exccause));
	__asm__ volatile("rsr %0, excvaddr" : "=r"(r->excvaddr));
	__asm__ volatile("rsr %0, epc1" : "=r"(r->epc1));
	__asm__ volatile("rsr %0, intenable" : "=r"(r->intenable));
	__asm__ volatile("rsr %0, interrupt" : "=r"(r->interrupt));
	__asm__ volatile("rsr %0, ccount" : "=r"(r->ccount));
	__asm__ volatile("rsr %0, sar" : "=r"(r->sar));
	__asm__ volatile("rsr %0, vecbase" : "=r"(r->vecbase));
	__asm__ volatile("rsr %0, prid" : "=r"(r->prid));
}
