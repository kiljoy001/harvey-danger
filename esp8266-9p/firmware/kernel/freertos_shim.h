/* freertos_shim - the FreeRTOS API subset that the RTOS SDK
 * components actually consume (77 symbols, B18 census), run on
 * the Plan 9 substrate (p9 procs, P9MsgQ, P9QLock) instead of
 * the FreeRTOS kernel. Handles are opaque pointers; ticks are
 * the substrate's 10 ms ticks. This is what lets the RTOS
 * esp_wifi layer and the blobs run blob-free: every layer
 * above stays RTOS-generation, only the kernel changes.
 */
#ifndef FREERTOS_SHIM_H
#define FREERTOS_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TimerHandle_t;
typedef void *EventGroupHandle_t;
typedef void *StreamBufferHandle_t;
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint32_t EventBits_t;

#define shimPASS 1
#define shimFAIL 0
#define shimBLOCK_FOREVER 0xFFFFFFFFu

/* tasks */
BaseType_t xTaskCreate(void (*fn)(void *), const char *name,
		       uint32_t stack_depth, void *arg, UBaseType_t prio,
		       TaskHandle_t *out);
void vTaskDelete(TaskHandle_t h);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t *last, TickType_t inc);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
BaseType_t xTaskGetSchedulerState(void);
void vTaskStartScheduler(void);
void vTaskSuspend(TaskHandle_t h);
void vTaskResume(TaskHandle_t h);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
UBaseType_t uxTaskPriorityGet(TaskHandle_t h);
void vTaskPrioritySet(TaskHandle_t h, UBaseType_t prio);
const char *pcTaskGetName(TaskHandle_t h);

/* queues and semaphores (one implementation, tagged kinds) */
QueueHandle_t xQueueGenericCreate(UBaseType_t len, UBaseType_t item_size,
				  uint8_t queue_type);
BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item,
			     TickType_t ticks, BaseType_t position);
BaseType_t xQueueGenericSendFromISR(QueueHandle_t q, const void *item,
				    BaseType_t *hptw);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *item,
				BaseType_t *hptw);
BaseType_t xQueueSemaphoreTake(QueueHandle_t q, TickType_t ticks);
QueueHandle_t xQueueCreateCountingSemaphore(UBaseType_t max,
					    UBaseType_t initial);
QueueHandle_t xQueueCreateMutex(uint8_t type);
BaseType_t xQueueGiveMutexRecursive(QueueHandle_t q);
BaseType_t xQueueTakeMutexRecursive(QueueHandle_t q, TickType_t ticks);
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q);
BaseType_t xQueueGiveFromISR(QueueHandle_t q, BaseType_t *hptw);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t xQueueIsQueueFullFromISR(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);

/* event groups */
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t want,
				BaseType_t clear_on_exit,
				BaseType_t wait_all, TickType_t ticks);
void vEventGroupDelete(EventGroupHandle_t g);
void vEventGroupSetBitsCallback(EventGroupHandle_t g, EventBits_t bits);

/* timers */
TimerHandle_t xTimerCreate(const char *name, TickType_t period,
			   UBaseType_t auto_reload, void *id,
			   void (*cb)(TimerHandle_t));
BaseType_t xTimerGenericCommand(TimerHandle_t t, BaseType_t cmd,
				TickType_t value, BaseType_t *hptw,
				TickType_t ticks);
void *pvTimerGetTimerID(TimerHandle_t t);
TaskHandle_t xTimerGetTimerDaemonTaskHandle(void);
BaseType_t xTimerPendFunctionCallFromISR(void (*fn)(void *, uint32_t),
					 void *arg1, uint32_t arg2,
					 BaseType_t *hptw);

/* stream buffers */
StreamBufferHandle_t xStreamBufferGenericCreate(size_t size,
						size_t trigger);
size_t xStreamBufferSend(StreamBufferHandle_t b, const void *data,
			 size_t len, TickType_t ticks);
size_t xStreamBufferReceive(StreamBufferHandle_t b, void *data,
			    size_t len, TickType_t ticks);
size_t xStreamBufferSendFromISR(StreamBufferHandle_t b, const void *data,
				size_t len, BaseType_t *hptw);
size_t xStreamBufferReceiveFromISR(StreamBufferHandle_t b, void *data,
				   size_t len, BaseType_t *hptw);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t b);
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t b);
void vStreamBufferDelete(StreamBufferHandle_t b);

/* notifications and TLS */
BaseType_t xTaskGenericNotify(TaskHandle_t h, uint32_t value,
			      BaseType_t action, uint32_t *prev);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
BaseType_t xTaskNotifyWait(uint32_t clear_in, uint32_t clear_out,
			   uint32_t *value, TickType_t ticks);
void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t h, BaseType_t idx);
void vTaskSetThreadLocalStoragePointerAndDelCallback(
	TaskHandle_t h, BaseType_t idx, void *ptr, void *cb);
void **pvTaskGetThreadLocalStorageBufferPointer(TaskHandle_t h, BaseType_t idx);

#endif
