/* shim_xtensa - target-only port symbols the RTOS components
 * expect from the FreeRTOS port layer. */
#include <stdint.h>
#include <stddef.h>
#include "driver/soc.h"

/* ROM ets_isr_attach, fixed address on ESP8266. */
static void rom_ets_isr_attach(int intr, void *handler, void *arg)
{
	((void (*)(int, void *, void *))0x40000f88)(intr, handler, arg);
}
#define ets_isr_attach rom_ets_isr_attach

/* _chip_interrupt_stk/_tmp and _chip_nmi_* come from
 * xtensa_vectors.S, as in the real port. */
uint32_t _xt_tick_divisor = 5000;
uint32_t g_esp_boot_ccount;
/* Must match port.c's type exactly: esp_timer_get_time()
 * reads g_esp_os_us as uint64_t; a 32-bit definition here
 * made every SDK time read garbage in its high word. */
uint64_t g_esp_os_us;

/* ISR table for _xt_isr_handler (port.c equivalent). */
typedef void (*xtisr_t)(void*);
struct xtisr_entry { xtisr_t handler; void* arg; };
static struct xtisr_entry s_isr_table[32];
uint8_t s_xt_isr_status;

void
_xt_isr_attach(int intr, void *handler, void *arg)
{
	if (intr >= 0 && intr < 32) {
		s_isr_table[intr].handler = (xtisr_t)handler;
		s_isr_table[intr].arg = arg;
	}
	ets_isr_attach(intr, handler, arg);
}

int
xPortInIsrContext(void)
{
	return s_xt_isr_status != 0;
}

void portYIELD_FROM_ISR(int woken)
{
	/* No yield out of an interrupt frame (see osi_wifi.c);
	 * the cooperative scheduler pumps ready procs anyway. */
	(void)woken;
}

uint32_t
prvGetExpectedIdleTime(void)
{
	return 0;
}

int
interrupt_is_disable(void)
{
	uint32_t ps;
	__asm__ volatile("rsr %0, ps" : "=r"(ps));
	return (ps & 0xF) != 0;
}

static void raw_putc(char c)
{
	volatile uint32_t *fifo = (volatile uint32_t *)0x60000000u;
	volatile uint32_t *stat = (volatile uint32_t *)0x6000001Cu;
	while (((*stat >> 16) & 0xFFu) >= 126u) {
	}
	*fifo = (uint32_t)(uint8_t)c;
}

static void raw_hex(uint32_t v)
{
	int i;
	for (i = 28; i >= 0; i -= 4) {
		uint32_t d = (v >> i) & 0xFu;
		raw_putc(d < 10 ? '0' + (char)d : 'a' + (char)(d - 10));
	}
}

/* Fault containment unit on this hardware is the reboot:
 * no MPU means shared state cannot be trusted after a
 * fault, so dump, flush, and restart rather than hang for
 * the watchdog. The namespace is served fresh after boot;
 * clients remount. (CMRX-style per-process termination is
 * not available without memory protection.) */
extern void esp_restart(void);

static void
fatal_reboot(void)
{
	const char *m = "REBOOT\n";
	uint32_t cc0, cc;
	while (*m)
		raw_putc(*m++);
	__asm__ volatile("rsr %0, ccount" : "=r"(cc0));
	do {
		__asm__ volatile("rsr %0, ccount" : "=r"(cc));
	} while ((uint32_t)(cc - cc0) < 16000000u); /* ~0.1 s flush */
	esp_restart();
	for (;;) {
	}
}

void
p9_stack_blown(int slot)
{
	const char *m = "\nSTACKBLOW slot=";
	while (*m)
		raw_putc(*m++);
	raw_putc('0' + (char)slot);
	raw_putc('\n');
	fatal_reboot();
}

void
panicHandler(void *frame, int wdt)
{
	uint32_t *f = (uint32_t *)frame;
	const char *m = "\nPANIC wdt=";
	while (*m)
		raw_putc(*m++);
	raw_putc(wdt ? '1' : '0');
	m = " frame=";
	while (*m)
		raw_putc(*m++);
	if (f != NULL) {
		int i;
		for (i = 0; i < 8; i++) {
			raw_putc(' ');
			raw_hex(f[i]);
		}
		if (wdt == 0) {
			/* xtensa frame: PC@1 PS@2 EXCCAUSE@20 EXCVADDR@21 */
			const char *m2 = " PC=";
			while (*m2)
				raw_putc(*m2++);
			raw_hex(f[1]);
			m2 = " CAUSE=";
			while (*m2)
				raw_putc(*m2++);
			raw_hex(f[20]);
			m2 = " VADDR=";
			while (*m2)
				raw_putc(*m2++);
			raw_hex(f[21]);
			m2 = " A12-15=";
			while (*m2)
				raw_putc(*m2++);
			raw_hex(f[15]);
			raw_putc('/');
			raw_hex(f[16]);
			raw_putc('/');
			raw_hex(f[17]);
			raw_putc('/');
			raw_hex(f[18]);
			{
				extern uint32_t g_isr_counts[32];
				extern uint32_t g_isr_min_sp;
				m2 = " C10=";
				while (*m2)
					raw_putc(*m2++);
				raw_hex(g_isr_counts[10]);
				m2 = " MINSP=";
				while (*m2)
					raw_putc(*m2++);
				raw_hex(g_isr_min_sp);
				m2 = " STKBOT=";
				while (*m2)
					raw_putc(*m2++);
				raw_hex(*(volatile uint32_t *)0x3ffe8220u);
			}
		}
	}
	raw_putc('\n');
	{
		extern int g_p9_last_slot;
		extern uint32_t g_p9_run_counts[];
		const char *m = "LASTSLOT=";
		while (*m)
			raw_putc(*m++);
		raw_putc(g_p9_last_slot < 0 ? '-' : '0' + (char)g_p9_last_slot);
		m = " COUNTS=";
		while (*m)
			raw_putc(*m++);
		{
			int i;
			for (i = 0; i < 6; i++)
				raw_hex(g_p9_run_counts[i]);
		}
		{
			extern uint32_t g_p9_last_now_ms;
			extern uint32_t g_p9_sched_delay_entries;
			m = " NOW=";
			while (*m)
				raw_putc(*m++);
			raw_hex(g_p9_last_now_ms);
			m = " SD=";
			while (*m)
				raw_putc(*m++);
			raw_hex(g_p9_sched_delay_entries);
			{
				extern uint32_t g_p9_isr_yield_violations;
				m = " ISRY=";
				while (*m)
					raw_putc(*m++);
				raw_hex(g_p9_isr_yield_violations);
			}
			{
				extern uint32_t shim_debug_suspended_mask(void);
				m = " SUSP=";
				while (*m)
					raw_putc(*m++);
				raw_hex(shim_debug_suspended_mask());
			}
			{
				extern uint32_t g_isr_last_idx, g_isr_last_handler, g_isr_phase;
				m = " ISR=";
				while (*m)
					raw_putc(*m++);
				raw_hex(g_isr_last_idx);
				raw_putc('/');
				raw_hex(g_isr_last_handler);
				raw_putc('/');
				raw_hex(g_isr_phase);
			}
			{
				extern int g_p9_sched_holds;
				m = " HOLDS=";
				while (*m)
					raw_putc(*m++);
				raw_hex((uint32_t)g_p9_sched_holds);
			}
			{
				extern uint32_t g_p9_last_ccount;
				uint32_t cc;
				__asm__ volatile("rsr %0, ccount" : "=r"(cc));
				m = " CC=";
				while (*m)
					raw_putc(*m++);
				raw_hex(g_p9_last_ccount);
				m = "/";
				while (*m)
					raw_putc(*m++);
				raw_hex(cc);
			}
		}
		{
			extern const char *g_p9_slot_names[];
			extern uint8_t g_p9_slot_done[];
			int i;
			for (i = 0; i < 7; i++) {
				const char *nm = g_p9_slot_names[i];
				int k;
				raw_putc(' ');
				raw_putc('0' + (char)i);
				raw_putc(g_p9_slot_done[i] ? 'X' : ':');
				if (nm == NULL) {
					raw_putc('-');
					continue;
				}
				for (k = 0; k < 12 && nm[k] != '\0'; k++)
					raw_putc(nm[k]);
			}
		}
		raw_putc('\n');
	}
	fatal_reboot();
}


/* NONOS-named helpers blob_support expects, RTOS-gen backed. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"

/* NMI allocation pool (B21). The blob allocates RX buffers
 * from the WDEV FIQ (esf_rx_buf_alloc -> pvPortMalloc ->
 * newlib malloc). If the NMI interrupts a proc inside
 * malloc, the recursive heap lock is acquired "by the same
 * task" and malloc re-enters, corrupting the heap; a later
 * malloc then spins on the corrupted free list - silent,
 * permanent, and load-dependent (our bench wedge). So the
 * FIQ never touches the heap: it allocates from this small
 * fixed-block pool (carved from the heap once, before the
 * radio starts). Block state is one byte per block, only
 * ever stored (never RMW), so NMI alloc vs proc free cannot
 * tear. Oversize or exhausted: return NULL - the blob
 * already drops frames on esf NULL. */
#define NMI_POOL_BLOCKS 6
#define NMI_POOL_BLOCKSZ 1664u
static uint8_t *nmi_pool;
static volatile uint8_t nmi_pool_taken[NMI_POOL_BLOCKS];
extern uint8_t NMIIrqIsOn; /* blob flag: FIQ handler active */

static void *
nmi_pool_alloc(size_t sz)
{
	int i;
	if (sz > NMI_POOL_BLOCKSZ)
		return NULL;
	for (i = 0; i < NMI_POOL_BLOCKS; i++) {
		if (nmi_pool_taken[i] == 0) {
			nmi_pool_taken[i] = 1;
			return nmi_pool + (size_t)i * NMI_POOL_BLOCKSZ;
		}
	}
	return NULL;
}

static int
nmi_pool_free(void *p)
{
	uintptr_t base = (uintptr_t)nmi_pool;
	uintptr_t q = (uintptr_t)p;
	if (nmi_pool == NULL || q < base ||
	    q >= base + NMI_POOL_BLOCKS * NMI_POOL_BLOCKSZ)
		return 0;
	nmi_pool_taken[(q - base) / NMI_POOL_BLOCKSZ] = 0;
	return 1;
}

void *pvPortMalloc(size_t sz, const char *file, int line)
{
	(void)file;
	(void)line;
	if (NMIIrqIsOn != 0 && nmi_pool != NULL)
		return nmi_pool_alloc(sz);
	if (nmi_pool == NULL && NMIIrqIsOn == 0)
		nmi_pool = (uint8_t *)malloc(NMI_POOL_BLOCKS *
					     NMI_POOL_BLOCKSZ);
	return malloc(sz);
}

void vPortFree(void *p, const char *file, int line)
{
	(void)file;
	(void)line;
	if (p != NULL && nmi_pool_free(p))
		return;
	free(p);
}

int os_printf_plus(const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}

uint32_t system_get_time(void)
{
	return (uint32_t)esp_timer_get_time();
}

typedef int esp_err_t_shim;
void _esp_error_check_failed(esp_err_t_shim rc, const char *file, int line,
			     const char *function, const char *expression)
    __attribute__((noreturn));
void
_esp_error_check_failed(esp_err_t_shim rc, const char *file, int line,
			const char *function, const char *expression)
{
	(void)rc;
	(void)file;
	(void)line;
	(void)function;
	(void)expression;
	for (;;) {
	}
}

void
_esp_error_check_failed_without_abort(esp_err_t_shim rc, const char *file,
				      int line, const char *function,
				      const char *expression)
{
	(void)rc;
	(void)file;
	(void)line;
	(void)function;
	(void)expression;
}

/* Critical sections (shared with blob_support.c's NONOS
 * implementation; here they are the only copy). */
static volatile uint32_t crit_nesting;
static uint32_t crit_saved_ps;
static void *crit_owner_os;

#include "p9.h"

/* Scheduler gate hooks for p9_os_xtensa.c: while a proc holds
 * a critical section it may still yield through this substrate
 * (delays/queues poll), and blob critical sections wrap
 * flash-cache-off windows (PHY calibration) in which no other
 * proc may execute flash code. run_one therefore runs only
 * the owning proc until the section closes. */
void *p9_crit_owner_os(void) { return crit_owner_os; }
int p9_crit_nesting(void) { return (int)crit_nesting; }

extern void p9_yield(void);

static uint32_t rsr_ps(void)
{
	uint32_t v;
	__asm__ volatile("rsr %0, ps" : "=r"(v));
	return v;
}

static void wsr_ps(uint32_t v)
{
	__asm__ volatile("wsr %0, ps\n\trsync" ::"r"(v));
}

void vPortEnterCritical(void)
{
	static int marked;
	uint32_t ps;
	if (!marked) {
		marked = 1;
		{
			volatile uint32_t *fifo = (volatile uint32_t *)0x60000000u;
			*fifo = (uint32_t)'H';
		}
	}
	ps = rsr_ps();
	if (crit_nesting == 0) {
		P9Proc *me = p9_current();
		crit_owner_os = me != NULL ? me->os : NULL;
		crit_saved_ps = ps;
		wsr_ps(ps | 0xFu);
	}
	crit_nesting++;
}

void vPortExitCritical(void)
{
	if (crit_nesting > 0)
		crit_nesting--;
	if (crit_nesting == 0) {
		crit_owner_os = NULL;
		wsr_ps(crit_saved_ps);
	}
}

/* Faithful port.c semantics, not just INTLEVEL: the WDEV
 * interrupt is NMI, which INTLEVEL cannot mask, so the real
 * lock also narrows INT_ENA_WDEV to the TSF0-reach event for
 * the duration. Blob code wraps RX/TX descriptor-ring surgery
 * in these calls assuming the FIQ is held off; with only
 * INTLEVEL raised, wDev_ProcessFiq could (and did) re-enter
 * mid-surgery and leave the RX ring inconsistent. */
#define SHIM_NMI_INT_ENABLE_REG (*(volatile uint32_t *)0x3ff00000u)
#define SHIM_INT_ENA_WDEV (*(volatile uint32_t *)0x3ff20c18u)
#define SHIM_WDEV_TSF0_REACH_INT (1u << 27)
extern uint32_t WDEV_INTEREST_EVENT;
extern uint8_t NMIIrqIsOn; /* blob flag: FIQ handler active */

void vPortETSIntrLock(void)
{
	uint32_t regval;
	if (NMIIrqIsOn != 0)
		return; /* port.c: no-op inside the FIQ */
	regval = SHIM_NMI_INT_ENABLE_REG;
	vPortEnterCritical();
	SHIM_NMI_INT_ENABLE_REG = 0;
	do {
		SHIM_INT_ENA_WDEV = SHIM_WDEV_TSF0_REACH_INT;
	} while (SHIM_INT_ENA_WDEV != SHIM_WDEV_TSF0_REACH_INT);
	SHIM_NMI_INT_ENABLE_REG = regval;
}

void vPortETSIntrUnlock(void)
{
	uint32_t regval;
	if (NMIIrqIsOn != 0)
		return;
	regval = SHIM_NMI_INT_ENABLE_REG;
	SHIM_NMI_INT_ENABLE_REG = 0;
	SHIM_INT_ENA_WDEV = WDEV_INTEREST_EVENT;
	SHIM_NMI_INT_ENABLE_REG = regval;
	vPortExitCritical();
}


/* Soft interrupt (ETS_SOFT_INUM): blob pp_post() queues
 * sig>=4 work (notably RX, sig 5) into a lock-free ring and
 * calls PendSV(2), expecting the soft ISR to transfer ring
 * entries into the pp queue via MacIsrSigPostDefHdl(). This
 * is load-bearing: without it the pp task never learns about
 * received packets. Our earlier no-op PendSV silently broke
 * that path. The ISR itself is deferred to scheduler context
 * by _xt_isr_handler like every level-1 handler. */
extern int MacIsrSigPostDefHdl(void);

uint32_t g_pendsv_posts;
uint32_t g_soft_drains;

static void SoftIsrHdl(void *arg)
{
	(void)arg;
	g_soft_drains++;
	(void)MacIsrSigPostDefHdl();
	/* No yield from here; the scheduler pumps ppT. */
}

void PendSV(int req)
{
	static int soft_init;
	uint32_t en;

	if (!soft_init) {
		soft_init = 1;
		_xt_isr_attach(7 /* ETS_SOFT_INUM */, (void *)SoftIsrHdl,
			       NULL);
		__asm__ volatile("rsr %0, intenable" : "=r"(en));
		en |= 1u << 7;
		__asm__ volatile("wsr %0, intenable\n\trsync" ::"r"(en));
	}
	if (req == 1 || req == 2) {
		g_pendsv_posts++;
		__asm__ volatile("wsr %0, intset" ::"r"(1u << 7) : "memory");
	}
}

uint32_t g_isr_counts[32];
uint32_t g_isr_last_idx;
uint32_t g_isr_last_handler;
uint32_t g_isr_phase; /* 1 = in handler call, 2 = handler returned */
uint32_t g_isr_min_sp = 0xFFFFFFFFu;

/* Level-1 interrupts are deferred to scheduler context:
 * executing blob handler chains inside a live interrupt frame
 * on this coroutine substrate corrupted the interrupted
 * context (handlers call OSI code, lower INTLEVEL, and run
 * deep flash call chains while cache/PHY state is in flux).
 * The vector-side handler therefore only masks and records
 * pending bits; p9_isr_dispatch_pending() runs the real
 * handlers as ordinary C calls from the scheduler loop, where
 * a yield is just a yield. The WDEV FIQ is NMI and cannot be
 * deferred; its OSI calls are try-only (see osi_wifi.c). */
uint32_t g_isr_pending;

void __attribute__((section(".iram1.text"))) _xt_isr_handler(void)
{
	uint32_t mask = soc_get_int_mask();
	uint32_t en;
	int i;

	for (i = 0; mask != 0 && i < 32; i++) {
		uint32_t bit = 1u << i;
		if ((bit & mask) == 0 || s_isr_table[i].handler == 0)
			continue;
		g_isr_counts[i]++;
		g_isr_pending |= bit;
		soc_clear_int_mask(bit);
		mask &= ~bit;
	}
	if (g_isr_pending != 0) {
		__asm__ volatile("rsr %0, intenable" : "=r"(en));
		en &= ~g_isr_pending;
		__asm__ volatile("wsr %0, intenable\n\trsync" ::"r"(en));
	}
}

extern int p9_intlevel_held(void) __attribute__((weak));

void p9_isr_dispatch_pending(void)
{
	int i;

	if (g_isr_pending == 0)
		return;
	if (p9_intlevel_held != NULL && p9_intlevel_held())
		return; /* a proc holds intr lock / cache-off window */
	for (i = 0; i < 32; i++) {
		uint32_t bit = 1u << i;
		uint32_t en;

		if ((g_isr_pending & bit) == 0 || s_isr_table[i].handler == 0)
			continue;
		g_isr_pending &= ~bit;
		g_isr_last_idx = (uint32_t)i;
		g_isr_last_handler = (uint32_t)s_isr_table[i].handler;
		g_isr_phase = 1;
		s_isr_table[i].handler(s_isr_table[i].arg);
		g_isr_phase = 2;
		__asm__ volatile("rsr %0, intenable" : "=r"(en));
		en |= bit;
		__asm__ volatile("wsr %0, intenable\n\trsync" ::"r"(en));
	}
}
