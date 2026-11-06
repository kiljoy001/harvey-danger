/* osi_wifi - Wi-Fi blob OS-adapter surface over p9 primitives.
 *
 * The RTOS SDK's esp_wifi blobs call these exact __wifi_*
 * symbols (components/esp8266/source/esp_wifi_os_adapter.c in
 * the RTOS tree is the FreeRTOS version). Here they are
 * implemented over firmware/kernel/p9.c, so the same blobs can
 * run on the Plan 9 proc layer (Phase B, doc/PLAN9-OS.md).
 * Signatures match the RTOS adapter, including its ticks and
 * OSI_FUNCS_TIME_BLOCKING / queue-position constants.
 */
#ifndef OSI_WIFI_H
#define OSI_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#define OSI_FUNCS_TIME_BLOCKING 0xFFFFFFFFu
#define OSI_QUEUE_SEND_FRONT 0
#define OSI_QUEUE_SEND_BACK 1
#define OSI_QUEUE_SEND_OVERWRITE 2

void *__wifi_task_create(void *task_func, const char *name,
			 uint32_t stack_depth, void *param, uint32_t prio);
void __wifi_task_delete(void *task);
void __wifi_task_yield_from_isr(void);
void __wifi_task_delay(uint32_t tick);
uint32_t __wifi_task_get_max_priority(void);
uint32_t __wifi_task_ms_to_ticks(uint32_t ms);
void __wifi_task_suspend_all(void);
void __wifi_task_resume_all(void);
void *__wifi_task_top_sp(void);

void *__wifi_queue_create(uint32_t queue_len, uint32_t item_size);
void __wifi_queue_delete(void *queue);
int __wifi_queue_send(void *queue, void *item, uint32_t block_time_tick,
		      uint32_t pos);
int __wifi_queue_send_from_isr(void *queue, void *item, int *hptw);
int __wifi_queue_recv(void *queue, void *item, uint32_t block_time_tick);
uint32_t __wifi_queue_msg_num(void *queue);

void *__wifi_semphr_create(uint32_t max, uint32_t init);
void __wifi_semphr_delete(void *semphr);
int32_t __wifi_semphr_take(void *semphr, uint32_t block_time_tick);
int32_t __wifi_semphr_give(void *semphr);

void *__wifi_timer_create(const char *name, uint32_t period_ticks,
			  bool auto_load, void *arg, void (*cb)(void *timer));
int __wifi_timer_reset(void *timer, uint32_t ticks);
int __wifi_timer_stop(void *timer, uint32_t ticks);

#endif
