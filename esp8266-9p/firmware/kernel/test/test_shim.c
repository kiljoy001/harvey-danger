/* Host tests for the FreeRTOS shim over the p9 substrate. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "freertos_shim.h"
#include "p9.h"

static QueueHandle_t q;
static SemaphoreHandle_t sem;
static EventGroupHandle_t eg;
static volatile int producer_done;
static volatile int timer_fired;

static void
producer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 10; i++)
		assert(xQueueGenericSend(q, &i, shimBLOCK_FOREVER, 1) ==
		       shimPASS);
	producer_done = 1;
}

static void
signaller(void *arg)
{
	(void)arg;
	vTaskDelay(5);
	xEventGroupSetBits(eg, 0x4);
}

static void
timer_cb(TimerHandle_t t)
{
	(void)t;
	timer_fired = 1;
}

static volatile int tls_done, tls_ok;
static void
tls_task(void *arg)
{
	void **slot;
	(void)arg;
	vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, 3,
							(void *)0x1234, NULL);
	slot = pvTaskGetThreadLocalStorageBufferPointer(NULL, 2);
	tls_ok = pvTaskGetThreadLocalStoragePointer(NULL, 3) ==
		 (void *)0x1234 &&
		 slot != NULL &&
		 pvTaskGetThreadLocalStorageBufferPointer(NULL, 2) == slot;
	*(int *)slot = 7;
	tls_ok = tls_ok &&
		 *(int *)pvTaskGetThreadLocalStorageBufferPointer(NULL, 2) == 7;
	tls_done = 1;
}

static void
blocker(void *arg)
{
	(void)arg;
	for (;;)
		p9_delay_ms(10);
}

int
main(void)
{
	int i, v;

	q = xQueueGenericCreate(4, sizeof(int), 0);
	assert(q != NULL);
	assert(xTaskCreate(producer, "prod", 1024, NULL, 1, NULL) ==
	       shimPASS);
	for (i = 0; i < 10; i++) {
		assert(xQueueReceive(q, &v, shimBLOCK_FOREVER) == shimPASS);
		assert(v == i);
	}
	while (!producer_done)
		p9_yield();
	puts("pass shim-queue-tasks");

	sem = xQueueCreateCountingSemaphore(2, 2);
	assert(sem != NULL);
	assert(xQueueSemaphoreTake(sem, 0) == shimPASS);
	assert(xQueueSemaphoreTake(sem, 0) == shimPASS);
	assert(xQueueSemaphoreTake(sem, 0) == shimFAIL);
	assert(xQueueGenericSend(sem, &(uint8_t){1}, 0, 1) == shimPASS);
	assert(xQueueSemaphoreTake(sem, 0) == shimPASS);
	puts("pass shim-counting-sem");

	eg = xEventGroupCreate();
	assert(eg != NULL);
	assert(xTaskCreate(signaller, "sig", 1024, NULL, 1, NULL) ==
	       shimPASS);
	assert((xEventGroupWaitBits(eg, 0x4, 1, 0, shimBLOCK_FOREVER) &
		0x4) != 0);
	puts("pass shim-event-group");

	{
		TimerHandle_t t =
			xTimerCreate("t", 5, 0, NULL, timer_cb);
		assert(t != NULL);
		assert(xTimerGenericCommand(t, 0, 0, NULL, 0) == shimPASS);
	}
	while (!timer_fired)
		p9_yield();
	puts("pass shim-timer");

	{
		StreamBufferHandle_t b = xStreamBufferGenericCreate(16, 1);
		char out[5] = {0};
		assert(b != NULL);
		assert(xStreamBufferSend(b, "hell", 4, 0) == 4);
		assert(xStreamBufferBytesAvailable(b) == 4);
		assert(xStreamBufferReceive(b, out, 4, 0) == 4);
		assert(strcmp(out, "hell") == 0);
		vStreamBufferDelete(b);
	}
	puts("pass shim-stream");

	{
		EventGroupHandle_t g2 = xEventGroupCreate();
		(void)g2;
		/* notify self-poll path: no pending notification */
		assert(ulTaskNotifyTake(1, 0) == 0);
	}
	puts("pass shim-notify-empty");

	{
		/* TLS errno contract (B19/B20): the SDK newlib
		 * __errno() writes through
		 * pvTaskGetThreadLocalStorageBufferPointer(NULL, idx)
		 * and stores errno in the slot itself, so the shim
		 * must return the slot's address, never NULL -
		 * including with no current task. */
		void **slot = pvTaskGetThreadLocalStorageBufferPointer(NULL, 2);
		assert(slot != NULL);
		assert(pvTaskGetThreadLocalStorageBufferPointer(NULL, 2) ==
		       slot);
		*(int *)slot = 42;
		assert(*(int *)pvTaskGetThreadLocalStorageBufferPointer(NULL,
									2) ==
		       42);
		assert(pvTaskGetThreadLocalStorageBufferPointer(NULL, 99) ==
		       NULL);
	}
	{
		/* Inside a proc, TLS slots are per-task. */
		assert(xTaskCreate(tls_task, "tls", 1024, NULL, 1, NULL) ==
		       shimPASS);
		while (!tls_done)
			p9_yield();
		assert(tls_ok);
	}
	puts("pass shim-tls-errno");

	{
		/* Proc-creation failure must be a distinct,
		 * checkable shimFAIL - never shimPASS and never
		 * a silent success (B26: srv9p silently never
		 * started when the proc table filled, because
		 * the caller had no signal worth checking).
		 * Fill the substrate with blocked procs; the
		 * next create must fail loudly by return code. */
		int created = 0;
		BaseType_t rc = shimPASS;
		while (created < 64) {
			rc = xTaskCreate(blocker, "blk", 1024, NULL, 1,
					 NULL);
			if (rc != shimPASS)
				break;
			created++;
		}
		assert(created >= 1);
		assert(rc == shimFAIL);
	}
	puts("pass shim-task-exhaustion");

	puts("test_shim: all tests passed");
	return 0;
}
