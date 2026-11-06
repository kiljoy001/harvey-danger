/* rtos_mem - /mem backing on the RTOS substrate.
 * A FreeRTOS task samples the shared memwatch ring every 100 ms:
 * esp heap free, the 9P server arena used/high, event marker
 * (0 idle, 1 client connected). Rendering happens on read. */
#ifndef RTOS_MEM_H
#define RTOS_MEM_H

#include <stdint.h>

#include "pp_arena.h"

void esp9_mem_init(pp_arena *arena);
void esp9_mem_start(pp_arena *arena);
void esp9_mem_event(uint32_t ev);
void esp9_mem_sample_at(uint32_t tick_ms, uint32_t free_heap);
const char *esp9_mem_read(uint32_t *len);

#endif
