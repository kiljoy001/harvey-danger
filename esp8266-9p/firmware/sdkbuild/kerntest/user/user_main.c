/* esp9 Phase C kernel exit test (NOT the plan9ized OS).
 *
 * Runs the p9 primitives + OSI surface on the Xtensa substrate
 * (firmware/kernel/p9_os_xtensa.c) and prints KERNEL-TEST lines
 * over UART0 for the CP2102 log. Behind stock blob init.
 */
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"

#include "p9.h"
#include "osi_wifi.h"
#include "memwatch.h"
#include "p9_os_xtensa.h"
#include "pp_arena.h"

extern void *pvPortMalloc(size_t sz, const char *file, int line);
extern void vPortFree(void *p, const char *file, int line);
extern int os_printf_plus(const char *fmt, ...);
extern void uart_div_modify(uint8 uart_no, uint32 div);
extern void *ets_memset(void *dst, int c, unsigned int n);
extern char *ets_strcpy(char *dst, const char *src);

static int failures;

static void ICACHE_FLASH_ATTR
check(int cond, const char *name)
{
	if (cond)
		os_printf("KERNEL-TEST pass %s\n", name);
	else {
		os_printf("KERNEL-TEST FAIL %s\n", name);
		failures++;
	}
}

/* msgq producer/consumer */
static P9MsgQ q1;
static uint8_t q1storage[8 * sizeof(int)];
static volatile int consumer_sum;
static volatile int consumer_done;

static void ICACHE_FLASH_ATTR
consumer(void *arg)
{
	int i, v;
	(void)arg;
	for (i = 0; i < 20; i++) {
		if (p9_msgq_recv(&q1, &v, P9_BLOCKING) != 1 || v != i) {
			os_printf("KERNEL-TEST FAIL consumer-seq %d\n", i);
			failures++;
			return;
		}
		consumer_sum += v;
	}
	consumer_done = 1;
}

/* qlock order */
static P9QLock ql;
static int order[4];
static volatile int ordern;

static void ICACHE_FLASH_ATTR
locker(void *arg)
{
	int id = (int)arg;
	p9_qlock(&ql);
	order[ordern++] = id;
	p9_delay_ms(15);
	p9_qunlock(&ql);
}

/* OSI timer */
static volatile int timer_fired;

static void ICACHE_FLASH_ATTR
timer_cb(void *arg)
{
	(void)arg;
	timer_fired = 1;
}

/* ---- memwatch: memory capture in action ---- */
static pp_arena mw_arena;
static uint8_t mw_arenamem[2048];
static volatile int mw_stop;
static volatile int mw_done;
static volatile uint32_t mw_event;

static uint32_t ICACHE_FLASH_ATTR
stack_max_used(void)
{
	uint32_t i, m = 0;

	for (i = 0; i < p9_os_stack_slots(); i++) {
		uint32_t s = p9_os_stack_highwater(i);
		if (s > m)
			m = s;
	}
	return m;
}

static void ICACHE_FLASH_ATTR
memwatch_proc(void *arg)
{
	(void)arg;
	while (!mw_stop) {
		MemwatchSample s;
		memset(&s, 0, sizeof(s));
		s.tick_ms = (uint32_t)p9_os_now_ms();
		s.event = mw_event;
		s.free_heap = system_get_free_heap_size();
		s.arena_used = mw_arena.used;
		s.arena_high = mw_arena.high;
		s.q_count = p9_msgq_num(&q1);
		s.stack_used = stack_max_used();
		memwatch_push(&s);
		p9_delay_ms(25);
	}
	mw_done = 1;
}

static void ICACHE_FLASH_ATTR
run_memwatch(void)
{
	MemwatchSample s0, slast;
	uint32_t i, n, baseline = 0, mark;
	void *blk, *abuf;
	int v;

	pp_arena_init(&mw_arena, mw_arenamem, sizeof(mw_arenamem));
	memwatch_reset();
	mw_event = 0;
	mw_stop = 0;
	mw_done = 0;
	check(p9_proc_create("memwatch", memwatch_proc, NULL) != NULL,
	      "memwatch-create");
	p9_delay_ms(120);
	if (memwatch_get(0, &s0))
		baseline = s0.free_heap;

	/* In-action load: 2048 bytes of real heap, 1024 of arena,
	 * and 3 queued messages. The sampler must see all three. */
	blk = pvPortMalloc(2048, "", 0);
	check(blk != NULL, "memwatch-heap-alloc");
	mark = pp_arena_mark(&mw_arena);
	abuf = pp_arena_alloc(&mw_arena, 1024, 8);
	check(abuf != NULL, "memwatch-arena-alloc");
	for (v = 0; v < 3; v++)
		p9_msgq_send(&q1, &v, 0, 1);
	mw_event = 1;
	p9_delay_ms(200);

	mw_event = 2;
	if (blk != NULL)
		vPortFree(blk, "", 0);
	pp_arena_release(&mw_arena, mark);
	while (p9_msgq_recv(&q1, &v, 0) == 1) {
	}
	p9_delay_ms(120);
	mw_stop = 1;
	while (!mw_done)
		p9_delay_ms(5);

	n = memwatch_count();
	for (i = 0; i < n; i++) {
		MemwatchSample s;
		if (memwatch_get(i, &s))
			os_printf("MEMWATCH seq=%u tick=%u ev=%u free=%u "
				  "arena=%u/%u q=%u stack=%u\n",
				  s.seq, s.tick_ms, s.event, s.free_heap,
				  s.arena_used, s.arena_high, s.q_count,
				  s.stack_used);
	}
	check(n >= 8, "memwatch-samples");
	check(baseline > 1500 &&
		      memwatch_min_free() <= baseline - 1500,
	      "memwatch-heap-dip");
	check(memwatch_max_arena_used() >= 1024, "memwatch-arena-peak");
	check(memwatch_max_stack_used() > 0 &&
		      memwatch_max_stack_used() <= 4096,
	      "memwatch-stack-peak");
	if (memwatch_get(n - 1, &slast))
		check(slast.free_heap >= memwatch_min_free() + 1000,
		      "memwatch-heap-recover");

	{
		P9XtRegs regs;
		p9_os_capture_regs(&regs);
		os_printf("REGDUMP pc=%08x sp=%08x ps=%08x exccause=%u "
			  "excvaddr=%08x epc1=%08x intenable=%08x "
			  "interrupt=%08x ccount=%u sar=%u vecbase=%08x "
			  "prid=%08x\n",
			  regs.pc, regs.sp, regs.ps, regs.exccause,
			  regs.excvaddr, regs.epc1, regs.intenable,
			  regs.interrupt, regs.ccount, regs.sar,
			  regs.vecbase, regs.prid);
		check(regs.sp > 0x3ffe0000u && regs.sp < 0x40000000u,
		      "regdump-sp-dram");
		check(regs.pc >= 0x40200000u && regs.pc < 0x40300000u,
		      "regdump-pc-irom");
		check(regs.ccount != 0, "regdump-ccount");
	}
}

static void ICACHE_FLASH_ATTR
run_tests(void)
{
	int i, ok;

	p9_msgq_init(&q1, q1storage, 8, sizeof(int));
	check(p9_proc_create("consumer", consumer, NULL) != NULL, "proc-create");
	for (i = 0; i < 20; i++)
		p9_msgq_send(&q1, &i, P9_BLOCKING, 1);
	while (!consumer_done)
		p9_delay_ms(5);
	check(consumer_sum == 190, "msgq-procs");

	p9_qlock_init(&ql);
	for (i = 0; i < 4; i++) {
		p9_proc_create("locker", locker, (void *)i);
		p9_delay_ms(20);
	}
	while (ordern < 4)
		p9_delay_ms(5);
	ok = 1;
	for (i = 0; i < 4; i++)
		if (order[i] != i)
			ok = 0;
	check(ok, "qlock-fifo");
	check(p9_canqlock(&ql), "canqlock");
	if (ql.locked)
		p9_qunlock(&ql);

	{
		void *s = __wifi_semphr_create(2, 1);
		check(s != NULL, "sem-create");
		check(__wifi_semphr_take(s, 0) == 1, "sem-take");
		check(__wifi_semphr_take(s, 0) == 0, "sem-timeout");
		check(__wifi_semphr_give(s) == 1, "sem-give");
		check(__wifi_semphr_take(s, 5) == 1, "sem-retake");
		__wifi_semphr_delete(s);
	}
	{
		void *tm = __wifi_timer_create("t1", 5, false, NULL, timer_cb);
		int waited = 0;
		check(tm != NULL, "timer-create");
		while (!timer_fired && waited < 2000) {
			p9_delay_ms(10);
			waited += 10;
		}
		check(timer_fired, "timer-fired");
	}
	{
		void *q = __wifi_queue_create(2, sizeof(int));
		int v, out = 0;
		check(q != NULL, "osi-queue-create");
		v = 1;
		__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK);
		v = 2;
		__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_BACK);
		v = 9;
		check(__wifi_queue_send(q, &v, 0, OSI_QUEUE_SEND_OVERWRITE) == 1,
		      "osi-overwrite");
		__wifi_queue_recv(q, &out, 0);
		check(out == 1, "osi-order1");
		__wifi_queue_recv(q, &out, 0);
		check(out == 9, "osi-order2");
		__wifi_queue_delete(q);
	}
	check(__wifi_task_get_max_priority() == 15, "osi-max-priority");
	{
		void *q = __wifi_queue_create(75, 8);
		uint64_t v = 42, out = 0;
		int hptw = 0;
		check(q != NULL, "osi-queue-75x8");
		if (q != NULL) {
			check(__wifi_queue_send_from_isr(q, &v, &hptw) == 1 &&
				      hptw == 1,
			      "osi-from-isr-hptw");
			__wifi_queue_recv(q, &out, 0);
			check(out == 42, "osi-queue-75-recv");
			__wifi_queue_delete(q);
		}
	}

	run_memwatch();

	if (failures == 0)
		os_printf("KERNEL-TEST ALL-PASS heap=%u\n",
			  system_get_free_heap_size());
	else
		os_printf("KERNEL-TEST FAILURES=%d\n", failures);
}

void ICACHE_FLASH_ATTR
user_init(void)
{
	uart_div_modify(0, UART_CLK_FREQ / 115200);
	os_printf("\nKERNEL-TEST boot sdk=%s\n", system_get_sdk_version());
	run_tests();
}

uint32 ICACHE_FLASH_ATTR
user_rf_cal_sector_set(void)
{
	enum flash_size_map size_map = system_get_flash_size_map();
	uint32 rf_cal_sec = 0;

	switch (size_map) {
	case FLASH_SIZE_4M_MAP_256_256:
		rf_cal_sec = 128 - 5;
		break;
	case FLASH_SIZE_8M_MAP_512_512:
		rf_cal_sec = 256 - 5;
		break;
	case FLASH_SIZE_16M_MAP_512_512:
	case FLASH_SIZE_16M_MAP_1024_1024:
		rf_cal_sec = 512 - 5;
		break;
	case FLASH_SIZE_32M_MAP_512_512:
	case FLASH_SIZE_32M_MAP_1024_1024:
		rf_cal_sec = 1024 - 5;
		break;
	default:
		rf_cal_sec = 0;
		break;
	}
	return rf_cal_sec;
}
